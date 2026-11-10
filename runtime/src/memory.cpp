#include "memory.h"
#include "log.h"
#include "write_watch.h"
#include "arch.h"

#include <sys/mman.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

uint8_t* g_base = nullptr;

GuestHeap g_virtualHeap("virtual", 0x40000000, 0x7F000000, 0x1000);
GuestHeap g_physicalHeap("physical", 0xA0000000, 0xC0000000, 0x1000);

void MemoryInit(int physFd)
{
    // 4 GiB, lazily committed by the host kernel. MAP_NORESERVE so untouched pages cost nothing.
    void* p = mmap(nullptr, 0x100000000ull, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        Fatal("cannot reserve 4 GiB guest address space");
    g_base = (uint8_t*)p;
    // Guest null page: keep it inaccessible so null derefs crash loudly.
    mprotect(g_base, 0x10000, PROT_NONE);

    // Physical memory (512 MiB) is visible through three windows: 0xA0000000 (64 KiB pages),
    // 0xC0000000 (16 MiB pages) and 0xE0000000 (4 KiB pages, offset by 0x1000 like Xenia).
    // One shared memory object (the GPU's dma-buf, or a memfd without GPU) is mapped at all
    // three so writes through one view show in the others.
    if (physFd == kPhysExternal)
    {
        WatchInit();
        Log("memory: guest base = %p (physical views mapped by the GPU driver)", g_base);
        return;
    }
    if (physFd == kPhysNoAlias)
    {
        LogWarn("memory: physical views are not aliased (0xC0000000/0xE0000000 are separate)");
        Log("memory: guest base = %p", g_base);
        return;
    }
    int fd = physFd;
    if (fd < 0)
    {
        fd = HostMemfd("bo3-physical");
        if (fd < 0 || ftruncate(fd, 0x20000000) != 0)
            Fatal("cannot create the guest physical memory object");
    }
    auto view = [&](uint32_t guest, size_t size, off_t offset) {
        if (mmap(g_base + guest, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, offset) == MAP_FAILED)
            Fatal("cannot map the physical memory view at 0x%08X: %s", guest, strerror(errno));
    };
    view(0xA0000000, 0x20000000, 0);
    view(0xC0000000, 0x20000000, 0);
    view(0xE0000000, 0x20000000 - 0x1000, 0x1000);
    close(fd);
    WatchInit();
    Log("memory: guest base = %p, physical memory aliased at 0xA0000000/0xC0000000/0xE0000000 (%s)", g_base,
        physFd >= 0 ? "GPU dma-buf" : "memfd");
}

GuestHeap::GuestHeap(const char* name, uint32_t begin, uint32_t end, uint32_t page)
    : m_name(name), m_begin(begin), m_end(end), m_page(page)
{
    m_free[begin] = end - begin;
}

static uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

uint32_t GuestHeap::Alloc(uint32_t size, uint32_t align)
{
    if (size == 0) size = 1;
    size = AlignUp(size, m_page);
    align = std::max(align, m_page);
    std::lock_guard lock(m_mutex);
    for (auto it = m_free.begin(); it != m_free.end(); ++it)
    {
        uint32_t start = AlignUp(it->first, align);
        uint64_t end = uint64_t(start) + size;
        if (end > uint64_t(it->first) + it->second) continue;
        uint32_t fStart = it->first, fSize = it->second;
        m_free.erase(it);
        if (start > fStart) m_free[fStart] = start - fStart;
        if (end < uint64_t(fStart) + fSize) m_free[uint32_t(end)] = uint32_t(uint64_t(fStart) + fSize - end);
        m_alloc[start] = size;
        m_used += size;
        memset(g_base + start, 0, size);
        return start;
    }
    LogWarn("%s heap: out of memory (request 0x%X, used 0x%X)", m_name, size, m_used);
    return 0;
}

uint32_t GuestHeap::AllocAt(uint32_t addr, uint32_t size)
{
    size = AlignUp(size + (addr & (m_page - 1)), m_page);
    addr &= ~(m_page - 1);
    std::lock_guard lock(m_mutex);
    auto it = m_free.upper_bound(addr);
    if (it == m_free.begin()) return 0;
    --it;
    uint64_t fEnd = uint64_t(it->first) + it->second;
    if (addr < it->first || uint64_t(addr) + size > fEnd) return 0;
    uint32_t fStart = it->first;
    m_free.erase(it);
    if (addr > fStart) m_free[fStart] = addr - fStart;
    if (uint64_t(addr) + size < fEnd) m_free[addr + size] = uint32_t(fEnd - addr - size);
    m_alloc[addr] = size;
    m_used += size;
    memset(g_base + addr, 0, size);
    return addr;
}

bool GuestHeap::Free(uint32_t addr)
{
    std::lock_guard lock(m_mutex);
    auto it = m_alloc.find(addr);
    if (it == m_alloc.end()) return false;
    uint32_t start = it->first, size = it->second;
    m_alloc.erase(it);
    m_used -= size;
    // insert + coalesce
    auto nx = m_free.lower_bound(start);
    if (nx != m_free.end() && nx->first == start + size) { size += nx->second; nx = m_free.erase(nx); }
    if (nx != m_free.begin())
    {
        auto pv = std::prev(nx);
        if (pv->first + pv->second == start) { pv->second += size; return true; }
    }
    m_free[start] = size;
    return true;
}

uint32_t GuestHeap::SizeOf(uint32_t addr)
{
    std::lock_guard lock(m_mutex);
    auto it = m_alloc.find(addr);
    return it == m_alloc.end() ? 0 : it->second;
}

uint32_t GuestAlloc(uint32_t size, uint32_t align) { return g_virtualHeap.Alloc(size, align); }
void GuestFree(uint32_t addr) { g_virtualHeap.Free(addr); }
