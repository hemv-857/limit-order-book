// The only reinterpret_casts in this file view a std::uint8_t buffer as a
// string_view. char and std::uint8_t have identical representation, and this is
// how a byte buffer becomes a view without copying it. File-scoped because
// clang-format reflows code and would detach line-local suppressions.
//
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)

#include "protocol/codec.hpp"

#include "util/crc32c.hpp"

#include <array>
#include <cstring>

namespace lob::protocol {
namespace {

constexpr std::array<char, 2> kMagic{'L', 'O'};

/// Widen a byte without sign extension. `char` is signed on every platform this
/// builds on, so casting a byte >= 0x80 straight to a wider unsigned type
/// sign-extends it -- the same trap that bit the journal reader.
constexpr std::uint32_t byte(char c) noexcept {
  return static_cast<std::uint32_t>(static_cast<unsigned char>(c));
}
constexpr std::uint64_t ubyte(char c) noexcept {
  return static_cast<std::uint64_t>(static_cast<unsigned char>(c));
}

/// Bounds-checked little-endian reader with a sticky failure flag, so a short
/// buffer fails the whole decode instead of yielding half-parsed fields.
class Reader {
 public:
  explicit Reader(std::string_view b) : b_(b) {}

  std::uint8_t u8() {
    if (!need(1)) {
      return 0;
    }
    ++p_;
    return ubyte(b_[p_ - 1]) == 0 ? 0 : static_cast<std::uint8_t>(b_[p_ - 1]);
  }
  std::uint32_t u32() {
    if (!need(4)) {
      return 0;
    }
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= byte(b_[p_ + static_cast<std::size_t>(i)]) << (8 * i);
    }
    p_ += 4;
    return v;
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
  std::int64_t i64() {
    return static_cast<std::int64_t>(u64());
  }
  std::string_view str() {
    const std::uint32_t n = u32();
    if (!need(n)) {
      return {};
    }
    const std::string_view v = b_.substr(p_, n);
    p_ += n;
    return v;
  }
  /// True only if every read so far stayed in bounds.
  [[nodiscard]] bool ok() const {
    return ok_;
  }
  /// Bytes consumed so far, so the caller can require an exact fit.
  [[nodiscard]] std::size_t consumed() const {
    return p_;
  }

 private:
  bool need(std::size_t n) {
    if (!ok_ || p_ + n > b_.size()) {
      ok_ = false;
      return false;
    }
    return true;
  }
  std::string_view b_;
  std::size_t p_ = 0;
  bool ok_ = true;
};

/// True for a defined message type. Derived from the enum's own bounds rather
/// than a hardcoded last value, so adding a type cannot silently make the
/// decoder reject it.
[[nodiscard]] bool known_type(std::uint8_t v) noexcept {
  return v >= static_cast<std::uint8_t>(MessageType::Hello) &&
         v <= static_cast<std::uint8_t>(MessageType::MarketDataIncrement);
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello:
      return "hello";
    case MessageType::Authenticate:
      return "authenticate";
    case MessageType::NewOrder:
      return "new_order";
    case MessageType::Cancel:
      return "cancel";
    case MessageType::Replace:
      return "replace";
    case MessageType::MassCancel:
      return "mass_cancel";
    case MessageType::Subscribe:
      return "subscribe";
    case MessageType::Heartbeat:
      return "heartbeat";
    case MessageType::Goodbye:
      return "goodbye";
    case MessageType::MarketDataSnapshot:
      return "market_data_snapshot";
    case MessageType::MarketDataIncrement:
      return "market_data_increment";
  }
  return "unknown";
}

std::string_view to_string(DecodeError error) noexcept {
  switch (error) {
    case DecodeError::None:
      return "none";
    case DecodeError::BadMagic:
      return "bad_magic";
    case DecodeError::BadVersion:
      return "bad_version";
    case DecodeError::UnknownType:
      return "unknown_type";
    case DecodeError::LengthTooLarge:
      return "length_too_large";
    case DecodeError::CrcMismatch:
      return "crc_mismatch";
    case DecodeError::Truncated:
      return "truncated";
    case DecodeError::MalformedPayload:
      return "malformed_payload";
  }
  return "unknown";
}

void PayloadWriter::u32(std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    bytes_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
  }
}
void PayloadWriter::u64(std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    bytes_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFU));
  }
}
void PayloadWriter::i64(std::int64_t v) {
  u64(static_cast<std::uint64_t>(v));
}

