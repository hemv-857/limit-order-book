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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lob {

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

/// Every segment belonging to `base_path`, concatenated in write order.
///
/// The base file first, then `<base_path>.000001`, `.000002`, ... up to the first
/// gap. A gap ends the journal: a missing segment means records are lost, and
/// silently continuing past it would replay a book that is quietly missing orders.
/// Only a torn *tail* (the last segment) is safe to tolerate, and `replay`
/// already handles that.
std::string read_all_segments(const std::string& base_path);

/// Read every intact record from `bytes` and feed it to `sink` in order.
/// Stops cleanly at the first torn or corrupt record and sets `truncated`.
ReplayReport replay(std::string_view bytes, const std::function<void(const JournalRecord&)>& sink);

/// How often the writer forces buffered records all the way to stable storage.
struct JournalDurability {
  /// fsync after this many records have been written. 0 disables periodic fsync,
  /// leaving only the fsync on close -- i.e. graceful-restart safety only.
  /// 1 gives per-record durability at the cost of an fsync per order, which is
  /// almost never what a venue wants; the useful middle ground is a few thousand.
  std::uint64_t fsync_every_records = 4096;
  /// Roll to a new segment once the current one reaches this many bytes. 0 keeps
  /// a single file forever. Bounds disk growth and keeps any one file small
  /// enough to read quickly during recovery.
  std::uint64_t segment_bytes = 64ULL * 1024ULL * 1024ULL;
};

/// Append-only writer for the journal.
///
/// Buffers records, writes them in batches, and fsyncs on a configurable cadence
/// so that a crash loses at most `JournalDurability::fsync_every_records`
/// records rather than everything still buffered. `close()` always flushes and
/// fsyncs, which is what makes the graceful case safe regardless of policy.
///
/// Segments: when `segment_bytes` is reached the writer rolls to
/// `<path>.<NNNNNN>`, starting from `<path>.<000001>`. `close()` flushes the
/// active segment, so the highest-numbered segment is always the open one.
class JournalWriter {
 public:
  JournalWriter() = default;
  ~JournalWriter();

  JournalWriter(const JournalWriter&) = delete;
  JournalWriter& operator=(const JournalWriter&) = delete;
  JournalWriter(JournalWriter&& other) noexcept;
  JournalWriter& operator=(JournalWriter&& other) noexcept;

  /// Open `path`, truncating any existing journal. Returns false on failure.
  /// Open for writing. `append` continues an existing log (used after recovery, so
  /// the log just replayed is not destroyed); otherwise the file is truncated,
  /// because nothing was replayed and any existing file is stale.
  bool open(const std::string& path, bool append = false);

  /// Durability policy. Must be set before open() to affect the first segment.
  void set_durability(const JournalDurability& durability) noexcept {
    durability_ = durability;
  }

  /// Force buffered records to stable storage now. This is the call an operator
  /// makes before declaring a checkpoint good.
  bool sync();

  [[nodiscard]] bool is_open() const noexcept {
    return fd_ >= 0;
  }

  /// Append one record. Buffered; see the class comment on durability.
  /// Returns false only if the record is invalid -- a write failure is counted and
  /// surfaced by `healthy()`, because silently losing records is worse than
  /// reporting a degraded journal.
  bool append(const JournalRecord& record);

  /// Push buffered records to the file. Does not fsync.
  bool flush();

  /// Flush and fsync the active segment, then close. Called on graceful shutdown.
  bool close();

  /// Base path and the segments currently open, lowest first. Used by the replay
  /// tool and by recovery, which must read every segment in order.
  [[nodiscard]] std::vector<std::string> segments() const;

  /// False once any write has failed. The venue keeps serving but the operator
  /// needs to know the journal is no longer trustworthy.
  [[nodiscard]] bool healthy() const noexcept {
    return healthy_;
  }

  // Atomic: the acceptor thread appends while an observer (a monitoring caller, a
  // test) reads these. TSan flagged the plain read in records_written().
  [[nodiscard]] std::uint64_t records_written() const noexcept {
    return written_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t bytes_written() const noexcept {
    return bytes_.load(std::memory_order_relaxed);
  }

 private:
  /// Roll to the next segment if the current one is full. Caller flushes first.
  bool rotate();
  [[nodiscard]] std::string segment_path(std::uint64_t index) const;

  int fd_ = -1;
  std::string base_path_;
  std::uint64_t segment_index_ = 0;
  std::uint64_t segment_written_ = 0;
  std::uint64_t since_sync_ = 0;
  JournalDurability durability_{};
  std::vector<std::uint8_t> buffer_;
  std::atomic<std::uint64_t> written_{0};
  std::atomic<std::uint64_t> bytes_{0};
  bool healthy_ = true;
};

/// Replay a journal file into a live engine. Returns the report; `truncated` is
/// true when a torn tail was discarded, which is the expected outcome of a crash.
ReplayReport replay_into_engine(std::string_view bytes, Engine& engine);

}  // namespace lob