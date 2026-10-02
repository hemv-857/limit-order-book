// Write-ahead journal and recovery.
//
// The engine is deterministic, so recovery does not need the internal book to be
// serialised: record the requests, replay them, and the book rebuilds itself.
// That keeps the on-disk format small, human-inspectable and independent of the
// book layout, at the cost of replay time proportional to the journal.
//
// Record framing (little endian throughout):
//
//   magic  : 4 bytes, "LOBJ"
//   kind   : 1 byte,   RecordKind
//   length : 4 bytes,  payload length
//   payload: `length` bytes, request encoded by encode_payload
//   crc32c : 4 bytes,  CRC-32C over kind + length + payload
//
// A torn tail -- the common outcome of a crash mid-write -- fails either the
// length check, the magic check or the CRC, and is reported as a clean
// truncation rather than an error. Everything before it is still valid.

#pragma once

#include "core/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lob {

/// CRC-32C (Castagnoli). Software table-driven: recovery runs once at startup,
/// off the hot path, so the cost is irrelevant and the code stays portable.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t len,
                                   std::uint32_t seed = 0) noexcept;

enum class RecordKind : std::uint8_t {
  Invalid = 0,
  NewOrder = 1,
  Cancel = 2,
  Replace = 3,
  MassCancel = 4,
};

/// A decoded request, tagged by kind. One variant covers every request type so
/// replay is a single switch rather than four call sites.
struct JournalRecord {
  RecordKind kind{};
  NewOrderRequest new_order{};
  CancelRequest cancel{};
  ReplaceRequest replace{};
  MassCancelRequest mass_cancel{};
};

/// Encode a request into `out` (appending). Stable format: the on-disk bytes
/// are part of the recovery contract.
void encode_payload(const JournalRecord& record, std::vector<std::uint8_t>& out);

/// Decode one payload. Returns false if the buffer is short or the kind is
/// unknown -- treated as a torn tail by the reader.
[[nodiscard]] bool decode_payload(RecordKind kind, std::string_view payload, JournalRecord& out);

/// One framed record, as written.
void encode_record(const JournalRecord& record, std::vector<std::uint8_t>& out);

/// Result of reading a journal.
struct ReplayReport {
  std::size_t records_applied = 0;
  std::size_t bytes_consumed = 0;
  bool truncated = false;  ///< a torn tail was found and discarded
};

/// Read every intact record from `bytes` and feed it to `sink` in order.
/// Stops cleanly at the first torn or corrupt record and sets `truncated`.
ReplayReport replay(std::string_view bytes, const std::function<void(const JournalRecord&)>& sink);

/// Replay a journal file into a live engine. Returns the report; `truncated` is
/// true when a torn tail was discarded, which is the expected outcome of a crash.
ReplayReport replay_into_engine(std::string_view bytes, Engine& engine);

}  // namespace lob