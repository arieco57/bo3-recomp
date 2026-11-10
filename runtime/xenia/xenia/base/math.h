// Minimal shim for Xenia's xenia/base/math.h (enough for the vendored GPU headers).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include "xenia/base/assert.h"
#define XE_NOALIAS __attribute__((const))
#define XE_FORCEINLINE inline __attribute__((always_inline))
#define XE_NOINLINE __attribute__((noinline))
#define XE_COLD __attribute__((cold))
#define XE_LIKELY(x) __builtin_expect(!!(x), 1)
#define XE_UNLIKELY(x) __builtin_expect(!!(x), 0)
namespace xe {
using fourcc_t = uint32_t;
constexpr fourcc_t make_fourcc(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}
constexpr fourcc_t make_fourcc(const char (&s)[5]) { return make_fourcc(s[0], s[1], s[2], s[3]); }
template <typename T, size_t N> constexpr size_t countof(T (&)[N]) { return N; }
template <typename T> constexpr T align(T value, T alignment) { return (value + alignment - 1) / alignment * alignment; }
template <typename T> constexpr T round_up(T value, T multiple) { return multiple ? (value + multiple - 1) / multiple * multiple : value; }
template <typename T> constexpr T clamp_float(T v, T lo, T hi) { return std::min(std::max(v, lo), hi); }
inline uint32_t log2_ceil(uint32_t v) { return v <= 1 ? 0 : 32 - __builtin_clz(v - 1); }
inline uint32_t log2_floor(uint32_t v) { return 31 - __builtin_clz(v); }
template <typename T> inline T bit_count(T v) { return T(__builtin_popcountll(uint64_t(v))); }
inline bool bit_scan_forward(uint32_t v, uint32_t* out) { if (!v) return false; *out = __builtin_ctz(v); return true; }
inline bool bit_scan_forward(uint64_t v, uint32_t* out) { if (!v) return false; *out = __builtin_ctzll(v); return true; }
inline bool bit_scan_reverse(uint32_t v, uint32_t* out) { if (!v) return false; *out = 31 - __builtin_clz(v); return true; }
inline uint32_t lzcnt(uint32_t v) { return v ? __builtin_clz(v) : 32; }
inline uint32_t tzcnt(uint32_t v) { return v ? __builtin_ctz(v) : 32; }
}  // namespace xe
