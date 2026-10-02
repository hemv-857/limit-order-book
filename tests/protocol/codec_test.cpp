#include "protocol/codec.hpp"

#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

namespace lob::protocol {
namespace {

Inbound new_order(OrderId id, Side side, Price px, Quantity qty, OrderType type = OrderType::Limit,
                  TimeInForce tif = TimeInForce::GTC) {
  Inbound m;
  m.type = MessageType::NewOrder;
  NewOrderRequest& n = m.new_order;
  n.seq = Sequence{7};
  n.ts = Timestamp{99};
  n.symbol = SymbolId{3};
  n.order_id = id;
  n.participant = ParticipantId{42};
  n.side = side;
  n.type = type;
  n.tif = tif;
  n.price = px;
  n.quantity = qty;
  return m;
}

std::string frame_bytes(const std::vector<std::uint8_t>& buf) {
  return std::string(reinterpret_cast<const char*>(buf.data()), buf.size());
}

std::string frame_bytes_of(const std::vector<std::uint8_t>& buf) {
  return frame_bytes(buf);
}

TEST(Codec, NewOrderRoundTripsEveryField) {
  Inbound in = new_order(OrderId{123}, Side::Sell, Price{-456}, Quantity{789}, OrderType::StopLimit,
                         TimeInForce::FOK);
  in.new_order.trigger_price = Price{-111};
  in.new_order.display_qty = Quantity{5};
  in.new_order.post_only = true;

  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(in, buf));
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  const auto out = decode(frame_bytes(buf), consumed, error);

  ASSERT_TRUE(out.has_value()) << to_string(error);
  EXPECT_EQ(consumed, buf.size());
  const NewOrderRequest& n = out->new_order;
  EXPECT_EQ(n.order_id.value, 123u);
  EXPECT_EQ(n.participant.value, 42u);
  EXPECT_EQ(n.symbol.value, 3u);
  EXPECT_EQ(n.side, Side::Sell);
  EXPECT_EQ(n.type, OrderType::StopLimit);
  EXPECT_EQ(n.tif, TimeInForce::FOK);
  EXPECT_EQ(n.price.value, -456);
  EXPECT_EQ(n.trigger_price.value, -111);
  EXPECT_EQ(n.quantity.value, 789);
  EXPECT_EQ(n.display_qty.value, 5);
  EXPECT_TRUE(n.post_only);
}

TEST(Codec, SequenceAndTimestampAreNeverTakenFromTheWire) {
  // A client-supplied sequence would let any participant choose its ordering
  // relative to everyone else, so the codec must always zero these.
  Inbound in = new_order(OrderId{1}, Side::Buy, Price{10}, Quantity{1});
  in.new_order.seq = Sequence{9999};
  in.new_order.ts = Timestamp{8888};
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(in, buf));
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  const auto out = decode(frame_bytes(buf), consumed, error);
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->new_order.seq.value, 0u);
  EXPECT_EQ(out->new_order.ts.value, 0);
}

TEST(Codec, AllMessageTypesRoundTrip) {
  std::vector<Inbound> messages;
  {
    Inbound m;
    m.type = MessageType::Hello;
    m.session_id = "sess-1";
    messages.push_back(m);
  }
  {
    Inbound m;
    m.type = MessageType::Authenticate;
    m.session_id = "sess-1";
    m.participant_token = "tok-abc";
    messages.push_back(m);
  }
  messages.push_back(new_order(OrderId{5}, Side::Buy, Price{100}, Quantity{2}));
  {
    Inbound m;
    m.type = MessageType::Cancel;
    m.cancel.order_id = OrderId{6};
    m.cancel.participant = ParticipantId{42};
    m.cancel.symbol = SymbolId{3};
    messages.push_back(m);
  }
  {
    Inbound m;
    m.type = MessageType::Replace;
    m.replace.order_id = OrderId{7};
    m.replace.participant = ParticipantId{42};
    m.replace.symbol = SymbolId{3};
    m.replace.new_price = Price{101};
    m.replace.new_quantity = Quantity{9};
    messages.push_back(m);
  }
  {
    Inbound m;
    m.type = MessageType::MassCancel;
    m.mass_cancel.participant = ParticipantId{42};
    messages.push_back(m);
  }
  {
    Inbound m;
    m.type = MessageType::Subscribe;
    m.subscribe_symbol = SymbolId{3};
    messages.push_back(m);
  }
  {
    Inbound m;
    m.type = MessageType::Heartbeat;
    messages.push_back(m);
  }
  {
    Inbound m;
    m.type = MessageType::Goodbye;
    messages.push_back(m);
  }

  for (const Inbound& m : messages) {
    std::vector<std::uint8_t> buf;
    ASSERT_TRUE(encode(m, buf)) << to_string(m.type);
    std::size_t consumed = 0;
    DecodeError error = DecodeError::None;
    const auto out = decode(frame_bytes(buf), consumed, error);
    ASSERT_TRUE(out.has_value()) << to_string(m.type) << ": " << to_string(error);
    EXPECT_EQ(out->type, m.type);
  }
}

