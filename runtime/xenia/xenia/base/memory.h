// Minimal shim for Xenia's xenia/base/memory.h.
#pragma once
#include <cstdint>
#include <cstring>
namespace xe {
inline uint8_t byte_swap(uint8_t v) { return v; }
inline uint16_t byte_swap(uint16_t v) { return __builtin_bswap16(v); }
inline uint32_t byte_swap(uint32_t v) { return __builtin_bswap32(v); }
inline uint64_t byte_swap(uint64_t v) { return __builtin_bswap64(v); }
inline int16_t byte_swap(int16_t v) { return int16_t(__builtin_bswap16(uint16_t(v))); }
inline int32_t byte_swap(int32_t v) { return int32_t(__builtin_bswap32(uint32_t(v))); }
inline float byte_swap(float v) { uint32_t u; memcpy(&u, &v, 4); u = __builtin_bswap32(u); memcpy(&v, &u, 4); return v; }
template <typename T> struct be {
  T value;
  be() = default;
  be(T v) : value(byte_swap(v)) {}
  operator T() const { return byte_swap(value); }
};
template <typename T> struct le {
  T value;
  le() = default;
  le(T v) : value(v) {}
  operator T() const { return value; }
};
}  // namespace xe
