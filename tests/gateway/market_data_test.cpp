#include "gateway/market_data.hpp"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace lob {
namespace {

constexpr SymbolId kSym{0};

SymbolConfig base_config() {
  SymbolConfig c;
  c.name = "XYZ";
  c.min_price = 0;
  c.max_price = 10000;
  c.tick_size = 1;
  c.lot_size = 1;
  c.max_order_qty = 10'000;
  c.max_notional = 100'000'000;
  c.max_open_orders = 2048;
  return c;
}

std::uint64_t g_seq = 0;
std::int64_t g_ts = 0;

NewOrderRequest ord(OrderId id, Side side, Price price, Quantity qty,
                    ParticipantId pid = ParticipantId{1}) {
  NewOrderRequest r;
  r.seq = Sequence{++g_seq};
  r.ts = Timestamp{++g_ts};
  r.symbol = kSym;
  r.order_id = id;
  r.participant = pid;
  r.side = side;
  r.type = OrderType::Limit;
  r.tif = TimeInForce::GTC;
  r.price = price;
  r.quantity = qty;
  return r;
}

/// Sequence of the most recent event, used to stamp a snapshot boundary.
std::uint64_t last_seq(const Engine& e) {
  return e.events().size() == 0 ? 0 : e.events()[e.events().size() - 1].seq.value;
}

/// Decode every frame in a byte buffer, so a test can assert on what a
/// subscriber would actually receive rather than on internal counters.
std::vector<protocol::Inbound> decode_all(const std::vector<std::uint8_t>& bytes) {
  std::vector<protocol::Inbound> out;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    std::size_t consumed = 0;
    protocol::DecodeError error = protocol::DecodeError::None;
    const std::string_view view(reinterpret_cast<const char*>(bytes.data() + offset),
                                bytes.size() - offset);
    const auto m = protocol::decode(view, consumed, error);
    if (!m) {
      break;
    }
    out.push_back(*m);
    offset += consumed;
  }
  return out;
}

/// A plain map-based shadow book, rebuilt purely from what a subscriber
/// received. Deliberately a different data structure from the real book so that
/// agreement between them is evidence, not tautology.
class ShadowBook {
 public:
  void apply(const protocol::SnapshotPayload& s) {
    bids_.clear();
    asks_.clear();
    if (s.has_bid) {
      bids_[s.best_bid] = s.best_bid_qty;
    }
    if (s.has_ask) {
      asks_[s.best_ask] = s.best_ask_qty;
    }
  }

  void apply(const protocol::IncrementPayload& k) {
    auto& side = k.side == Side::Buy ? bids_ : asks_;
    switch (k.action) {
      case UpdateAction::Added:
      case UpdateAction::Changed:
        if (k.quantity == 0) {
          side.erase(k.price);  // Removed
        } else {
          side[k.price] = k.quantity;
        }
        break;
      case UpdateAction::Removed:
        side.erase(k.price);
        break;
    }
  }

  [[nodiscard]] bool has_bid() const {
    return !bids_.empty();
  }
  [[nodiscard]] bool has_ask() const {
    return !asks_.empty();
  }
  [[nodiscard]] std::int64_t best_bid() const {
    return bids_.empty() ? 0 : bids_.rbegin()->first;
  }
  [[nodiscard]] std::int64_t best_ask() const {
    return asks_.empty() ? 0 : asks_.begin()->first;
  }
  [[nodiscard]] std::int64_t best_bid_qty() const {
    return bids_.empty() ? 0 : bids_.rbegin()->second;
  }
  [[nodiscard]] std::int64_t best_ask_qty() const {
    return asks_.empty() ? 0 : asks_.begin()->second;
  }

 private:
  std::map<std::int64_t, std::int64_t> bids_;
  std::map<std::int64_t, std::int64_t> asks_;
};

// ---------------------------------------------------------------------------
// Snapshot construction
// ---------------------------------------------------------------------------

TEST(MarketData, SnapshotDescribesTheCurrentTouch) {
  Engine e({base_config()}, EngineConfig{});
  e.submit(ord(OrderId{1}, Side::Buy, Price{100}, Quantity{5}));
  e.submit(ord(OrderId{2}, Side::Sell, Price{102}, Quantity{3}));

  const protocol::SnapshotPayload s = make_snapshot(kSym, e.book(kSym).top_of_book(), 42);
  EXPECT_EQ(s.sequence, 42u);
  EXPECT_EQ(s.symbol.value, 0u);
  ASSERT_TRUE(s.has_bid);
  EXPECT_EQ(s.best_bid, 100);
  EXPECT_EQ(s.best_bid_qty, 5);
  ASSERT_TRUE(s.has_ask);
  EXPECT_EQ(s.best_ask, 102);
  EXPECT_EQ(s.best_ask_qty, 3);
}

