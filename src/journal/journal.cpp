#include "journal/journal.hpp"

#include "util/crc32c.hpp"

#include <array>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>
#include <functional>

namespace lob {
namespace {

constexpr std::array<char, 4> kMagic{'L', 'O', 'B', 'J'};
constexpr std::size_t kHeaderLen = 4 + 1 + 4;  // magic + kind + length
constexpr std::size_t kCrcLen = 4;

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
  }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
  }
}

void put_u8(std::vector<std::uint8_t>& out, std::uint8_t v) {
  out.push_back(v);
}

/// Widen a byte out of a string_view without sign extension. `char` is signed on
/// every platform this builds on, so casting one straight to uint32_t/uint64_t
/// turns any byte >= 0x80 into 0xFF.. and silently corrupts the value.
constexpr std::uint32_t byte(char c) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(c));
}
constexpr std::uint64_t ubyte(char c) noexcept {
  return static_cast<std::uint64_t>(static_cast<unsigned char>(c));
}

/// Little-endian cursor over a payload slice. Every read is bounds-checked and
/// sets a sticky failure flag, so a short buffer can never read out of range.
class Reader {
 public:
  explicit Reader(std::string_view b) : b_(b) {}

  std::uint8_t u8() {
    return need(1) ? take(1)[0] : 0;
  }
  std::uint32_t u32() {
    if (!need(4)) {
      return 0;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= ubyte(b_[p_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    p_ += 4;
    return v;
  }
  /// Signed variant: Price/Quantity/etc. wrap a signed 64-bit value, and
  /// brace-initialising them from a uint64_t is a narrowing error.
  std::int64_t i64() {
    return static_cast<std::int64_t>(u64());
  }
  std::uint64_t u64() {
    if (!need(8)) {
      return 0;
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= ubyte(b_[p_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    p_ += 8;
    return v;
  }
  [[nodiscard]] bool ok() const {
    return ok_;
  }

 private:
  bool need(std::size_t n) {
    if (!ok_ || p_ + n > b_.size()) {
      ok_ = false;
      return false;
    }
    return true;
  }
  std::string_view take(std::size_t n) {
    const std::string_view v = b_.substr(p_, n);
    p_ += n;
    return v;
  }
  std::string_view b_;
  std::size_t p_ = 0;
  bool ok_ = true;
};

}  // namespace

void encode_payload(const JournalRecord& r, std::vector<std::uint8_t>& out) {
  // No kind byte: the frame header already carries it, and duplicating it here
  // would shift every decoded field by one.
  switch (r.kind) {
    case RecordKind::NewOrder: {
      const auto& n = r.new_order;
      put_u64(out, n.seq.value);
      put_u64(out, n.ts.value);
      put_u32(out, n.symbol.value);
      put_u64(out, n.order_id.value);
      put_u64(out, n.participant.value);
      put_u8(out, static_cast<std::uint8_t>(n.side));
      put_u8(out, static_cast<std::uint8_t>(n.type));
      put_u8(out, static_cast<std::uint8_t>(n.tif));
      put_u64(out, n.price.value);
      put_u64(out, n.trigger_price.value);
      put_u64(out, n.quantity.value);
      put_u64(out, n.display_qty.value);
      put_u8(out, n.post_only ? 1U : 0U);
      break;
    }
    case RecordKind::Cancel: {
      const auto& c = r.cancel;
      put_u64(out, c.seq.value);
      put_u64(out, c.ts.value);
      put_u32(out, c.symbol.value);
      put_u64(out, c.order_id.value);
      put_u64(out, c.participant.value);
      break;
    }
    case RecordKind::Replace: {
      const auto& p = r.replace;
      put_u64(out, p.seq.value);
      put_u64(out, p.ts.value);
      put_u32(out, p.symbol.value);
      put_u64(out, p.order_id.value);
      put_u64(out, p.participant.value);
      put_u64(out, p.new_price.value);
      put_u64(out, p.new_quantity.value);
      break;
    }
    case RecordKind::MassCancel: {
      const auto& m = r.mass_cancel;
      put_u64(out, m.seq.value);
      put_u64(out, m.ts.value);
      put_u64(out, m.participant.value);
      break;
    }
    case RecordKind::Invalid:
      break;
  }
}

bool decode_payload(RecordKind kind, std::string_view p, JournalRecord& out) {
  out = JournalRecord{};
  out.kind = kind;
  Reader r{p};
  switch (kind) {
    case RecordKind::NewOrder: {
      auto& n = out.new_order;
      n.seq.value = r.u64();
      n.ts.value = r.i64();
      n.symbol.value = r.u32();
      n.order_id.value = r.u64();
      n.participant.value = r.u64();
      n.side = static_cast<Side>(r.u8());
      n.type = static_cast<OrderType>(r.u8());
      n.tif = static_cast<TimeInForce>(r.u8());
      n.price.value = r.i64();
      n.trigger_price.value = r.i64();
      n.quantity.value = r.i64();
      n.display_qty.value = r.i64();
      n.post_only = r.u8() != 0U;
      break;
    }
    case RecordKind::Cancel: {
      auto& c = out.cancel;
      c.seq.value = r.u64();
      c.ts.value = r.i64();
      c.symbol.value = r.u32();
      c.order_id.value = r.u64();
      c.participant.value = r.u64();
      break;
    }
    case RecordKind::Replace: {
      auto& x = out.replace;
      x.seq.value = r.u64();
      x.ts.value = r.i64();
      x.symbol.value = r.u32();
      x.order_id.value = r.u64();
      x.participant.value = r.u64();
      x.new_price.value = r.i64();
      x.new_quantity.value = r.i64();
      break;
    }
    case RecordKind::MassCancel: {
      auto& m = out.mass_cancel;
      m.seq.value = r.u64();
      m.ts.value = r.i64();
      m.participant.value = r.u64();
      break;
    }
    default:
      return false;
  }
  return r.ok();
}

void encode_record(const JournalRecord& rec, std::vector<std::uint8_t>& out) {
  const std::size_t start = out.size();
  std::vector<std::uint8_t> body;
  encode_payload(rec, body);

  out.insert(out.end(), kMagic.begin(), kMagic.end());
  put_u8(out, static_cast<std::uint8_t>(rec.kind));
  put_u32(out, static_cast<std::uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  // CRC covers kind, length and payload -- everything after this record's magic.
  //
  // Relative to `start`, NOT to offset 0 of `out`. Offsets from 0 are only correct
  // when the record happens to be the first thing in the buffer, which is true for
  // a single-record encode and false for every record a journal ever writes. The
  // symptom is a log whose first record replays and whose every later record is
  // silently discarded as corrupt, so recovery truncates to one record and loses
  // everything after it.
  put_u32(out, crc32c(out.data() + start + 4, out.size() - start - 4, 0));
}

ReplayReport replay(std::string_view bytes, const std::function<void(const JournalRecord&)>& sink) {
  ReplayReport rep;
  std::size_t p = 0;
  while (p + kHeaderLen + kCrcLen <= bytes.size()) {
    // magic
    if (std::memcmp(bytes.data() + p, kMagic.data(), kMagic.size()) != 0) {
      rep.truncated = true;
      return rep;
    }
    const auto kind = static_cast<RecordKind>(static_cast<std::uint8_t>(bytes[p + 4]));
    // std::string_view::operator[] yields signed char, so every byte must be
    // widened via unsigned char: casting a byte >= 0x80 straight to uint32_t
    // sign-extends it to 0xFFFFFFxx and corrupts the value.
    const std::uint32_t len = byte(bytes[p + 5]) | (byte(bytes[p + 6]) << 8) |
                              (byte(bytes[p + 7]) << 16) | (byte(bytes[p + 8]) << 24);

    const std::size_t total = kHeaderLen + len + kCrcLen;
    if (p + total > bytes.size()) {
      rep.truncated = true;  // torn tail
      return rep;
    }

    const std::string_view body = bytes.substr(p + kHeaderLen, len);
    const std::size_t c = p + kHeaderLen + len;
    const std::uint32_t want = byte(bytes[c]) | (byte(bytes[c + 1]) << 8) |
                               (byte(bytes[c + 2]) << 16) | (byte(bytes[c + 3]) << 24);
    const std::uint32_t got = crc32c(bytes.data() + p + 4, kHeaderLen - 4 + len, 0);
    if (want != got) {
      rep.truncated = true;  // corrupt
      return rep;
    }

    JournalRecord rec;
    if (!decode_payload(kind, body, rec)) {
      rep.truncated = true;
      return rep;
    }
    sink(rec);
    ++rep.records_applied;
    p += total;
  }
  if (p != bytes.size()) {
    rep.truncated = true;  // trailing partial header
  }
  rep.bytes_consumed = p;
  return rep;
}

// ---------------------------------------------------------------------------
// JournalWriter
// ---------------------------------------------------------------------------

namespace {
/// Batch size before an automatic flush. Large enough that the write syscall is
/// not on the hot path, small enough that a graceful shutdown rarely has a large
/// tail to lose.
constexpr std::size_t kFlushThreshold = 64UL * 1024UL;
}  // namespace

JournalWriter::~JournalWriter() {
  if (fd_ >= 0) {
    (void)close();
  }
}

JournalWriter::JournalWriter(JournalWriter&& other) noexcept
    : fd_(other.fd_),
      buffer_(std::move(other.buffer_)),
      written_(other.written_.load(std::memory_order_relaxed)),
      bytes_(other.bytes_.load(std::memory_order_relaxed)),
      healthy_(other.healthy_) {
  other.fd_ = -1;
  other.healthy_ = true;
}

JournalWriter& JournalWriter::operator=(JournalWriter&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      (void)close();
    }
    fd_ = other.fd_;
    buffer_ = std::move(other.buffer_);
    written_.store(other.written_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    bytes_.store(other.bytes_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    healthy_ = other.healthy_;
    other.fd_ = -1;
    other.healthy_ = true;
  }
  return *this;
}

bool JournalWriter::open(const std::string& path, bool append) {
  if (fd_ >= 0) {
    (void)close();
  }
  // Append, not truncate, when recovering: truncating here would erase the very log
  // that was just replayed, so a second crash would recover nothing at all. A torn
  // tail is not a reason to discard the good prefix -- replay already stops at the
  // first bad record, and the next append simply lands after it.
  //
  // Starting fresh (append == false) truncates, because then no replay happened and
  // an existing file is stale by definition.
  const int flags = O_WRONLY | O_CREAT | O_CLOEXEC | (append ? O_APPEND : O_TRUNC);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX open() is variadic.
  fd_ = ::open(path.c_str(), flags, 0644);
  if (fd_ < 0) {
    healthy_ = false;
    return false;
  }
  buffer_.clear();
  buffer_.reserve(kFlushThreshold + 256U);
  healthy_ = true;
  base_path_ = path;
  segment_index_ = 0;
  segment_written_ = 0;
  since_sync_ = 0;
  written_.store(0, std::memory_order_relaxed);
  bytes_.store(0, std::memory_order_relaxed);
  return true;
}

std::string JournalWriter::segment_path(std::uint64_t index) const {
  if (index == 0U) {
    return base_path_;
  }
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), ".%06llu", static_cast<unsigned long long>(index));
  return base_path_ + suffix;
}

std::vector<std::string> JournalWriter::segments() const {
  std::vector<std::string> out;
  if (base_path_.empty()) {
    return out;
  }
  out.push_back(segment_path(0U));
  for (std::uint64_t i = 1U; i <= segment_index_; ++i) {
    out.push_back(segment_path(i));
  }
  return out;
}

bool JournalWriter::rotate() {
  if (fd_ >= 0) {
    // Flush and fsync before moving on: an earlier segment must be durable or the
    // next crash loses the end of it with no way to know the file stops there.
    if (!flush()) {
      return false;
    }
    if (::fsync(fd_) != 0) {
      healthy_ = false;
      return false;
    }
    (void)::close(fd_);
  }
  ++segment_index_;
  const std::string next = segment_path(segment_index_);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX open() is variadic.
  fd_ = ::open(next.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    healthy_ = false;
    return false;
  }
  segment_written_ = 0;
  since_sync_ = 0;
  return true;
}

bool JournalWriter::sync() {
  if (fd_ < 0) {
    return false;
  }
  if (!flush()) {
    return false;
  }
  if (::fsync(fd_) != 0) {
    healthy_ = false;
    return false;
  }
  since_sync_ = 0;
  return true;
}

std::string read_all_segments(const std::string& base_path) {
  std::string out;
  const auto slurp = [&out](const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return false;
    }
    char buf[65536];
    while (true) {
      const ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0) {
        out.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      break;
    }
    ::close(fd);
    return true;
  };
  (void)slurp(base_path);
  for (std::uint64_t i = 1U;; ++i) {
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), ".%06llu", static_cast<unsigned long long>(i));
    const std::string path = base_path + suffix;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg) -- POSIX open() is variadic.
    const int probe = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (probe < 0) {
      break;  // first gap ends the journal
    }
    ::close(probe);
    if (!slurp(path)) {
      break;
    }
  }
  return out;
}

