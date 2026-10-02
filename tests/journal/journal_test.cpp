#include "journal/journal.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace lob {
namespace {

SymbolConfig cfg(std::string_view name = "XYZ") {
  SymbolConfig c;
  c.name = name;
  c.min_price = 0;
  c.max_price = 10000;
  c.tick_size = 1;
  c.lot_size = 1;
  c.max_order_qty = 10'000;
  c.max_notional = 100'000'000;
  c.max_open_orders = 2048;
  return c;
}

Engine make_engine() {
  EngineConfig ec;
  return Engine({cfg()}, ec);
}

JournalRecord rec_new(std::uint64_t seq, OrderId id, Side side, Price px, Quantity qty,
                      OrderType type = OrderType::Limit, TimeInForce tif = TimeInForce::GTC) {
  JournalRecord r;
  r.kind = RecordKind::NewOrder;
  NewOrderRequest& n = r.new_order;
  n.seq = Sequence{seq};
  n.ts = Timestamp{static_cast<std::int64_t>(seq)};
  n.symbol = SymbolId{0};
  n.order_id = id;
  ParticipantId pid{static_cast<std::uint32_t>(seq % 4 + 1)};
  n.participant = pid;
  n.side = side;
  n.type = type;
  n.tif = tif;
  n.price = px;
  n.quantity = qty;
  return r;
}

std::string to_bytes(const std::vector<JournalRecord>& rs) {
  std::string out;
  std::vector<std::uint8_t> buf;
  for (const auto& r : rs) {
    encode_record(r, buf);
    out.append(reinterpret_cast<const char*>(buf.data()), buf.size());
    buf.clear();
  }
  return out;
}

TEST(Crc32c, MatchesKnownVector) {
  // The standard CRC-32C check value for "123456789".
  const char* s = "123456789";
  EXPECT_EQ(crc32c(s, 9), 0xE3069283U);
}

TEST(Journal, RoundTripsEveryRequestType) {
  std::vector<JournalRecord> in;
  in.push_back(rec_new(1, OrderId{10}, Side::Buy, Price{100}, Quantity{5}));
  JournalRecord c;
  c.kind = RecordKind::Cancel;
  c.cancel = CancelRequest{Sequence{2}, Timestamp{2}, SymbolId{0}, OrderId{10}, ParticipantId{1}};
  in.push_back(c);
  JournalRecord p;
  p.kind = RecordKind::Replace;
  p.replace = ReplaceRequest{Sequence{3},      Timestamp{3}, SymbolId{0}, OrderId{10},
                             ParticipantId{1}, Price{101},   Quantity{7}};
  in.push_back(p);
  JournalRecord m;
  m.kind = RecordKind::MassCancel;
  m.mass_cancel = MassCancelRequest{Sequence{4}, Timestamp{4}, ParticipantId{1}};
  in.push_back(m);

  std::vector<JournalRecord> out;
  const ReplayReport rep =
      replay(to_bytes(in), [&out](const JournalRecord& r) { out.push_back(r); });
  EXPECT_FALSE(rep.truncated);
  ASSERT_EQ(out.size(), in.size());

  EXPECT_EQ(out[0].new_order.order_id, in[0].new_order.order_id);
  EXPECT_EQ(out[0].new_order.price.value, 100);
  EXPECT_EQ(out[1].cancel.order_id, OrderId{10});
  EXPECT_EQ(out[2].replace.new_price.value, 101);
  EXPECT_EQ(out[2].replace.new_quantity.value, 7);
  EXPECT_EQ(out[3].mass_cancel.participant, ParticipantId{1});
}

TEST(Journal, EveryFieldSurvivesTheRoundTrip) {
  JournalRecord r;
  r.kind = RecordKind::NewOrder;
  NewOrderRequest& n = r.new_order;
  n.seq = Sequence{1234567890123ULL};
  n.ts = Timestamp{-42424242};
  n.symbol = SymbolId{77};
  n.order_id = OrderId{987654321098ULL};
  n.participant = ParticipantId{5150};
  n.side = Side::Sell;
  n.type = OrderType::StopLimit;
  n.tif = TimeInForce::FOK;
  n.price = Price{-123456789};
  n.trigger_price = Price{-200};
  n.quantity = Quantity{-5};
  n.display_qty = Quantity{-3};
  n.post_only = true;

  JournalRecord out;
  const ReplayReport rep = replay(to_bytes({r}), [&out](const JournalRecord& x) { out = x; });
  ASSERT_FALSE(rep.truncated);
  const NewOrderRequest& m = out.new_order;
  EXPECT_EQ(m.seq.value, n.seq.value);
  EXPECT_EQ(m.ts.value, n.ts.value);
  EXPECT_EQ(m.symbol.value, n.symbol.value);
  EXPECT_EQ(m.order_id.value, n.order_id.value);
  EXPECT_EQ(m.participant.value, n.participant.value);
  EXPECT_EQ(m.side, n.side);
  EXPECT_EQ(m.type, n.type);
  EXPECT_EQ(m.tif, n.tif);
  EXPECT_EQ(m.price.value, n.price.value);
  EXPECT_EQ(m.trigger_price.value, n.trigger_price.value);
  EXPECT_EQ(m.quantity.value, n.quantity.value);
  EXPECT_EQ(m.display_qty.value, n.display_qty.value);
  EXPECT_EQ(m.post_only, n.post_only);
}

TEST(Journal, StopOrderRoundTripsTriggerAndPostOnly) {
  JournalRecord r = rec_new(1, OrderId{1}, Side::Sell, Price{0}, Quantity{3}, OrderType::StopLimit,
                            TimeInForce::GTC);
  r.new_order.trigger_price = Price{97};
  r.new_order.post_only = true;

  JournalRecord out;
  const ReplayReport rep = replay(to_bytes({r}), [&out](const JournalRecord& x) { out = x; });
  EXPECT_FALSE(rep.truncated);
  EXPECT_EQ(out.new_order.trigger_price.value, 97);
  EXPECT_TRUE(out.new_order.post_only);
  EXPECT_EQ(out.new_order.type, OrderType::StopLimit);
}

TEST(Journal, TruncatedTailIsDiscardedAndEarlierRecordsSurvive) {
  std::vector<JournalRecord> in;
  for (std::uint64_t i = 1; i <= 5; ++i) {
    in.push_back(rec_new(i, OrderId{i}, Side::Buy, Price{100}, Quantity{1}));
  }
  std::string bytes = to_bytes(in);

  // Cut mid-record, the way a crash during append leaves it.
  bytes.resize(bytes.size() - 6);

  std::size_t seen = 0;
  const ReplayReport rep = replay(bytes, [&seen](const JournalRecord&) { ++seen; });
  EXPECT_TRUE(rep.truncated);
  EXPECT_EQ(seen, 4);
}

TEST(Journal, CorruptPayloadIsDetectedByCrc) {
  std::string bytes = to_bytes({rec_new(1, OrderId{1}, Side::Buy, Price{100}, Quantity{1})});
  bytes[14] ^= 0xFF;  // flip a bit inside the payload
  std::size_t seen = 0;
  const ReplayReport rep = replay(bytes, [&seen](const JournalRecord&) { ++seen; });
  EXPECT_TRUE(rep.truncated);
  EXPECT_EQ(seen, 0);
}

TEST(Journal, CorruptLengthIsRejected) {
  std::string bytes = to_bytes({rec_new(1, OrderId{1}, Side::Buy, Price{100}, Quantity{1})});
  bytes[5] = 0xFF;  // absurd payload length
  std::size_t seen = 0;
  const ReplayReport rep = replay(bytes, [&seen](const JournalRecord&) { ++seen; });
  EXPECT_TRUE(rep.truncated);
  EXPECT_EQ(seen, 0);
}

// The test that justifies the whole approach: because the engine is
// deterministic, replaying the request log must rebuild a bit-identical book.
// If this ever fails, recovery is silently producing a different venue.
TEST(Journal, ReplayRebuildsAnIdenticalBook) {
  std::mt19937_64 rng(12345);
  std::uniform_int_distribution<int> side(0, 1);
  std::uniform_int_distribution<std::int64_t> px(95, 105);
  std::uniform_int_distribution<std::int64_t> qty(1, 20);
  std::uniform_int_distribution<std::int64_t> which(0, 99);

  Engine live = make_engine();
  std::vector<JournalRecord> log;
  std::uint64_t seq = 0;

  for (int i = 0; i < 4000; ++i) {
    ++seq;
    const int w = which(rng);
    if (w < 70 || log.empty()) {
      JournalRecord r = rec_new(seq, OrderId{seq}, side(rng) == 0 ? Side::Buy : Side::Sell,
                                Price{px(rng)}, Quantity{qty(rng)});
      log.push_back(r);
      live.submit(r.new_order);
      live.clear_events();
    } else if (w < 85) {
      JournalRecord r;
      r.kind = RecordKind::Cancel;
      const auto& prev = log[static_cast<std::size_t>(seq % log.size())];
      r.cancel = CancelRequest{Sequence{seq}, Timestamp{static_cast<std::int64_t>(seq)},
                               SymbolId{0}, OrderId{static_cast<std::uint64_t>(seq % 4000)},
                               prev.new_order.participant};
      log.push_back(r);
      live.submit(r.cancel);
      live.clear_events();
    } else {
      JournalRecord r;
      r.kind = RecordKind::Replace;
      r.replace = ReplaceRequest{Sequence{seq},     Timestamp{static_cast<std::int64_t>(seq)},
                                 SymbolId{0},       OrderId{static_cast<std::uint64_t>(seq % 4000)},
                                 ParticipantId{1},  Price{px(rng)},
                                 Quantity{qty(rng)}};
      log.push_back(r);
      live.submit(r.replace);
      live.clear_events();
    }
  }
  // A mass cancel so the log also exercises the symbol-wide path.
  JournalRecord m;
  m.kind = RecordKind::MassCancel;
  m.mass_cancel = MassCancelRequest{Sequence{++seq}, Timestamp{static_cast<std::int64_t>(seq)},
                                    ParticipantId{1}};
  log.push_back(m);
  live.submit(m.mass_cancel);

  // Control: a second engine fed the same requests directly, with no journal
  // involved. If this also diverges, the problem is engine determinism, not the
  // codec.
  Engine twin = make_engine();
  for (const auto& rec : log) {
    twin.clear_events();
    switch (rec.kind) {
      case RecordKind::NewOrder:
        twin.submit(rec.new_order);
        break;
      case RecordKind::Cancel:
        twin.submit(rec.cancel);
        break;
      case RecordKind::Replace:
        twin.submit(rec.replace);
        break;
      case RecordKind::MassCancel:
        twin.submit(rec.mass_cancel);
        break;
    }
  }
  EXPECT_EQ(twin.state_hash(), live.state_hash()) << "two engines fed identically diverge";

  Engine recovered = make_engine();
  const ReplayReport rep = replay_into_engine(to_bytes(log), recovered);

  EXPECT_FALSE(rep.truncated);
  EXPECT_EQ(rep.records_applied, log.size());
  EXPECT_EQ(recovered.state_hash(), live.state_hash());
}

}  // namespace
}  // namespace lob