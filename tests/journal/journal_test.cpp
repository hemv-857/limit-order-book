#include "journal/journal.hpp"

#include "util/crc32c.hpp"

#include <fcntl.h>

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

/// Slice-by-eight must agree with the byte-at-a-time definition for every length,
/// not just the ones a frame happens to use. The slice loop and the tail loop are
/// different code paths, and a CRC that disagrees on one of them corrupts frames
/// silently -- there is no error, only a reader that rejects good data.
TEST(Crc32c, SliceByEightAgreesWithTheDefinitionAtEveryLength) {
  std::vector<std::uint8_t> data(256);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>(i * 37U + 11U);
  }
  // Reference: the definition, one byte at a time, written out longhand.
  auto reference = [&data](std::size_t len) {
    std::uint32_t c = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < len; ++i) {
      c ^= data[i];
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0U ? (c >> 1U) ^ 0x82F63B78U : c >> 1U;
      }
    }
    return ~c;
  };

  for (std::size_t len = 0; len <= data.size(); ++len) {
    EXPECT_EQ(crc32c(data.data(), len, 0), reference(len)) << "length " << len;
  }
}

/// And the seed must compose the same way, since chunked callers rely on it.
TEST(Crc32c, SeedChainingMatchesOneShot) {
  std::vector<std::uint8_t> data(200);
  for (std::size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::uint8_t>(i * 91U + 7U);
  }
  const std::uint32_t one_shot = crc32c(data.data(), data.size(), 0);
  std::uint32_t chained = 0;
  std::size_t offset = 0;
  for (const std::size_t chunk : {1U, 7U, 8U, 9U, 64U, 111U}) {
    chained = crc32c(data.data() + offset, chunk, chained);
    offset += chunk;
  }
  chained = crc32c(data.data() + offset, data.size() - offset, chained);
  EXPECT_EQ(chained, one_shot);
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
  bytes[14] = static_cast<char>(bytes[14] ^ 0xFF);  // flip a bit inside the payload
  std::size_t seen = 0;
  const ReplayReport rep = replay(bytes, [&seen](const JournalRecord&) { ++seen; });
  EXPECT_TRUE(rep.truncated);
  EXPECT_EQ(seen, 0);
}

TEST(Journal, CorruptLengthIsRejected) {
  std::string bytes = to_bytes({rec_new(1, OrderId{1}, Side::Buy, Price{100}, Quantity{1})});
  bytes[5] = static_cast<char>(0xFF);  // absurd payload length
  std::size_t seen = 0;
  const ReplayReport rep = replay(bytes, [&seen](const JournalRecord&) { ++seen; });
  EXPECT_TRUE(rep.truncated);
  EXPECT_EQ(seen, 0);
}

