#include "write_watch.h"
#include "memory.h"
#include "log.h"
#include "arch.h"

#include <sys/mman.h>
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr uint32_t kShift = 16;
constexpr uint32_t kPhysSize = 0x20000000;
constexpr uint32_t kGranules = kPhysSize >> kShift;
// Several cache entries may watch the same memory, so a granule keeps the sequence number of
// its last write and each caller the number of its last check (no "dirty" flag to consume).
// write protection: 0 none, 1 protected, 2 being unprotected, 3 being protected. mprotect runs
// outside the lock: on a phone it can take 100 ms (the kernel's address-space lock), and every
// thread that needed the lock meanwhile stalled - the game's sound mixing and the audio decoder
// among them (sounds cut). A granule in 2 or 3 is left alone until its thread is done.
std::atomic<uint8_t> s_prot[kGranules];
std::atomic<uint32_t> s_written[kGranules];    // sequence number of the last (possible) write
std::atomic<uint32_t> s_seq{ 1 };
uint8_t s_hostBusy[kGranules];                 // syscalls writing into it (s_lock)
uint8_t s_cpuDirty[kGranules];                 // CPU stores since the last GPU cache flush (s_lock)
uint16_t s_gpuOwned[kGranules];                // copy mode: GPU writes of these granules not copied back yet (s_lock)
uint8_t* s_gpuCopy = nullptr;                  // copy mode: the GPU's copy of guest physical memory
void (*s_copyFlush)(uint32_t, uint32_t) = nullptr;
uint64_t s_copyStats = 0;                      // copy mode: bytes uploaded (s_lock)
bool s_gpuFlush = false;
bool s_flushAll = false;                       // BO3_FLUSH_ALL: clean every written granule at each submit
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

// protections to apply once the lock is released (declare before the SpinLock: destroyed after it)
struct PendingProtect
{
    std::vector<std::pair<uint32_t, uint32_t>>& runs;
    PendingProtect() : runs(Runs()) { runs.clear(); }
    static std::vector<std::pair<uint32_t, uint32_t>>& Runs() { static thread_local std::vector<std::pair<uint32_t, uint32_t>> v; return v; }
    void Add(uint32_t g0, uint32_t g1) { runs.emplace_back(g0, g1); }   // (lock held, the granules set to 3)
    ~PendingProtect()
    {
        for (auto& r : runs)
        {
            Protect(r.first, r.second, PROT_READ);
            for (uint32_t g = r.first; g < r.second; g++) s_prot[g].store(1, std::memory_order_release);
        }
        runs.clear();
    }
};

