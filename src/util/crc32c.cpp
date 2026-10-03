#include "util/crc32c.hpp"

#include <array>

namespace lob {
namespace {

/// CRC-32C polynomial, reflected.
constexpr std::uint32_t kPoly = 0x82F63B78U;

/// Eight tables, one per byte position, so eight bytes are folded per iteration
/// instead of one.
///
/// The byte-at-a-time version was measured at ~287 Mi/s on a 72-byte payload. That
/// is slow enough to matter: every frame is CRC'd on the way in *and* on the way
/// out, and the journal does the same per record, so a venue pays this twice per
/// message. Slice-by-eight is the standard fix and needs no CPU feature detection,
/// which keeps the portability this file exists to provide.
struct Tables {
  std::array<std::array<std::uint32_t, 256>, 8> t{};
};

const Tables& crc_tables() {
  static const Tables tables = [] {
    Tables out;
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0U ? (c >> 1U) ^ kPoly : c >> 1U;
      }
      out.t[0][i] = c;
    }
    for (std::size_t n = 1; n < 8; ++n) {
      for (std::uint32_t i = 0; i < 256; ++i) {
        const std::uint32_t prev = out.t[n - 1][i];
        out.t[n][i] = (prev >> 8U) ^ out.t[0][prev & 0xFFU];
      }
    }
    return out;
  }();
  return tables;
}

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t len, std::uint32_t seed) noexcept {
  const auto* p = static_cast<const std::uint8_t*>(data);
  const Tables& tables = crc_tables();
  const auto& t = tables.t;
  std::uint32_t c = ~seed;

  // Eight bytes at a time while there is a full slice left.
  while (len >= 8) {
    c ^= static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    c = t[7][c & 0xFFU] ^ t[6][(c >> 8) & 0xFFU] ^ t[5][(c >> 16) & 0xFFU] ^
        t[4][(c >> 24) & 0xFFU] ^ t[3][p[4]] ^ t[2][p[5]] ^ t[1][p[6]] ^ t[0][p[7]];
    p += 8;
    len -= 8;
  }
  // Tail byte at a time.
  for (std::size_t i = 0; i < len; ++i) {
    c = t[0][(c ^ p[i]) & 0xFFU] ^ (c >> 8U);
  }
  return ~c;
}

}  // namespace lob