// The test that justifies the whole approach: because the engine is
// deterministic, replaying the request log must rebuild a bit-identical book.
// If this ever fails, recovery is silently producing a different venue.
/// Regression: encode_record computed its CRC from offset 0 of the destination
/// buffer, which is only the start of the record when it is the *first* thing in
/// that buffer. A journal is a stream of records appended to one buffer, so every
/// record after the first got a CRC computed over the wrong bytes and replay
/// silently stopped at record one. The earlier tests missed it because their
/// helper cleared the buffer between records, so every record did start at zero.
TEST(Journal, EveryRecordInOneBufferHasItsOwnValidCrc) {
  constexpr std::size_t kRecords = 500;
  std::vector<std::uint8_t> one_buffer;
  for (std::uint64_t i = 1; i <= kRecords; ++i) {
    encode_record(rec_new(i, OrderId{i}, i % 2 == 0 ? Side::Buy : Side::Sell,
                          Price{static_cast<std::int64_t>(100 + i % 7)},
                          Quantity{static_cast<std::int64_t>(i % 5 + 1)}),
                  one_buffer);
  }
  const std::string bytes(reinterpret_cast<const char*>(one_buffer.data()), one_buffer.size());

  std::size_t seen = 0;
  const ReplayReport report = replay(bytes, [&seen](const JournalRecord& r) {
    EXPECT_EQ(r.new_order.order_id.value, seen + 1) << "record " << seen << " out of order";
    ++seen;
  });

  EXPECT_FALSE(report.truncated) << "a journal written by encode_record must replay cleanly";
  EXPECT_EQ(report.records_applied, kRecords)
      << "records after the first were discarded as corrupt";
  EXPECT_EQ(seen, kRecords);
  EXPECT_EQ(report.bytes_consumed, one_buffer.size());
}

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
    const auto w = static_cast<int>(which(rng));
    if (w < 70 || log.empty()) {
      JournalRecord r = rec_new(seq, OrderId{seq}, side(rng) == 0 ? Side::Buy : Side::Sell,
                                Price{px(rng)}, Quantity{qty(rng)});
      log.push_back(r);
      live.submit(r.new_order);
      live.clear_events();
    } else if (w < 85) {
      JournalRecord r;
      r.kind = RecordKind::Cancel;
      const auto& prev =
          log[static_cast<std::size_t>(seq % static_cast<std::uint64_t>(log.size()))];
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
      case RecordKind::Invalid:
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
namespace lob {

// ---------------------------------------------------------------------------
// Durability: fsync cadence and segment rollover
// ---------------------------------------------------------------------------

namespace {
JournalRecord order_rec(std::uint64_t id) {
  const auto seq = static_cast<std::uint64_t>(id);
  return rec_new(seq, OrderId{seq}, (id % 2U) == 0U ? Side::Buy : Side::Sell,
                 Price{static_cast<std::int64_t>(100U + id)}, Quantity{1});
}
std::string seg_path(const std::string& base, std::uint64_t index) {
  if (index == 0U) {
    return base;
  }
  char suffix[32];
  std::snprintf(suffix, sizeof(suffix), ".%06llu", static_cast<unsigned long long>(index));
  return base + suffix;
}
bool exists(const std::string& path) {
  return ::access(path.c_str(), F_OK) == 0;
}
}  // namespace

// A tiny segment limit must actually roll, and every segment must be readable in
// order. Recovery reads all of them, so a rolled log that loses one is a venue
// that comes up quietly missing orders.
TEST(JournalWriter, RollsToANewSegmentWhenTheCurrentOneIsFull) {
  const std::string base = "/tmp/lob_journal_rot.journal";
  for (std::uint64_t i = 0; i < 4U; ++i) {
    ::unlink(seg_path(base, i).c_str());
  }

  JournalWriter writer;
  JournalDurability durability;
  durability.segment_bytes = 512;  // force a roll every few records
  durability.fsync_every_records = 8;
  writer.set_durability(durability);
  ASSERT_TRUE(writer.open(base, false));

  std::vector<std::uint8_t> one;
  encode_record(order_rec(1), one);
  const std::uint64_t per_record = one.size();
  for (std::uint64_t i = 1; i <= 200U; ++i) {
    ASSERT_TRUE(writer.append(order_rec(i))) << "append failed at " << i;
  }
  ASSERT_TRUE(writer.close());

  EXPECT_TRUE(exists(base)) << "the first segment is missing";
  EXPECT_TRUE(exists(seg_path(base, 1U))) << "the writer never rolled a segment";
  EXPECT_GT(writer.segments().size(), 1U) << "segments() disagrees with the filesystem";

  // No segment may exceed the configured size by more than one record's slack.
  for (const std::string& path : writer.segments()) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    ASSERT_GE(fd, 0) << "missing segment " << path;
    struct stat st{};
    ASSERT_EQ(::fstat(fd, &st), 0);
    ::close(fd);
    EXPECT_LE(static_cast<std::uint64_t>(st.st_size), 512U + per_record * 2U)
        << path << " grew well past the segment limit";
  }

  // And the whole thing still replays, in order, with no torn record.
  const std::string bytes = read_all_segments(base);
  std::size_t seen = 0;
  std::uint64_t expected_id = 1;
  const ReplayReport report = replay(bytes, [&](const JournalRecord& r) {
    ASSERT_EQ(r.kind, RecordKind::NewOrder);
    ASSERT_EQ(r.new_order.order_id.value, expected_id);
    ++expected_id;
    ++seen;
  });
  EXPECT_FALSE(report.truncated) << "a rolled journal replayed with a torn record";
  EXPECT_EQ(seen, 200U);
  EXPECT_EQ(report.records_applied, 200U);

  for (const std::string& path : writer.segments()) {
    ::unlink(path.c_str());
  }
}

// read_all_segments must stop at the first gap rather than skipping past it: a
// missing segment means lost orders, and carrying on would replay a book that is
// silently missing them.
TEST(JournalWriter, ASegmentGapStopsTheRead) {
  const std::string base = "/tmp/lob_journal_gap.journal";
  ::unlink(base.c_str());
  ::unlink(seg_path(base, 1U).c_str());
  ::unlink(seg_path(base, 2U).c_str());

  JournalWriter writer;
  ASSERT_TRUE(writer.open(base, false));
  for (std::uint64_t i = 1; i <= 5U; ++i) {
    ASSERT_TRUE(writer.append(order_rec(i)));
  }
  ASSERT_TRUE(writer.close());

  // Forge a later segment out of order: segment 2 exists, segment 1 does not.
  lob::JournalWriter forge;
  ASSERT_TRUE(forge.open(seg_path(base, 2U), false));
  ASSERT_TRUE(forge.append(order_rec(99)));
  ASSERT_TRUE(forge.close());

  std::size_t seen = 0;
  (void)replay(read_all_segments(base), [&](const JournalRecord&) { ++seen; });
  EXPECT_EQ(seen, 5U) << "read_all_segments skipped a gap and read a later segment";

  ::unlink(base.c_str());
  ::unlink(seg_path(base, 2U).c_str());
}

// fsync on close is what makes a graceful restart lossless regardless of policy.
TEST(JournalWriter, CloseIsLosslessWithPeriodicFsyncDisabled) {
  const std::string base = "/tmp/lob_journal_nosync.journal";
  ::unlink(base.c_str());

  JournalWriter writer;
  JournalDurability durability;
  durability.fsync_every_records = 0;  // periodic fsync off: close() must still save
  writer.set_durability(durability);
  ASSERT_TRUE(writer.open(base, false));
  for (std::uint64_t i = 1; i <= 50U; ++i) {
    ASSERT_TRUE(writer.append(order_rec(i)));
  }
  ASSERT_TRUE(writer.close());

  std::size_t seen = 0;
  const ReplayReport report =
      replay(read_all_segments(base), [&](const JournalRecord&) { ++seen; });
  EXPECT_EQ(seen, 50U) << "close() lost records with periodic fsync disabled";
  EXPECT_EQ(report.records_applied, 50U);
  ::unlink(base.c_str());
}

}  // namespace lob