std::vector<std::uint8_t> encode_payload(const Inbound& m) {
  PayloadWriter w;
  switch (m.type) {
    case MessageType::Hello:
      w.bytes(m.session_id);
      break;
    case MessageType::Authenticate:
      w.bytes(m.session_id);
      w.bytes(m.participant_token);
      break;
    case MessageType::NewOrder: {
      const auto& n = m.new_order;
      w.u64(n.order_id.value);
      w.u64(n.participant.value);
      w.u32(n.symbol.value);
      w.u8(static_cast<std::uint8_t>(n.side));
      w.u8(static_cast<std::uint8_t>(n.type));
      w.u8(static_cast<std::uint8_t>(n.tif));
      w.i64(n.price.value);
      w.i64(n.trigger_price.value);
      w.i64(n.quantity.value);
      w.i64(n.display_qty.value);
      w.u8(n.post_only ? 1U : 0U);
      break;
    }
    case MessageType::Cancel: {
      const auto& c = m.cancel;
      w.u64(c.order_id.value);
      w.u64(c.participant.value);
      w.u32(c.symbol.value);
      break;
    }
    case MessageType::Replace: {
      const auto& r = m.replace;
      w.u64(r.order_id.value);
      w.u64(r.participant.value);
      w.u32(r.symbol.value);
      w.i64(r.new_price.value);
      w.i64(r.new_quantity.value);
      break;
    }
    case MessageType::MassCancel:
      w.u64(m.mass_cancel.participant.value);
      break;
    case MessageType::Subscribe:
      w.u32(m.subscribe_symbol.value);
      break;
    case MessageType::Heartbeat:
    case MessageType::Goodbye:
      break;
    case MessageType::MarketDataSnapshot: {
      const auto& k = m.snapshot;
      w.u64(k.sequence);
      w.u32(k.symbol.value);
      w.u8(k.has_bid ? 1U : 0U);
      w.i64(k.best_bid);
      w.i64(k.best_bid_qty);
      w.u32(k.best_bid_orders);
      w.u8(k.has_ask ? 1U : 0U);
      w.i64(k.best_ask);
      w.i64(k.best_ask_qty);
      w.u32(k.best_ask_orders);
      break;
    }
    case MessageType::MarketDataIncrement: {
      const auto& k = m.increment;
      w.u64(k.sequence);
      w.u32(k.symbol.value);
      w.u8(static_cast<std::uint8_t>(k.side));
      w.u8(static_cast<std::uint8_t>(k.action));
      w.i64(k.price);
      w.i64(k.quantity);
      break;
    }
  }
  return w.payload();
}

bool encode(MessageType type, std::string_view payload, std::vector<std::uint8_t>& out) {
  if (payload.size() > kMaxPayload) {
    return false;
  }
  const std::size_t start = out.size();
  out.push_back(static_cast<std::uint8_t>(kMagic[0]));
  out.push_back(static_cast<std::uint8_t>(kMagic[1]));
  out.push_back(kProtocolVersion);
  out.push_back(static_cast<std::uint8_t>(type));
  const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((len >> (8 * i)) & 0xFFU));
  }
  out.insert(out.end(), payload.begin(), payload.end());
  // CRC over everything after the magic, including the payload.
  const std::uint32_t crc = crc32c(out.data() + start + 2, out.size() - start - 2, 0);
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((crc >> (8 * i)) & 0xFFU));
  }
  return true;
}

bool encode(const Inbound& m, std::vector<std::uint8_t>& out) {
  const std::vector<std::uint8_t> payload = encode_payload(m);
  return encode(
      m.type, std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()), out);
}

