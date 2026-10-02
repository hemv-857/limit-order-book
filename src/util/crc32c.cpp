#include "util/crc32c.hpp"

#include <array>

namespace lob {
namespace {

/// CRC-32C polynomial, reflected.
constexpr std::uint32_t kPoly = 0x82F63B78U;

const std::array<std::uint32_t, 256>& crc_table() {
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0U ? (c >> 1U) ^ kPoly : c >> 1U;
      }
      t[i] = c;
    }
    return t;
  }();
  return table;
}

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t len, std::uint32_t seed) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  const auto& t = crc_table();
  std::uint32_t c = ~seed;
  for (std::size_t i = 0; i < len; ++i) {
    c = t[(c ^ p[i]) & 0xFFU] ^ (c >> 8U);
  }
  return ~c;
}

}  // namespace lob