TEST(MarketData, EmptyBookSnapshotSaysSo) {
  Engine e({base_config()}, EngineConfig{});
  const protocol::SnapshotPayload s = make_snapshot(kSym, e.book(kSym).top_of_book(), 1);
  EXPECT_FALSE(s.has_bid);
  EXPECT_FALSE(s.has_ask);
}

TEST(MarketData, OnlyBookUpdatesBecomeIncrements) {
  Event e;
  e.type = EventType::BookUpdate;
  e.symbol = kSym;
  e.seq = Sequence{9};
  e.side = Side::Sell;
  e.action = UpdateAction::Changed;
  e.price = Price{101};
  e.qty = Quantity{7};
  protocol::IncrementPayload out;
  ASSERT_TRUE(as_increment(e, out));
  EXPECT_EQ(out.sequence, 9u);
  EXPECT_EQ(out.action, UpdateAction::Changed);
  EXPECT_EQ(out.price, 101);

  // Anything else must not reach the feed.
  for (EventType t : {EventType::Trade, EventType::Accepted, EventType::Rejected,
                      EventType::Cancelled, EventType::Replaced}) {
    Event other;
    other.type = t;
    protocol::IncrementPayload ignored;
    EXPECT_FALSE(as_increment(other, ignored)) << to_string(t);
  }
}

// ---------------------------------------------------------------------------
// Subscription lifecycle
// ---------------------------------------------------------------------------

TEST(MarketData, SubscribeDeliversASnapshotFirst) {
  Engine e({base_config()}, EngineConfig{});
  e.submit(ord(OrderId{1}, Side::Buy, Price{100}, Quantity{5}));
  MarketDataPublisher pub;

  ASSERT_TRUE(pub.subscribe(SessionId{7}, kSym,
                            make_snapshot(kSym, e.book(kSym).top_of_book(), last_seq(e))));
  const auto frames = decode_all(pub.outbox(SessionId{7}));
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].type, protocol::MessageType::MarketDataSnapshot);
  EXPECT_EQ(frames[0].snapshot.best_bid, 100);
}

TEST(MarketData, ResubscribingIsRefused) {
  Engine e({base_config()}, EngineConfig{});
  MarketDataPublisher pub;
  EXPECT_TRUE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 1)));
  EXPECT_FALSE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 2)))
      << "a second subscribe must not silently reset the boundary";
}

TEST(MarketData, UnsubscribeStopsDelivery) {
  Engine e({base_config()}, EngineConfig{});
  MarketDataPublisher pub;
  ASSERT_TRUE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 1)));
  std::vector<std::uint8_t> taken;
  pub.take(SessionId{1}, taken);

  ASSERT_TRUE(pub.unsubscribe(SessionId{1}, kSym));
  Event ev;
  ev.type = EventType::BookUpdate;
  ev.symbol = kSym;
  ev.seq = Sequence{50};
  pub.publish(ev);
  EXPECT_EQ(pub.outbox(SessionId{1}).size(), 0u);
  EXPECT_FALSE(pub.unsubscribe(SessionId{1}, kSym));
}

/// The property the whole design exists for: an increment at or below the
/// snapshot's sequence has already been folded into the snapshot, so
/// re-delivering it would apply the same change twice.
TEST(MarketData, IncrementsAtOrBelowTheSnapshotSequenceAreNotDelivered) {
  Engine e({base_config()}, EngineConfig{});
  e.submit(ord(OrderId{1}, Side::Buy, Price{100}, Quantity{5}));
  MarketDataPublisher pub;
  ASSERT_TRUE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 100)));
  std::vector<std::uint8_t> sink;
  pub.take(SessionId{1}, sink);  // drain the snapshot; only increments matter here

  for (std::uint64_t seq : {50u, 99u, 100u}) {  // before, and exactly at, the boundary
    Event ev;
    ev.type = EventType::BookUpdate;
    ev.symbol = kSym;
    ev.seq = Sequence{seq};
    pub.publish(ev);
  }
  EXPECT_EQ(pub.outbox(SessionId{1}).size(), 0u)
      << "pre-snapshot increments were delivered and would be double-applied";
  EXPECT_EQ(pub.delivered_through(SessionId{1}, kSym), 100u);
}

