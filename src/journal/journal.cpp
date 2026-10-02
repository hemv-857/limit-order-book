#include "journal/journal.hpp"

#include "util/crc32c.hpp"

#include <array>
#include <cstring>
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
  std::vector<std::uint8_t> body;
  encode_payload(rec, body);

  out.insert(out.end(), kMagic.begin(), kMagic.end());
  put_u8(out, static_cast<std::uint8_t>(rec.kind));
  put_u32(out, static_cast<std::uint32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
  // CRC covers kind, length and payload -- everything after the magic.
  put_u32(out, crc32c(out.data() + 4, out.size() - 4, 0));
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