bool JournalWriter::append(const JournalRecord& record) {
  if (fd_ < 0) {
    return false;
  }
  encode_record(record, buffer_);
  written_.fetch_add(1, std::memory_order_relaxed);
  ++since_sync_;
  if (buffer_.size() >= kFlushThreshold && !flush()) {
    return false;
  }
  // Deliberately not inside the flush branch. Tying rollover to the 64 KiB flush
  // threshold means a small segment limit never takes effect until the buffer
  // fills, so a venue configured for 1 MiB segments silently wrote one giant file.
  // rotate() and sync() each flush first, so the checks are safe at any size.
  if (durability_.segment_bytes != 0U &&
      segment_written_ + buffer_.size() >= durability_.segment_bytes) {
    if (!rotate()) {
      return false;
    }
  }
  // This is the whole crash-durability story: a crash now costs at most
  // fsync_every_records records rather than the whole buffer.
  if (durability_.fsync_every_records != 0U && since_sync_ >= durability_.fsync_every_records) {
    return sync();
  }
  return true;
}

bool JournalWriter::flush() {
  if (fd_ < 0 || buffer_.empty()) {
    return healthy_;
  }
  std::size_t offset = 0;
  while (offset < buffer_.size()) {
    const ssize_t n = ::write(fd_, buffer_.data() + offset, buffer_.size() - offset);
    if (n > 0) {
      offset += static_cast<std::size_t>(n);
      continue;
    }
    // A short write is progress; anything else is a real failure.
    if (n < 0 && errno == EINTR) {
      continue;
    }
    healthy_ = false;
    return false;
  }
  bytes_.fetch_add(buffer_.size(), std::memory_order_relaxed);
  segment_written_ += buffer_.size();
  buffer_.clear();
  return true;
}

bool JournalWriter::close() {
  if (fd_ < 0) {
    return healthy_;
  }
  const bool flushed = flush();
  // fsync on close, not on every write: this is what makes a *graceful* restart
  // lossless. A crash still loses whatever the page cache had not taken.
  if (flushed) {
    healthy_ = (::fsync(fd_) == 0) && healthy_;
  }
  ::close(fd_);
  fd_ = -1;
  buffer_.clear();
  return healthy_;
}

ReplayReport replay_into_engine(std::string_view bytes, Engine& engine) {
  return replay(bytes, [&engine](const JournalRecord& r) {
    // Recovery rebuilds state, not an event stream: nobody is consuming the
    // events, and a long journal would otherwise overrun the fixed buffer.
    engine.clear_events();
    switch (r.kind) {
      case RecordKind::NewOrder:
        engine.submit(r.new_order);
        break;
      case RecordKind::Cancel:
        engine.submit(r.cancel);
        break;
      case RecordKind::Replace:
        engine.submit(r.replace);
        break;
      case RecordKind::MassCancel:
        engine.submit(r.mass_cancel);
        break;
      case RecordKind::Invalid:
        break;
    }
  });
}

}  // namespace lob