#pragma once

#include <bit>
#include <compare>
#include <cstdint>
#include <limits>

namespace lob {

/// Fixed-width little-endian scalar load/store.
///
/// The wire protocol and the journal are both little-endian on every supported
/// platform, but memcpy of a native integer is only correct on a little-endian
/// host. These helpers make the byte order explicit at every boundary so the
/// code stays correct if it is ever built big-endian, and they compile to a
/// single instruction on x86 and ARM64 instead of a byte-by-byte loop.

[[nodiscard]] inline std::uint16_t load_u16_le(const std::uint8_t* p) noexcept {
  std::uint16_t v;
  __builtin_memcpy(&v, p, sizeof(v));
  if constexpr (std::endian::native == std::endian::big) {
    v = static_cast<std::uint16_t>(__builtin_bswap16(v));
  }
  return v;
}

[[nodiscard]] inline std::uint32_t load_u32_le(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  __builtin_memcpy(&v, p, sizeof(v));
  if constexpr (std::endian::native == std::endian::big) {
    v = __builtin_bswap32(v);
  }
  return v;
}

[[nodiscard]] inline std::uint64_t load_u64_le(const std::uint8_t* p) noexcept {
  std::uint64_t v;
  __builtin_memcpy(&v, p, sizeof(v));
  if constexpr (std::endian::native == std::endian::big) {
    v = __builtin_bswap64(v);
  }
  return v;
}

[[nodiscard]] inline std::int64_t load_i64_le(const std::uint8_t* p) noexcept {
  const auto u = load_u64_le(p);
  std::int64_t v;
  __builtin_memcpy(&v, &u, sizeof(v));
  return v;
}

inline void store_u16_le(std::uint8_t* p, std::uint16_t v) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    v = __builtin_bswap16(v);
  }
  __builtin_memcpy(p, &v, sizeof(v));
}

inline void store_u32_le(std::uint8_t* p, std::uint32_t v) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    v = __builtin_bswap32(v);
  }
  __builtin_memcpy(p, &v, sizeof(v));
}

inline void store_u64_le(std::uint8_t* p, std::uint64_t v) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    v = __builtin_bswap64(v);
  }
  __builtin_memcpy(p, &v, sizeof(v));
}

inline void store_i64_le(std::uint8_t* p, std::int64_t v) noexcept {
  std::uint64_t u;
  __builtin_memcpy(&u, &v, sizeof(u));
  store_u64_le(p, u);
}

}  // namespace lob