TEST(MarketData, IncrementsAfterTheSnapshotAreDeliveredInOrder) {
  Engine e({base_config()}, EngineConfig{});
  MarketDataPublisher pub;
  ASSERT_TRUE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 100)));
  std::vector<std::uint8_t> taken;
  pub.take(SessionId{1}, taken);

  for (std::uint64_t seq = 101; seq <= 110; ++seq) {
    Event ev;
    ev.type = EventType::BookUpdate;
    ev.symbol = kSym;
    ev.seq = Sequence{seq};
    ev.side = Side::Buy;
    ev.price = Price{100};
    ev.qty = Quantity{static_cast<std::int64_t>(seq)};
    pub.publish(ev);
  }
  const auto frames = decode_all(pub.outbox(SessionId{1}));
  ASSERT_EQ(frames.size(), 10u);
  for (std::size_t i = 0; i < frames.size(); ++i) {
    EXPECT_EQ(frames[i].type, protocol::MessageType::MarketDataIncrement);
    EXPECT_EQ(frames[i].increment.sequence, 101 + i);
  }
  EXPECT_EQ(pub.delivered_through(SessionId{1}, kSym), 110u);
}

TEST(MarketData, EachSubscriberHasItsOwnBoundary) {
  // Two subscribers join at different points in the same stream and must each
  // see exactly the part of it that postdates their own snapshot.
  Engine e({base_config()}, EngineConfig{});
  MarketDataPublisher pub;
  ASSERT_TRUE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 10)));
  ASSERT_TRUE(
      pub.subscribe(SessionId{2}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 20)));
  std::vector<std::uint8_t> sink;
  pub.take(SessionId{1}, sink);
  pub.take(SessionId{2}, sink);

  for (std::uint64_t seq = 11; seq <= 25; ++seq) {
    Event ev;
    ev.type = EventType::BookUpdate;
    ev.symbol = kSym;
    ev.seq = Sequence{seq};
    pub.publish(ev);
  }
  // Subscriber 1 joined at 10 so sees 11..25; subscriber 2 joined at 20 so sees
  // 21..25.
  const auto early = decode_all(pub.outbox(SessionId{1}));
  const auto late = decode_all(pub.outbox(SessionId{2}));
  EXPECT_EQ(early.size(), 15u);
  EXPECT_EQ(late.size(), 5u);
  EXPECT_EQ(early.front().increment.sequence, 11u);
  EXPECT_EQ(late.front().increment.sequence, 21u);
}

TEST(MarketData, EventsForUnsubscribedSymbolsAreNotDelivered) {
  Engine e({base_config()}, EngineConfig{});
  MarketDataPublisher pub;
  ASSERT_TRUE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 1)));
  std::vector<std::uint8_t> sink;
  pub.take(SessionId{1}, sink);  // drain the snapshot
  Event ev;
  ev.type = EventType::BookUpdate;
  ev.symbol = SymbolId{99};
  ev.seq = Sequence{5};
  pub.publish(ev);
  EXPECT_EQ(pub.outbox(SessionId{1}).size(), 0u);
}