void MarkWritten(uint32_t g)
{
    s_written[g].store(s_seq.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    s_cpuDirty[g] = 1;
}

// unprotections to apply once the lock is released (declare before the SpinLock: destroyed after it)
struct PendingUnprotect
{
    std::vector<uint32_t>& gs;
    PendingUnprotect() : gs(List()) { gs.clear(); }
    static std::vector<uint32_t>& List() { static thread_local std::vector<uint32_t> v; return v; }
    ~PendingUnprotect()
    {
        for (uint32_t g : gs)
        {
            Protect(g, g + 1, PROT_READ | PROT_WRITE);
            s_prot[g].store(0, std::memory_order_release);
        }
        gs.clear();
    }
};

// (lock held) a write happened or is about to: unprotect - after the lock, before the caller returns
void Written(uint32_t g, PendingUnprotect& pending)
{
    MarkWritten(g);
    while (s_prot[g].load(std::memory_order_acquire) >= 2) CpuRelax();   // (its thread finishes without the lock)
    if (s_prot[g].load(std::memory_order_relaxed))
    {
        s_prot[g].store(2, std::memory_order_relaxed);
        pending.gs.push_back(g);
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
        clean = s_prot[g].load(std::memory_order_acquire) == 1 && int32_t(s_written[g].load(std::memory_order_relaxed) - stamp) < 0;
    return clean;
}

bool WatchCheckAndArm(uint32_t phys, uint32_t size, uint32_t& stamp)
{
    if (WatchClean(phys, size, stamp)) return true;
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    PendingProtect pending;   // (the caller reads the memory after the protection is in place)
    SpinLock lock;
    // Taken before protecting: any store that misses the protection lands before the caller
    // reads the memory, any later one faults and gets a higher number.
    stamp = s_seq.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t run = UINT32_MAX;
    for (uint32_t g = g0; g <= g1; g++)
    {
        bool needs = g < g1 && !s_prot[g].load(std::memory_order_relaxed) && !s_hostBusy[g];
        if (needs) s_prot[g].store(3, std::memory_order_relaxed);
        if (needs && run == UINT32_MAX) run = g;
        if (!needs && run != UINT32_MAX) { pending.Add(run, g); run = UINT32_MAX; }
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

void WatchHostWriteNow(uint32_t phys, uint32_t size)
{
    if (!s_on) return;
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    // nothing protected (the usual case): no lock - it may be held for a slow mprotect
    bool prot = false;
    for (uint32_t g = g0; g < g1; g++) prot |= s_prot[g].load(std::memory_order_acquire) != 0;
    if (!prot)
    {
        for (uint32_t g = g0; g < g1; g++) MarkWritten(g);
        return;
    }
    PendingUnprotect pending;
    SpinLock lock;
    for (uint32_t g = g0; g < g1; g++) Written(g, pending);
}

void WatchBeginHostWrite(uint32_t guest, uint32_t size)
{
    if (!s_on || !size) return;
    uint32_t phys;
    if (!ToPhys(uintptr_t(g_base) + guest, phys)) return;
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    PendingUnprotect pending;
    SpinLock lock;
    for (uint32_t g = g0; g < g1; g++) { s_hostBusy[g]++; Written(g, pending); }
}

void WatchEndHostWrite(uint32_t guest, uint32_t size)
{
    if (!s_on || !size) return;
    uint32_t phys;
    if (!ToPhys(uintptr_t(g_base) + guest, phys)) return;
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    PendingUnprotect pending;
    SpinLock lock;
    for (uint32_t g = g0; g < g1; g++) { s_hostBusy[g]--; Written(g, pending); }
}

bool WatchHandleFault(void* addr)
{
    uint32_t phys;
    if (!s_on || !ToPhys(uintptr_t(addr), phys)) return false;
    uint32_t g = phys >> kShift;
    bool unprotect;
    {
        SpinLock lock;
        // Not protected (any more), or another thread is unprotecting it: retry the store - unless
        // it was never watched, then this is a real crash.
        uint8_t st = s_prot[g].load(std::memory_order_relaxed);
        if (st != 1) return st >= 2 || s_written[g].load(std::memory_order_relaxed) != 0;   // (2 / 3: retry until done)
        MarkWritten(g);
        s_prot[g].store(2, std::memory_order_relaxed);
        unprotect = true;
    }
    if (unprotect)
    {
        Protect(g, g + 1, PROT_READ | PROT_WRITE);
        s_prot[g].store(0, std::memory_order_release);
    }
    return true;
}

// Data cache clean to the point of coherency (what the GPU reads), by virtual address.
static void CleanDcache(uint8_t* p, size_t n)
{
#if defined(__aarch64__)
    uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = uintptr_t(4) << ((ctr >> 16) & 15);
    uintptr_t a = uintptr_t(p) & ~(line - 1), e = uintptr_t(p) + n;
    for (; a < e; a += line) asm volatile("dc cvac, %0" ::"r"(a) : "memory");
    asm volatile("dsb ish" ::: "memory");
#else
    (void)p; (void)n;
#endif
}

void WatchEnableGpuFlush()
{
    if (!s_on || getenv("BO3_NO_GPU_FLUSH")) return;
    SpinLock lock;
    for (uint32_t g = 0; g < kGranules; g++) s_cpuDirty[g] = 1;   // everything written so far
    s_gpuFlush = true;
    s_flushAll = getenv("BO3_FLUSH_ALL") != nullptr;
    Log("memory: the GPU does not snoop the CPU caches - %s", s_flushAll ? "written guest memory is cleaned before each submission"
                                                                          : "what the GPU reads is cleaned from the CPU caches first");
}

bool WatchNeedsRequests() { return s_gpuCopy || (s_gpuFlush && !s_flushAll); }

bool WatchAnyDirty(uint32_t phys, uint32_t size)
{
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    for (uint32_t g = g0; g < g1; g++)
        if (s_cpuDirty[g]) return true;
    return false;
}

void WatchRequestUpload(uint32_t phys, uint32_t size)
{
    if (!size || !WatchNeedsRequests()) return;
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    PendingProtect pending;
    SpinLock lock;
    uint32_t prun = UINT32_MAX;
    for (uint32_t g = g0; g <= g1; g++)
    {
        bool up = g < g1 && s_cpuDirty[g] && !s_gpuOwned[g] && !s_hostBusy[g] && s_prot[g].load(std::memory_order_relaxed) <= 1;
        if (up)
        {
            uint32_t off = g << kShift;
            if (s_gpuCopy)
            {
                memcpy(s_gpuCopy + off, g_base + 0xA0000000u + off, 1u << kShift);
                if (s_copyFlush) s_copyFlush(off, 1u << kShift);
            }
            else CleanDcache(g_base + 0xA0000000u + off, 1u << kShift);   // (non-snooping GPU: clean on request)
            s_cpuDirty[g] = 0;
            s_copyStats += 1u << kShift;
        }
        // protect what went up (the next store marks it again)
        bool arm = up && !s_prot[g].load(std::memory_order_relaxed);
        if (arm) s_prot[g].store(3, std::memory_order_relaxed);
        if (arm && prun == UINT32_MAX) prun = g;
        if (!arm && prun != UINT32_MAX) { pending.Add(prun, g); prun = UINT32_MAX; }
    }
}

void WatchFlushForGpu()
{
    if (!s_gpuFlush || s_gpuCopy || !s_flushAll) return;   // (copy mode / default: on request, WatchRequestUpload)
    static uint32_t runs[kGranules * 2];
    uint32_t nRuns = 0;
    {
        PendingProtect pending;
        SpinLock lock;
        // protect first (later stores fault and mark the granule again), clean after
        uint32_t run = UINT32_MAX, prun = UINT32_MAX;
        for (uint32_t g = 0; g <= kGranules; g++)
        {
            bool dirty = g < kGranules && s_cpuDirty[g] && !s_gpuOwned[g] && s_prot[g].load(std::memory_order_relaxed) <= 1;   // (copy mode: GPU writes pending / mprotect in progress - later)
            if (dirty && !s_hostBusy[g]) s_cpuDirty[g] = 0;   // (a syscall still writing: again next time)
            bool arm = dirty && !s_hostBusy[g] && !s_prot[g].load(std::memory_order_relaxed);
            if (arm) s_prot[g].store(3, std::memory_order_relaxed);
            if (arm && prun == UINT32_MAX) prun = g;
            if (!arm && prun != UINT32_MAX) { pending.Add(prun, g); prun = UINT32_MAX; }
            if (dirty && run == UINT32_MAX) run = g;
            if (!dirty && run != UINT32_MAX) { runs[nRuns++] = run; runs[nRuns++] = g; run = UINT32_MAX; }
        }
    }
    for (uint32_t i = 0; i < nRuns; i += 2)
    {
        uint32_t off = runs[i] << kShift, n = (runs[i + 1] - runs[i]) << kShift;
        if (s_gpuCopy)
        {
            memcpy(s_gpuCopy + off, g_base + 0xA0000000u + off, n);
            if (s_copyFlush) s_copyFlush(off, n);
        }
        else CleanDcache(g_base + 0xA0000000u + off, n);
    }
}

void WatchEnableCopyMode(uint8_t* gpu, void (*flush)(uint32_t, uint32_t))
{
    if (!s_on) return;
    SpinLock lock;
    for (uint32_t g = 0; g < kGranules; g++) s_cpuDirty[g] = 1;   // everything goes up once
    s_gpuCopy = gpu;
    s_copyFlush = flush;
    s_gpuFlush = true;
    Log("memory: copy mode - the GPU has its own copy of guest memory, the CPU's writes are copied before each submission");
}

bool WatchCopyMode() { return s_gpuCopy != nullptr; }
uint64_t WatchCopyPeekStats() { return s_copyStats; }
uint64_t WatchCopyTakeStats() { SpinLock lock; uint64_t b = s_copyStats; s_copyStats = 0; return b; }

std::shared_ptr<std::vector<uint32_t>> WatchGpuWriteBegin(uint32_t phys, uint32_t size)
{
    if (!s_gpuCopy) return nullptr;
    phys &= kPhysSize - 1;
    phys &= ~3u;
    size = std::min((size + 3) & ~3u, kPhysSize - phys);
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    SpinLock lock;
    for (uint32_t g = g0; g < g1; g++)
    {
        // up now (unless the GPU already owns it: then the CPU side is not the newest)
        if (!s_gpuOwned[g] && s_cpuDirty[g] && !s_hostBusy[g])
        {
            uint32_t off = g << kShift;
            memcpy(s_gpuCopy + off, g_base + 0xA0000000u + off, 1u << kShift);
            if (s_copyFlush) s_copyFlush(off, 1u << kShift);
            s_cpuDirty[g] = 0;
        }
        s_gpuOwned[g]++;
    }
    auto snap = std::make_shared<std::vector<uint32_t>>(size / 4);
    memcpy(snap->data(), s_gpuCopy + phys, size);
    return snap;
}

void WatchGpuWriteDone(uint32_t phys, uint32_t size, const std::shared_ptr<std::vector<uint32_t>>& snap,
                       void (*invalidate)(uint32_t, uint32_t))
{
    if (!s_gpuCopy || !snap) return;
    phys &= kPhysSize - 1;
    phys &= ~3u;
    size = std::min((size + 3) & ~3u, kPhysSize - phys);
    size = std::min<uint32_t>(size, uint32_t(snap->size() * 4));
    if (invalidate) invalidate(phys, size);
    uint32_t g0, g1;
    Range(phys, size, g0, g1);
    const uint32_t* gpu = reinterpret_cast<const uint32_t*>(s_gpuCopy + phys);
    uint32_t* cpu = reinterpret_cast<uint32_t*>(g_base + 0xA0000000u + phys);
    const uint32_t* old = snap->data();
    bool any = false;
    for (uint32_t i = 0; i < size / 4; i++)
        if (gpu[i] != old[i])
        {
            if (!any)
            {
                // (unprotected first: these stores must not fault; they count as CPU writes and go up again)
                PendingUnprotect pending;
                SpinLock lock;
                for (uint32_t g = g0; g < g1; g++) Written(g, pending);
                any = true;
            }
            cpu[i] = gpu[i];
        }
    SpinLock lock;
    for (uint32_t g = g0; g < g1; g++) if (s_gpuOwned[g]) s_gpuOwned[g]--;
}
