// Wire protocol: message framing and the request codec.
//
// Everything a client can send is untrusted, so the decoder's contract is that
// no input -- truncated, over-long, random bytes, wrong version -- can make it
// read out of bounds or accept a message. Every read is bounds-checked against a
// sticky failure flag, so a short buffer fails the whole decode rather than
// yielding half-parsed fields.
//
// Frame layout (little endian, so the wire matches the journal's on-disk format
// and one CRC implementation serves both):
//
//   magic    : 2 bytes, "LO"
//   version  : 1 byte,  kProtocolVersion
//   type     : 1 byte,  MessageType
//   length   : 4 bytes, payload length, capped by kMaxPayload
//   payload  : `length` bytes
//   crc32c   : 4 bytes,  CRC-32C over version, type, length and payload
//
// The CRC trails the payload, as it does in the journal, so one framing shape
// and one CRC routine serve both and there is a single place to get it wrong.
//
// The CRC is verified before the payload is looked at, so a corrupt frame is
// rejected without ever being parsed.
//
// A deliberately small, fixed protocol. Sequence, timestamp and symbol are
// stamped by the sequencer rather than trusted from the wire: a client-supplied
// sequence would let any participant choose its own ordering relative to others,
// which is not something a venue should hand out.

#pragma once

#include "core/engine.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lob::protocol {

constexpr std::uint8_t kProtocolVersion = 1;
/// Hard cap on a single payload. Bounds the work a hostile peer can force per
/// frame, and means a garbage length field cannot ask for a huge allocation.
constexpr std::uint32_t kMaxPayload = 64UL * 1024UL;
constexpr std::size_t kHeaderLen = 2 + 1 + 1 + 4;  // magic, version, type, length
constexpr std::size_t kCrcLen = 4;

enum class MessageType : std::uint8_t {
  Hello = 1,
  Authenticate = 2,
  NewOrder = 3,
  Cancel = 4,
  Replace = 5,
  MassCancel = 6,
  Subscribe = 7,
  Heartbeat = 8,
  Goodbye = 9,
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;

/// Why a frame was refused. Distinguishing these matters operationally: a
/// ProtocolError means a buggy or hostile client, a CRC failure means a network
/// problem or a middlebox, and they should not be alerted on identically.
enum class DecodeError : std::uint8_t {
  None = 0,
  BadMagic,
  BadVersion,
  UnknownType,
  LengthTooLarge,
  CrcMismatch,
  Truncated,
  MalformedPayload,
};

[[nodiscard]] std::string_view to_string(DecodeError error) noexcept;

/// A decoded inbound message. One variant per request type, so the session
/// machine dispatches with a single visit instead of a chain of ifs.
struct Inbound {
  MessageType type{MessageType::Hello};
  NewOrderRequest new_order{};
  CancelRequest cancel{};
  ReplaceRequest replace{};
  MassCancelRequest mass_cancel{};
  SymbolId subscribe_symbol{};
  std::string session_id;         ///< Hello / Authenticate only
  std::string participant_token;  ///< Authenticate only
};

/// Encode a frame. Returns false if the payload would exceed kMaxPayload, in
/// which case nothing is appended.
[[nodiscard]] bool encode(MessageType type, std::string_view payload,
                          std::vector<std::uint8_t>& out);

/// Encode an inbound message as a frame.
[[nodiscard]] bool encode(const Inbound& message, std::vector<std::uint8_t>& out);

/// Append a payload with a simple little-endian writer. Exposed so the session
/// tests can build deliberately malformed payloads without hand-rolling bytes.
class PayloadWriter {
 public:
  void u8(std::uint8_t v) {
    bytes_.push_back(v);
  }
  void u32(std::uint32_t v);
  void u64(std::uint64_t v);
  void i64(std::int64_t v);
  void bytes(std::string_view v) {
    u32(static_cast<std::uint32_t>(v.size()));
    bytes_.insert(bytes_.end(), v.begin(), v.end());
  }
  [[nodiscard]] const std::vector<std::uint8_t>& payload() const {
    return bytes_;
  }

 private:
  std::vector<std::uint8_t> bytes_;
};

/// Build the payload for one message type.
[[nodiscard]] std::vector<std::uint8_t> encode_payload(const Inbound& message);

/// Decode exactly one frame. `bytes` must begin at a frame boundary; the session
/// layer owns finding boundaries. On success `consumed` is set to the frame
/// length.
[[nodiscard]] std::optional<Inbound> decode(std::string_view bytes, std::size_t& consumed,
                                            DecodeError& error);

/// Decode the payload for one message type, already CRC-verified.
[[nodiscard]] std::optional<Inbound> decode_payload(MessageType type, std::string_view payload,
                                                    DecodeError& error);

/// Incremental frame reader: feed it whatever arrived, take whole frames out.
///
/// This is what makes partial reads correct rather than an afterthought. A TCP
/// read can land anywhere -- mid-header, mid-payload, or spanning several frames
/// -- so the buffer keeps a partial tail until the rest arrives.
class FrameReader {
 public:
  /// Append received bytes.
  void append(std::string_view bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  }

  /// Pop the next complete frame, if one is buffered. Leaves a partial frame in
  /// place for the next call.
  [[nodiscard]] std::optional<Inbound> next(DecodeError& error);

  /// True once a frame is buffered but not yet complete: the peer has sent a
  /// partial message. Used to distinguish a slow peer from a silent one.
  [[nodiscard]] bool has_partial() const noexcept {
    return !buffer_.empty();
  }

  /// Bytes held that do not yet form a whole frame.
  [[nodiscard]] std::size_t pending() const noexcept {
    return buffer_.size();
  }

  /// Drop buffered bytes. Used when the connection is being torn down after a
  /// protocol error.
  void clear() noexcept {
    buffer_.clear();
  }

  [[nodiscard]] std::size_t buffered() const noexcept {
    return buffer_.size();
  }

 private:
  std::vector<std::uint8_t> buffer_;
};

}  // namespace lob::protocol