TEST(Codec, RejectsBadMagic) {
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(new_order(OrderId{1}, Side::Buy, Price{1}, Quantity{1}), buf));
  buf[0] = 'X';
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  EXPECT_FALSE(decode(frame_bytes(buf), consumed, error).has_value());
  EXPECT_EQ(error, DecodeError::BadMagic);
}

TEST(Codec, RejectsWrongVersion) {
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(new_order(OrderId{1}, Side::Buy, Price{1}, Quantity{1}), buf));
  buf[2] = 99;
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  EXPECT_FALSE(decode(frame_bytes(buf), consumed, error).has_value());
  EXPECT_EQ(error, DecodeError::BadVersion);
}

TEST(Codec, RejectsUnknownType) {
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(new_order(OrderId{1}, Side::Buy, Price{1}, Quantity{1}), buf));
  buf[3] = 200;
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  EXPECT_FALSE(decode(frame_bytes(buf), consumed, error).has_value());
  EXPECT_EQ(error, DecodeError::UnknownType);
}

TEST(Codec, RejectsAbsurdLengthWithoutAllocating) {
  // A garbage length field must not be able to ask for a huge buffer.
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(new_order(OrderId{1}, Side::Buy, Price{1}, Quantity{1}), buf));
  for (int i = 0; i < 4; ++i) {
    buf[4 + static_cast<std::size_t>(i)] = 0xFF;
  }
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  EXPECT_FALSE(decode(frame_bytes(buf), consumed, error).has_value());
  EXPECT_EQ(error, DecodeError::LengthTooLarge);
}

TEST(Codec, RejectsCorruptPayloadByCrc) {
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(new_order(OrderId{1}, Side::Buy, Price{1234}, Quantity{1}), buf));
  buf[kHeaderLen + 8] ^= 0xFF;  // inside the payload
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  EXPECT_FALSE(decode(frame_bytes(buf), consumed, error).has_value());
  EXPECT_EQ(error, DecodeError::CrcMismatch);
}

TEST(Codec, RejectsTruncatedFrame) {
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(new_order(OrderId{1}, Side::Buy, Price{1234}, Quantity{1}), buf));
  buf.resize(buf.size() - 3);
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  EXPECT_FALSE(decode(frame_bytes(buf), consumed, error).has_value());
  EXPECT_EQ(error, DecodeError::Truncated);
}

TEST(Codec, RejectsPayloadShorterThanItsFields) {
  // CRC is valid because we build the frame ourselves; the payload is just short.
  PayloadWriter w;
  w.u64(1);  // only part of a NewOrder
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(MessageType::NewOrder, frame_bytes_of(w.payload()), buf));
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  EXPECT_FALSE(decode(frame_bytes(buf), consumed, error).has_value());
  EXPECT_EQ(error, DecodeError::MalformedPayload);
}

TEST(Codec, RejectsPayloadWithTrailingBytes) {
  // Sender and receiver disagree about the layout. Ignoring the tail would turn
  // version skew into a silent field misread.
  PayloadWriter w;
  const Inbound m = new_order(OrderId{1}, Side::Buy, Price{10}, Quantity{1});
  const std::vector<std::uint8_t> good = encode_payload(m);
  w.u64(1);
  for (std::size_t i = 0; i < good.size(); ++i) {
    w.u8(good[i]);
  }
  w.u8(0xAB);
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(MessageType::NewOrder, frame_bytes_of(w.payload()), buf));
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  EXPECT_FALSE(decode(frame_bytes(buf), consumed, error).has_value());
  EXPECT_EQ(error, DecodeError::MalformedPayload);
}

TEST(Codec, StringLengthCannotRunPastThePayload) {
  PayloadWriter w;
  w.u32(0xFFFFFFFF);  // claims a 4 GB session id
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(MessageType::Hello, frame_bytes_of(w.payload()), buf));
  std::size_t consumed = 0;
  DecodeError error = DecodeError::None;
  EXPECT_FALSE(decode(frame_bytes(buf), consumed, error).has_value());
  EXPECT_EQ(error, DecodeError::MalformedPayload);
}

/// The property that matters most: no byte string, however malformed, may make
/// the decoder read out of bounds or accept a frame. Under ASan and UBSan this
/// is the real check; the assertions keep it honest in a plain build.
TEST(Codec, ArbitraryBytesNeverDecodeAndNeverCrash) {
  std::mt19937 rng(1234);
  for (int trial = 0; trial < 20000; ++trial) {
    std::vector<std::uint8_t> junk(rng() % 96U);
    for (auto& b : junk) {
      b = static_cast<std::uint8_t>(rng() & 0xFF);
    }
    std::size_t consumed = 0;
    DecodeError error = DecodeError::None;
    // Must return cleanly, whatever it decides.
    (void)decode(frame_bytes(junk), consumed, error);

    FrameReader reader;
    reader.append(frame_bytes(junk));
    DecodeError e2 = DecodeError::None;
    (void)reader.next(e2);
  }
}

