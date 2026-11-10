// xboxkrnl: virtual / physical memory.
#include "guest.h"
#include "stubs.h"

#include <atomic>

constexpr uint32_t MEM_COMMIT = 0x1000, MEM_RESERVE = 0x2000, MEM_RELEASE = 0x8000;
constexpr uint32_t MEM_LARGE_PAGES = 0x20000000, MEM_16MB_PAGES = 0x80000000;
constexpr uint32_t PAGE_READWRITE = 0x04;

// NtAllocateVirtualMemory(base_addr*, region_size*, alloc_type, protect, debug_memory)
GUEST_IMPORT(NtAllocateVirtualMemory)
{
    uint32_t basePtr = ARG(3), sizePtr = ARG(4), type = ARG(5);
    uint32_t want = GuestRead32(basePtr), size = GuestRead32(sizePtr);
    uint32_t page = (type & MEM_16MB_PAGES) ? 0x1000000 : (type & MEM_LARGE_PAGES) ? 0x10000 : 0x1000;
    size = (size + page - 1) & ~(page - 1);
    uint32_t addr;
    if (want)
    {
        // Committing inside a region we already reserved is a no-op (memory is always backed).
        if (g_virtualHeap.Contains(want) && !(type & MEM_RESERVE)) addr = want & ~(page - 1);
        else addr = g_virtualHeap.AllocAt(want, size);
    }
    else
        addr = g_virtualHeap.Alloc(size, std::max<uint32_t>(page, 0x10000));
    TRACE_IMPORT("want=0x%X size=0x%X type=0x%X -> 0x%X", want, size, type, addr);
    if (!addr) RETURN(STATUS_NO_MEMORY);
    GuestWrite32(basePtr, addr);
    GuestWrite32(sizePtr, size);
    RETURN(STATUS_SUCCESS);
}

// NtFreeVirtualMemory(base_addr*, region_size*, free_type, debug_memory)
GUEST_IMPORT(NtFreeVirtualMemory)
{
    uint32_t addr = GuestRead32(ARG(3));
    if (ARG(5) & MEM_RELEASE)
    {
        uint32_t size = g_virtualHeap.SizeOf(addr);
        if (!g_virtualHeap.Free(addr)) RETURN(STATUS_INVALID_PARAMETER);
        if (ARG(4)) GuestWrite32(ARG(4), size);
    }
    RETURN(STATUS_SUCCESS);
}

// NtQueryVirtualMemory(base_address, MEMORY_BASIC_INFORMATION*)
GUEST_IMPORT(NtQueryVirtualMemory)
{
    uint32_t a = ARG(3), info = ARG(4);
    uint32_t page = a & ~0xFFFu;
    GuestWrite32(info + 0x00, page);
    GuestWrite32(info + 0x04, page);
    GuestWrite32(info + 0x08, PAGE_READWRITE);
    GuestWrite32(info + 0x0C, 0x1000);
    GuestWrite32(info + 0x10, MEM_COMMIT);
    GuestWrite32(info + 0x14, PAGE_READWRITE);
    GuestWrite32(info + 0x18, 0x20000);   // MEM_PRIVATE
    RETURN(STATUS_SUCCESS);
}

// MmAllocatePhysicalMemoryEx(flags, size, protect, min_address, max_address, alignment)
GUEST_IMPORT(MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ARG(4), protect = ARG(5), align = ARG(8);
    uint32_t page = (protect & MEM_16MB_PAGES) ? 0x1000000 : (protect & MEM_LARGE_PAGES) ? 0x10000 : 0x1000;
    uint32_t addr = g_physicalHeap.Alloc((size + page - 1) & ~(page - 1), std::max(align, page));
    TRACE_IMPORT("size=0x%X protect=0x%X align=0x%X -> 0x%X", size, protect, align, addr);
    if (!addr) LogWarn("MmAllocatePhysicalMemoryEx: out of physical memory (0x%X)", size);
    RETURN(addr);
}

GUEST_IMPORT(MmFreePhysicalMemory) { g_physicalHeap.Free(ARG(4)); RETURN(0); }
GUEST_IMPORT(MmQueryAddressProtect) { RETURN(PAGE_READWRITE); }
GUEST_IMPORT(MmSetAddressProtect) { RETURN(0); }
// 0xE0000000 view is offset by one page (see MemoryInit)
GUEST_IMPORT(MmGetPhysicalAddress) { RETURN((ARG(3) & 0x1FFFFFFF) + (ARG(3) >= 0xE0000000 ? 0x1000 : 0)); }
GUEST_IMPORT(MmMapIoSpace) { LogWarn("MmMapIoSpace(0x%X, 0x%X)", ARG(3), ARG(4)); RETURN(ARG(3)); }

// MmQueryStatistics(MM_STATISTICS*): first field is the struct size.
GUEST_IMPORT(MmQueryStatistics)
{
    uint32_t s = ARG(3);
    uint32_t len = GuestRead32(s);
    if (len < 0x68) RETURN(STATUS_INVALID_PARAMETER);
    uint32_t totalPages = 0x20000;   // 512 MiB / 4 KiB
    uint32_t used = (g_virtualHeap.Used() + g_physicalHeap.Used()) / 0x1000;
    GuestWrite32(s + 0x04, totalPages);                                   // TotalPhysicalPages
    GuestWrite32(s + 0x08, 0x1000);                                       // KernelPages
    GuestWrite32(s + 0x0C, used < totalPages ? totalPages - used : 0);    // TitleAvailablePages
    GuestWrite32(s + 0x10, totalPages);                                   // TitleTotalVirtualMemoryBytes (approx)
    GuestWrite32(s + 0x18, used);                                         // TitlePhysicalPages
    RETURN(STATUS_SUCCESS);
}
