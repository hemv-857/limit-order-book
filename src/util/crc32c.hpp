// CRC-32C (Castagnoli), shared by the journal and the wire protocol.
//
// Software table-driven. Both callers are off the hot path -- recovery runs once
// at startup, and framing runs per message on the gateway thread -- so the cost
// is irrelevant next to portability and the absence of a CPU feature dependency.

#pragma once

#include <cstddef>
#include <cstdint>

namespace lob {

/// CRC-32C (Castagnoli). Software table-driven: recovery runs once at startup,
/// off the hot path, so the cost is irrelevant and the code stays portable.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t len,
                                   std::uint32_t seed = 0) noexcept;

}  // namespace lob