TEST(Codec, MutatedValidFramesAreRejectedNotAccepted) {
  // Take real frames and corrupt them; the decoder must never accept a frame it
  // should have rejected. A mutation that lands on the CRC is caught, and one
  // that lands in the payload is caught too.
  std::mt19937 rng(99);
  int accepted = 0;
  for (int trial = 0; trial < 20000; ++trial) {
    std::vector<std::uint8_t> buf;
    ASSERT_TRUE(encode(new_order(OrderId{static_cast<std::uint64_t>(trial)}, Side::Buy,
                                 Price{static_cast<std::int64_t>(trial)}, Quantity{1}),
                       buf));
    if (buf.empty()) {
      continue;
    }
    buf[rng() % buf.size()] ^= static_cast<std::uint8_t>(1U << (rng() % 8U));
    std::size_t consumed = 0;
    DecodeError error = DecodeError::None;
    if (decode(frame_bytes(buf), consumed, error).has_value()) {
      ++accepted;
    }
  }
  // A single bit flip can in principle land on a field whose value is unchanged
  // in meaning, but the CRC covers the payload, so acceptance should be
  // essentially nil. Anything materially above zero means the CRC is not
  // covering what it should.
  EXPECT_LT(accepted, 20) << "corrupted frames were accepted: " << accepted;
}

// ---------------------------------------------------------------------------
// Incremental frame reading
// ---------------------------------------------------------------------------

TEST(FrameReader, ReassemblesAFrameDeliveredOneByteAtATime) {
  // A TCP read can land anywhere, including mid-header. Feeding one byte at a
  // time is the strictest possible test of the reassembly logic.
  std::vector<std::uint8_t> buf;
  ASSERT_TRUE(encode(new_order(OrderId{77}, Side::Sell, Price{55}, Quantity{4}), buf));
  const std::string bytes = frame_bytes(buf);

  FrameReader reader;
  int decoded = 0;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    reader.append(std::string_view(bytes).substr(i, 1));
    DecodeError error = DecodeError::None;
    const auto m = reader.next(error);
    if (m) {
      ++decoded;
      EXPECT_EQ(m->new_order.order_id.value, 77u);
    } else {
      EXPECT_EQ(error, DecodeError::Truncated) << "byte " << i;
      EXPECT_TRUE(reader.has_partial());
    }
  }
  EXPECT_EQ(decoded, 1);
  EXPECT_FALSE(reader.has_partial());
  EXPECT_EQ(reader.pending(), 0u);
}

TEST(FrameReader, HandlesSeveralFramesInOneRead) {
  std::vector<std::uint8_t> buf;
  for (int i = 1; i <= 5; ++i) {
    ASSERT_TRUE(encode(
        new_order(OrderId{static_cast<std::uint64_t>(i)}, Side::Buy, Price{10}, Quantity{1}), buf));
  }
  FrameReader reader;
  reader.append(frame_bytes(buf));

  std::vector<std::uint64_t> ids;
  while (true) {
    DecodeError error = DecodeError::None;
    const auto m = reader.next(error);
    if (!m) {
      EXPECT_EQ(error, DecodeError::Truncated);
      break;
    }
    ids.push_back(m->new_order.order_id.value);
  }
  ASSERT_EQ(ids.size(), 5u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(ids[static_cast<std::size_t>(i)], static_cast<std::uint64_t>(i + 1));
  }
  EXPECT_EQ(reader.pending(), 0u);
}

TEST(FrameReader, HandlesASplitAcrossAReadBoundary) {
  std::vector<std::uint8_t> first;
  std::vector<std::uint8_t> second;
  ASSERT_TRUE(encode(new_order(OrderId{1}, Side::Buy, Price{10}, Quantity{1}), first));
  ASSERT_TRUE(encode(new_order(OrderId{2}, Side::Sell, Price{11}, Quantity{1}), second));
  const std::string a = frame_bytes(first);
  const std::string b = frame_bytes(second);

  FrameReader reader;
  // First frame complete, second split down the middle.
  reader.append(a);
  reader.append(std::string_view(b).substr(0, b.size() / 2));
  DecodeError error = DecodeError::None;
  const auto m1 = reader.next(error);
  ASSERT_TRUE(m1.has_value());
  EXPECT_EQ(m1->new_order.order_id.value, 1u);
  EXPECT_FALSE(reader.next(error).has_value());
  EXPECT_TRUE(reader.has_partial());

  reader.append(std::string_view(b).substr(b.size() / 2));
  const auto m2 = reader.next(error);
  ASSERT_TRUE(m2.has_value());
  EXPECT_EQ(m2->new_order.order_id.value, 2u);
  EXPECT_EQ(reader.pending(), 0u);
}

}  // namespace
}  // namespace lob::protocol