/// The end-to-end property: feed a real engine's BookUpdate stream to a
/// subscriber and reconstruct the book from snapshot + increments. The
/// reconstructed book must match the live one. This is the check that would
/// fail if a gap or a duplicate ever crept into the boundary logic.
TEST(MarketData, ReconstructingFromSnapshotAndIncrementsMatchesTheLiveBook) {
  Engine e({base_config()}, EngineConfig{});
  MarketDataPublisher pub;

  // Build some state before anyone subscribes, so the snapshot is non-trivial.
  e.submit(ord(OrderId{1}, Side::Buy, Price{100}, Quantity{5}));
  e.submit(ord(OrderId{2}, Side::Buy, Price{99}, Quantity{7}));
  e.submit(ord(OrderId{3}, Side::Sell, Price{105}, Quantity{4}));
  const std::uint64_t boundary = last_seq(e);
  ASSERT_TRUE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), boundary)));

  // Churn the book and fan every event out as it happens.
  for (int i = 10; i <= 60; ++i) {
    const auto id = OrderId{static_cast<std::uint64_t>(i)};
    e.submit(ord(id, (i % 2 == 0) ? Side::Buy : Side::Sell, Price{100 + (i % 5)},
                 Quantity{static_cast<std::int64_t>(i % 4 + 1)},
                 ParticipantId{static_cast<std::uint32_t>(i % 3 + 1)}));
    for (std::size_t k = 0; k < e.events().size(); ++k) {
      pub.publish(e.events()[k]);
    }
    e.clear_events();
  }

  // Replay the received frames against a shadow book.
  ShadowBook shadow;
  const auto frames = decode_all(pub.outbox(SessionId{1}));
  ASSERT_FALSE(frames.empty());
  EXPECT_EQ(frames.front().type, protocol::MessageType::MarketDataSnapshot);
  shadow.apply(frames.front().snapshot);
  std::uint64_t previous = frames.front().snapshot.sequence;
  for (std::size_t i = 1; i < frames.size(); ++i) {
    ASSERT_EQ(frames[i].type, protocol::MessageType::MarketDataIncrement);
    // No duplicates and no gaps: sequences strictly increase.
    EXPECT_GT(frames[i].increment.sequence, previous) << "gap or duplicate at frame " << i;
    previous = frames[i].increment.sequence;
    shadow.apply(frames[i].increment);
  }

  // Both the best bid and best ask must agree.
  const TopOfBook live = e.book(kSym).top_of_book();
  EXPECT_EQ(shadow.has_bid(), live.has_bid);
  EXPECT_EQ(shadow.has_ask(), live.has_ask);
  if (live.has_bid) {
    EXPECT_EQ(shadow.best_bid(), live.best_bid.value);
    EXPECT_EQ(shadow.best_bid_qty(), live.best_bid_qty.value);
  }
  if (live.has_ask) {
    EXPECT_EQ(shadow.best_ask(), live.best_ask.value);
    EXPECT_EQ(shadow.best_ask_qty(), live.best_ask_qty.value);
  }
}

// ---------------------------------------------------------------------------
// Slow-consumer policy
// ---------------------------------------------------------------------------

TEST(MarketData, SubscriberThatExceedsItsCapIsDroppedNotSkipped) {
  // Dropping updates would leave the client holding a book it believes is
  // current and is not. Disconnecting forces a fresh snapshot.
  Engine e({base_config()}, EngineConfig{});
  MarketDataConfig cfg;
  cfg.max_outbox_bytes = 512;
  MarketDataPublisher pub(cfg);

  ASSERT_TRUE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 0)));
  for (std::uint64_t seq = 1; seq <= 500; ++seq) {
    Event ev;
    ev.type = EventType::BookUpdate;
    ev.symbol = kSym;
    ev.seq = Sequence{seq};
    ev.price = Price{100};
    ev.qty = Quantity{1};
    pub.publish(ev);
    if (pub.is_dropped(SessionId{1})) {
      break;
    }
  }
  EXPECT_TRUE(pub.is_dropped(SessionId{1}));
  EXPECT_EQ(pub.outbox(SessionId{1}).size(), 0u);

  // Once dropped it gets nothing further, and cannot resubscribe.
  const std::size_t after = pub.outbox(SessionId{1}).size();
  Event ev;
  ev.type = EventType::BookUpdate;
  ev.symbol = kSym;
  ev.seq = Sequence{9999};
  pub.publish(ev);
  EXPECT_EQ(pub.outbox(SessionId{1}).size(), after);
  EXPECT_FALSE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 10'000)));
}

TEST(MarketData, OneSlowSubscriberDoesNotStarveOthers) {
  Engine e({base_config()}, EngineConfig{});
  MarketDataConfig cfg;
  cfg.max_outbox_bytes = 512;
  MarketDataPublisher pub(cfg);
  ASSERT_TRUE(
      pub.subscribe(SessionId{1}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 0)));
  ASSERT_TRUE(
      pub.subscribe(SessionId{2}, kSym, make_snapshot(kSym, e.book(kSym).top_of_book(), 0)));
  // Drain the fast one so it never fills up.
  std::vector<std::uint8_t> sink;
  pub.take(SessionId{2}, sink);

  for (std::uint64_t seq = 1; seq <= 400; ++seq) {
    Event ev;
    ev.type = EventType::BookUpdate;
    ev.symbol = kSym;
    ev.seq = Sequence{seq};
    ev.price = Price{100};
    ev.qty = Quantity{1};
    pub.publish(ev);
    sink.clear();
    pub.take(SessionId{2}, sink);
  }
  EXPECT_TRUE(pub.is_dropped(SessionId{1}));
  EXPECT_FALSE(pub.is_dropped(SessionId{2}));
  EXPECT_EQ(pub.delivered_through(SessionId{2}, kSym), 400u);
}

}  // namespace
}  // namespace lob