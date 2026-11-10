#include "write_watch.h"
#include "memory.h"
#include "log.h"
#include "arch.h"

#include <sys/mman.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>

namespace
{
constexpr uint32_t kShift = 16;
constexpr uint32_t kPhysSize = 0x20000000;
constexpr uint32_t kGranules = kPhysSize >> kShift;
// Several cache entries may watch the same memory, so a granule keeps the sequence number of
// its last write and each caller the number of its last check (no "dirty" flag to consume).
std::atomic<uint8_t> s_prot[kGranules];        // write-protected
std::atomic<uint32_t> s_written[kGranules];    // sequence number of the last (possible) write
std::atomic<uint32_t> s_seq{ 1 };
uint8_t s_hostBusy[kGranules];                 // syscalls writing into it (s_lock)
std::atomic_flag s_lock = ATOMIC_FLAG_INIT;
bool s_on = false;

struct SpinLock
{
    SpinLock() { while (s_lock.test_and_set(std::memory_order_acquire)) CpuRelax(); }
    ~SpinLock() { s_lock.clear(std::memory_order_release); }
};

void Protect(uint32_t g0, uint32_t g1, int prot)
{
    uint32_t p0 = g0 << kShift, p1 = g1 << kShift;
    mprotect(g_base + 0xA0000000u + p0, p1 - p0, prot);
    mprotect(g_base + 0xC0000000u + p0, p1 - p0, prot);
    uint32_t e0 = std::max(p0, 0x1000u);   // the 0xE0000000 view starts at physical 0x1000
    if (p1 > e0) mprotect(g_base + 0xE0000000u + (e0 - 0x1000u), p1 - e0, prot);
}

bool ToPhys(uintptr_t host, uint32_t& phys)
{
    if (host < uintptr_t(g_base) + 0xA0000000u || host >= uintptr_t(g_base) + 0x100000000ull) return false;
    uint32_t a = uint32_t(host - uintptr_t(g_base));
    if (a < 0xC0000000u) phys = a - 0xA0000000u;
    else if (a < 0xE0000000u) phys = a - 0xC0000000u;
    else phys = a - 0xE0000000u + 0x1000u;
    return phys < kPhysSize;
}

void Range(uint32_t phys, uint32_t size, uint32_t& g0, uint32_t& g1)
{
    phys &= kPhysSize - 1;
    g0 = phys >> kShift;
    g1 = std::min<uint32_t>(kGranules, uint32_t((uint64_t(phys) + std::max(size, 1u) - 1) >> kShift) + 1);
}

// (lock held) a write happened or is about to: unprotect
void Written(uint32_t g)
{
    s_written[g].store(s_seq.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    if (s_prot[g].load(std::memory_order_relaxed))
    {
        Protect(g, g + 1, PROT_READ | PROT_WRITE);
        s_prot[g].store(0, std::memory_order_release);
    }
}
}

void WatchInit()
{
    s_on = getenv("BO3_NO_WRITE_WATCH") == nullptr;
    if (s_on) Log("memory: texture write tracking on (64 KiB granules)");
}

bool WatchEnabled() { return s_on; }

bool WatchClean(uint32_t phys, uint32_t size, uint32_t stamp)
{
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    bool clean = stamp != 0;
    for (uint32_t g = g0; g < g1 && clean; g++)
        clean = s_prot[g].load(std::memory_order_acquire) && int32_t(s_written[g].load(std::memory_order_relaxed) - stamp) < 0;
    return clean;
}

bool WatchCheckAndArm(uint32_t phys, uint32_t size, uint32_t& stamp)
{
    if (WatchClean(phys, size, stamp)) return true;
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    SpinLock lock;
    // Taken before protecting: any store that misses the protection lands before the caller
    // reads the memory, any later one faults and gets a higher number.
    stamp = s_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t run = UINT32_MAX;
    for (uint32_t g = g0; g <= g1; g++)
    {
        bool needs = g < g1 && !s_prot[g].load(std::memory_order_relaxed) && !s_hostBusy[g];
        if (needs) s_prot[g].store(1, std::memory_order_relaxed);
        if (needs && run == UINT32_MAX) run = g;
        if (!needs && run != UINT32_MAX) { Protect(run, g, PROT_READ); run = UINT32_MAX; }
    }
    return false;
}

void WatchMarkDirty(uint32_t phys, uint32_t size)
{
    if (!s_on) return;
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    for (uint32_t g = g0; g < g1; g++) s_written[g].store(s_seq.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void WatchBeginHostWrite(uint32_t guest, uint32_t size)
{
    if (!s_on || !size) return;
    uint32_t phys;
    if (!ToPhys(uintptr_t(g_base) + guest, phys)) return;
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    SpinLock lock;
    for (uint32_t g = g0; g < g1; g++) { s_hostBusy[g]++; Written(g); }
}

void WatchEndHostWrite(uint32_t guest, uint32_t size)
{
    if (!s_on || !size) return;
    uint32_t phys;
    if (!ToPhys(uintptr_t(g_base) + guest, phys)) return;
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    SpinLock lock;
    for (uint32_t g = g0; g < g1; g++) { s_hostBusy[g]--; Written(g); }
}

bool WatchHandleFault(void* addr)
{
    uint32_t phys;
    if (!s_on || !ToPhys(uintptr_t(addr), phys)) return false;
    uint32_t g = phys >> kShift;
    SpinLock lock;
    // Not protected (any more): another thread got there first, retry the store - unless it
    // was never watched, then this is a real crash.
    bool watched = s_prot[g].load(std::memory_order_relaxed) || s_written[g].load(std::memory_order_relaxed) != 0;
    if (s_prot[g].load(std::memory_order_relaxed)) Written(g);
    return watched;
}