std::optional<Inbound> decode_payload(MessageType type, std::string_view payload,
                                      DecodeError& error) {
  Inbound m;
  m.type = type;
  Reader r{payload};
  switch (type) {
    case MessageType::Hello:
      m.session_id = std::string(r.str());
      break;
    case MessageType::Authenticate:
      m.session_id = std::string(r.str());
      m.participant_token = std::string(r.str());
      break;
    case MessageType::NewOrder: {
      auto& n = m.new_order;
      n.seq = Sequence{0};  // stamped by the sequencer, never taken from the wire
      n.ts = Timestamp{0};
      n.order_id.value = r.u64();
      n.participant.value = r.u64();
      n.symbol.value = r.u32();
      n.side = static_cast<Side>(r.u8());
      n.type = static_cast<OrderType>(r.u8());
      n.tif = static_cast<TimeInForce>(r.u8());
      n.price = Price{r.i64()};
      n.trigger_price = Price{r.i64()};
      n.quantity = Quantity{r.i64()};
      n.display_qty = Quantity{r.i64()};
      n.post_only = r.u8() != 0U;
      break;
    }
    case MessageType::Cancel: {
      auto& c = m.cancel;
      c.seq = Sequence{0};
      c.ts = Timestamp{0};
      c.order_id.value = r.u64();
      c.participant.value = r.u64();
      c.symbol.value = r.u32();
      break;
    }
    case MessageType::Replace: {
      auto& x = m.replace;
      x.seq = Sequence{0};
      x.ts = Timestamp{0};
      x.order_id.value = r.u64();
      x.participant.value = r.u64();
      x.symbol.value = r.u32();
      x.new_price = Price{r.i64()};
      x.new_quantity = Quantity{r.i64()};
      break;
    }
    case MessageType::MassCancel:
      m.mass_cancel.seq = Sequence{0};
      m.mass_cancel.ts = Timestamp{0};
      m.mass_cancel.participant.value = r.u64();
      break;
    case MessageType::Subscribe:
      m.subscribe_symbol.value = r.u32();
      break;
    case MessageType::Heartbeat:
    case MessageType::Goodbye:
      break;
    case MessageType::MarketDataSnapshot: {
      auto& k = m.snapshot;
      k.sequence = r.u64();
      k.symbol.value = r.u32();
      k.has_bid = r.u8() != 0U;
      k.best_bid = r.i64();
      k.best_bid_qty = r.i64();
      k.best_bid_orders = r.u32();
      k.has_ask = r.u8() != 0U;
      k.best_ask = r.i64();
      k.best_ask_qty = r.i64();
      k.best_ask_orders = r.u32();
      break;
    }
    case MessageType::MarketDataIncrement: {
      auto& k = m.increment;
      k.sequence = r.u64();
      k.symbol.value = r.u32();
      k.side = static_cast<Side>(r.u8());
      k.action = static_cast<UpdateAction>(r.u8());
      k.price = r.i64();
      k.quantity = r.i64();
      break;
    }
    default:
      error = DecodeError::UnknownType;
      return std::nullopt;
  }
  // Trailing bytes mean the sender and receiver disagree about the layout, which
  // is a protocol error rather than something to ignore -- silently ignoring it
  // is how version skew turns into a field-level misread.
  if (!r.ok() || r.consumed() != payload.size()) {
    error = DecodeError::MalformedPayload;
    return std::nullopt;
  }
  error = DecodeError::None;
  return m;
}

std::optional<Inbound> decode(std::string_view b, std::size_t& consumed, DecodeError& error) {
  consumed = 0;
  if (b.size() < kHeaderLen) {
    error = DecodeError::Truncated;
    return std::nullopt;
  }
  if (b[0] != kMagic[0] || b[1] != kMagic[1]) {
    error = DecodeError::BadMagic;
    return std::nullopt;
  }
  if (static_cast<std::uint8_t>(b[2]) != kProtocolVersion) {
    error = DecodeError::BadVersion;
    return std::nullopt;
  }
  const auto raw_type = static_cast<std::uint8_t>(b[3]);
  if (!known_type(raw_type)) {
    error = DecodeError::UnknownType;
    return std::nullopt;
  }
  const std::uint32_t len =
      byte(b[4]) | (byte(b[5]) << 8) | (byte(b[6]) << 16) | (byte(b[7]) << 24);
  if (len > kMaxPayload) {
    error = DecodeError::LengthTooLarge;
    return std::nullopt;
  }
  const std::size_t total = kHeaderLen + len + kCrcLen;
  if (b.size() < total) {
    error = DecodeError::Truncated;
    return std::nullopt;
  }
  const std::size_t crc_at = kHeaderLen + len;
  std::uint32_t want = 0;
  for (int i = 0; i < 4; ++i) {
    want |= byte(b[crc_at + static_cast<std::size_t>(i)]) << (8 * i);
  }
  // CRC is checked before the payload is parsed, so a corrupt frame is never
  // handed to the field decoder.
  const std::uint32_t got = crc32c(b.data() + 2, crc_at - 2, 0);
  if (want != got) {
    error = DecodeError::CrcMismatch;
    return std::nullopt;
  }
  const auto type = static_cast<MessageType>(raw_type);
  auto message = decode_payload(type, b.substr(kHeaderLen, len), error);
  if (!message) {
    return std::nullopt;
  }
  consumed = total;
  return message;
}

std::optional<Inbound> FrameReader::next(DecodeError& error) {
  const std::string_view view =
      buffer_.empty()
          ? std::string_view{}
          : std::string_view(reinterpret_cast<const char*>(buffer_.data()), buffer_.size());
  std::size_t consumed = 0;
  auto message = decode(view, consumed, error);
  if (!message) {
    // Truncated means "not yet", which is not an error -- the caller should wait
    // for more bytes. Anything else is a real protocol error and the caller is
    // expected to drop the connection.
    return std::nullopt;
  }
  buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
  return message;
}

}  // namespace lob::protocol

// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
