#pragma once
#if defined(__x86_64__)
#include <immintrin.h>
#endif
// Guest memory: one 4 GiB host reservation; guest address A lives at g_base + A.
//
// Xbox 360 address map used here (simplified):
//   0x40000000-0x7EFFFFFF  virtual heap       (NtAllocateVirtualMemory, stacks, misc)
//   0x82000000-...         XEX image, then XenonRecomp's function table
//   0xA0000000-0xBFFFFFFF  physical heap      (MmAllocatePhysicalMemoryEx, GPU-visible)
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <map>

extern uint8_t* g_base;

// Equality of two byte ranges, inlined (the hot per-draw comparisons of 64..128 bytes otherwise
// call libc memcmp).
inline bool FastEq(const void* a, const void* b, size_t n)
{
    const uint8_t* x = static_cast<const uint8_t*>(a);
    const uint8_t* y = static_cast<const uint8_t*>(b);
#ifdef __AVX2__
    for (; n >= 32; x += 32, y += 32, n -= 32)
    {
        __m256i d = _mm256_xor_si256(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(x)), _mm256_loadu_si256(reinterpret_cast<const __m256i*>(y)));
        if (!_mm256_testz_si256(d, d)) return false;
    }
#endif
    for (; n >= 8; x += 8, y += 8, n -= 8) { uint64_t u, v; memcpy(&u, x, 8); memcpy(&v, y, 8); if (u != v) return false; }
    for (; n >= 4; x += 4, y += 4, n -= 4) { uint32_t u, v; memcpy(&u, x, 4); memcpy(&v, y, 4); if (u != v) return false; }
    for (; n; n--) if (*x++ != *y++) return false;
    return true;
}

inline void* GuestToHost(uint32_t a) { return g_base + a; }
inline uint32_t HostToGuest(const void* p) { return uint32_t((const uint8_t*)p - g_base); }

// Big-endian accessors for guest memory.
inline uint8_t  GuestRead8 (uint32_t a) { return g_base[a]; }
inline uint16_t GuestRead16(uint32_t a) { uint16_t v; memcpy(&v, g_base + a, 2); return __builtin_bswap16(v); }
inline uint32_t GuestRead32(uint32_t a) { uint32_t v; memcpy(&v, g_base + a, 4); return __builtin_bswap32(v); }
inline uint64_t GuestRead64(uint32_t a) { uint64_t v; memcpy(&v, g_base + a, 8); return __builtin_bswap64(v); }
inline void GuestWrite8 (uint32_t a, uint8_t v)  { g_base[a] = v; }
inline void GuestWrite16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); memcpy(g_base + a, &v, 2); }
inline void GuestWrite32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); memcpy(g_base + a, &v, 4); }
inline void GuestWrite64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); memcpy(g_base + a, &v, 8); }

// Simple thread-safe first-fit page allocator over a fixed guest range.
class GuestHeap
{
public:
    GuestHeap(const char* name, uint32_t begin, uint32_t end, uint32_t page);
    uint32_t Alloc(uint32_t size, uint32_t align = 0);   // returns 0 on failure; memory zeroed
    uint32_t AllocAt(uint32_t addr, uint32_t size);      // fixed-address reservation, 0 on failure
    bool Free(uint32_t addr);
    uint32_t SizeOf(uint32_t addr);                      // 0 if not an allocation start
    bool Contains(uint32_t addr) const { return addr >= m_begin && addr < m_end; }
    uint32_t Used() const { return m_used; }
    const char* Name() const { return m_name; }

private:
    const char* m_name;
    uint32_t m_begin, m_end, m_page, m_used = 0;
    std::map<uint32_t, uint32_t> m_free;   // start -> size
    std::map<uint32_t, uint32_t> m_alloc;  // start -> size
    std::mutex m_mutex;
};

extern GuestHeap g_virtualHeap;
extern GuestHeap g_physicalHeap;

// Reserve the 4 GiB guest space. physFd: 512 MiB shared object backing physical memory (mapped
// at 0xA0000000/0xC0000000/0xE0000000; consumed), -1 = create a memfd, kPhysNoAlias = plain
// anonymous memory with unaliased views.
constexpr int kPhysNoAlias = -2;
constexpr int kPhysExternal = -3;   // reserve only: the caller maps the three views (Android, VkMapGuestMemoryPlaced)
void MemoryInit(int physFd = -1);
uint32_t GuestAlloc(uint32_t size, uint32_t align = 16);  // convenience: virtual heap
void GuestFree(uint32_t addr);
