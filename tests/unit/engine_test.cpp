#include "core/engine.hpp"

#include "core/validate.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace lob {
namespace {

constexpr SymbolId kSym{0};

SymbolConfig base_config(std::string_view name = "XYZ") {
  SymbolConfig cfg;
  cfg.name = name;
  cfg.min_price = 0;
  cfg.max_price = 10000;
  cfg.tick_size = 1;
  cfg.lot_size = 1;
  cfg.max_order_qty = 10'000;
  cfg.max_notional = 100'000'000;
  cfg.max_open_orders = 2048;
  return cfg;
}

std::uint64_t g_seq = 0;
std::int64_t g_ts = 0;

NewOrderRequest ord(OrderId id, Side side, Price price, Quantity qty,
                    ParticipantId pid = ParticipantId{1}, OrderType type = OrderType::Limit,
                    TimeInForce tif = TimeInForce::GTC) {
  NewOrderRequest r;
  r.seq = Sequence{++g_seq};
  r.ts = Timestamp{++g_ts};
  r.symbol = kSym;
  r.order_id = id;
  r.participant = pid;
  r.side = side;
  r.type = type;
  r.tif = tif;
  r.price = price;
  r.quantity = qty;
  return r;
}

/// Collect the event stream as strings, which is exactly what the differential
/// and golden tests compare. Rendering to text here means a field-order change
/// shows up as a failure rather than passing silently.
std::vector<std::string> drain(const Engine& e) {
  std::vector<std::string> out;
  out.reserve(e.events().size());
  for (std::size_t i = 0; i < e.events().size(); ++i) {
    out.push_back(e.events()[i].to_string());
  }
  return out;
}

const Event& last(const Engine& e) {
  return e.events()[e.events().size() - 1];
}

std::size_t count_of(const Engine& e, EventType type) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < e.events().size(); ++i) {
    if (e.events()[i].type == type) {
      ++n;
    }
  }
  return n;
}

/// Maker order ids of each Trade, in event order.
std::vector<std::uint64_t> maker_ids(const Engine& e) {
  std::vector<std::uint64_t> out;
  for (std::size_t i = 0; i < e.events().size(); ++i) {
    if (e.events()[i].type == EventType::Trade) {
      out.push_back(e.events()[i].maker_order_id.value);
    }
  }
  return out;
}

/// Fill quantities of each Trade, in event order.
std::vector<std::int64_t> fill_quantities(const Engine& e) {
  std::vector<std::int64_t> out;
  for (std::size_t i = 0; i < e.events().size(); ++i) {
    if (e.events()[i].type == EventType::Trade) {
      out.push_back(e.events()[i].qty.value);
    }
  }
  return out;
}

/// Fill prices of each Trade, in event order.
std::vector<std::int64_t> fill_prices(const Engine& e) {
  std::vector<std::int64_t> out;
  for (std::size_t i = 0; i < e.events().size(); ++i) {
    if (e.events()[i].type == EventType::Trade) {
      out.push_back(e.events()[i].price.value);
    }
  }
  return out;
}

std::vector<std::string> of_type(const Engine& e, EventType type) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < e.events().size(); ++i) {
    if (e.events()[i].type == type) {
      out.push_back(e.events()[i].to_string());
    }
  }
  return out;
}

class EngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_seq = 0;
    g_ts = 0;
  }

  Engine make(std::vector<SymbolConfig> cfgs) {
    EngineConfig ec;
    return Engine(std::move(cfgs), ec);
  }

  Engine make_one() {
    return make({base_config()});
  }

  void ExpectInvariants(const Engine& e) {
    std::string_view why;
    ASSERT_TRUE(e.check_invariants(&why)) << "invariant violated: " << why;
  }
};

// ---------------------------------------------------------------------------
// Basic resting and matching
// ---------------------------------------------------------------------------

TEST_F(EngineTest, LimitOrderRestsAndAppearsInTopOfBook) {
  Engine e = make_one();
  auto r = ord(OrderId{1}, Side::Buy, Price{100}, Quantity{10});
  e.submit(r);
  EXPECT_EQ(e.events().size(), 2u);  // Accepted + BookUpdate
  EXPECT_EQ(e.events()[0].type, EventType::Accepted);
  EXPECT_EQ(e.events()[1].type, EventType::BookUpdate);
  const TopOfBook tob = e.book(kSym).top_of_book();
  ASSERT_TRUE(tob.has_bid);
  EXPECT_EQ(tob.best_bid.value, 100);
  EXPECT_EQ(tob.best_bid_qty.value, 10);
  ExpectInvariants(e);
}

TEST_F(EngineTest, MarketOrderSweepsMultipleLevelsAtMakerPrices) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{101}, Quantity{5}, ParticipantId{2}));
  e.submit(ord(OrderId{2}, Side::Sell, Price{102}, Quantity{7}, ParticipantId{2}));
  e.submit(ord(OrderId{3}, Side::Sell, Price{103}, Quantity{9}, ParticipantId{2}));
  e.clear_events();

  auto buy =
      ord(OrderId{4}, Side::Buy, Price{0}, Quantity{15}, ParticipantId{1}, OrderType::Market);
  e.submit(buy);

  // Cheapest prices fill first: 5 at 101, 7 at 102, then 3 of the 9 at 103.
  const auto fills = fill_prices(e);
  ASSERT_EQ(fills.size(), 3u);
  EXPECT_EQ(fills[0], 101);
  EXPECT_EQ(fills[1], 102);
  EXPECT_EQ(fills[2], 103);
  // The buyer was the aggressor, so only the buy side records incoming volume.
  EXPECT_EQ(e.aggressor_buy_volume(kSym), 15u);
  EXPECT_EQ(e.aggressor_sell_volume(kSym), 0u);
  const TopOfBook tob = e.book(kSym).top_of_book();
  ASSERT_TRUE(tob.has_ask);
  EXPECT_EQ(tob.best_ask_qty.value, 6);  // 9 at 103 less the 3 that traded
  ExpectInvariants(e);
}

TEST_F(EngineTest, PriceTimePriorityIsRespected) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{3}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();

  e.submit(ord(OrderId{4}, Side::Buy, Price{100}, Quantity{25}, ParticipantId{1}));

  // Priority means makers are consumed in arrival order: 10 from 1, 10 from 2,
  // then the remaining 5 from 3.
  const auto makers = maker_ids(e);
  ASSERT_EQ(makers.size(), 3u);
  EXPECT_EQ(makers[0], 1u);
  EXPECT_EQ(makers[1], 2u);
  EXPECT_EQ(makers[2], 3u);
  // And the last fill is only the 5 that was left on maker 3.
  const auto quantities = fill_quantities(e);
  ASSERT_EQ(quantities.size(), 3u);
  EXPECT_EQ(quantities[0], 10);
  EXPECT_EQ(quantities[1], 10);
  EXPECT_EQ(quantities[2], 5);
  ExpectInvariants(e);
}

// ---------------------------------------------------------------------------
// Time in force
// ---------------------------------------------------------------------------

TEST_F(EngineTest, IocFillsWhatItCanAndCancelsTheRest) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{4}, ParticipantId{2}));
  e.clear_events();

  e.submit(ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}, OrderType::Limit,
               TimeInForce::IOC));
  EXPECT_EQ(count_of(e, EventType::Trade), 1u);
  const auto cancels = of_type(e, EventType::Cancelled);
  ASSERT_EQ(cancels.size(), 1u);
  EXPECT_NE(cancels[0].find("immediate_or_cancel"), std::string::npos);
  ExpectInvariants(e);
}

TEST_F(EngineTest, FokFillsEntirelyOrNotAtAll) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{5}, ParticipantId{2}));
  e.clear_events();

  // Not enough liquidity: must reject outright with no fills at all.
  e.submit(ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}, OrderType::Limit,
               TimeInForce::FOK));
  EXPECT_EQ(count_of(e, EventType::Rejected), 1u);
  EXPECT_EQ(last(e).reject_code, RejectCode::FokInsufficientLiquidity);
  EXPECT_EQ(count_of(e, EventType::Trade), 0u);
  // The maker must be untouched.
  EXPECT_EQ(e.book(kSym).top_of_book().best_ask_qty.value, 5);
  e.clear_events();

  // Exactly enough: fills completely, nothing left to cancel.
  e.submit(ord(OrderId{3}, Side::Buy, Price{100}, Quantity{5}, ParticipantId{1}, OrderType::Limit,
               TimeInForce::FOK));
  EXPECT_EQ(count_of(e, EventType::Rejected), 0u);
  EXPECT_EQ(count_of(e, EventType::Trade), 1u);
  EXPECT_EQ(count_of(e, EventType::Cancelled), 0u);
  ExpectInvariants(e);
}

TEST_F(EngineTest, DayOrderExpiresAtSessionEnd) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}, OrderType::Limit,
               TimeInForce::Day));
  e.submit(ord(OrderId{2}, Side::Buy, Price{99}, Quantity{10}, ParticipantId{1}, OrderType::Limit,
               TimeInForce::GTC));
  e.clear_events();

  e.on_session_end(Timestamp{1'000'000});
  const auto cancels = of_type(e, EventType::Cancelled);
  ASSERT_EQ(cancels.size(), 1u);
  EXPECT_NE(cancels[0].find("time_in_force"), std::string::npos);
  // The GTC order survives.
  const TopOfBook tob = e.book(kSym).top_of_book();
  ASSERT_TRUE(tob.has_bid);
  EXPECT_EQ(tob.best_bid.value, 99);
  ExpectInvariants(e);
}

// ---------------------------------------------------------------------------
// Post-only
// ---------------------------------------------------------------------------

TEST_F(EngineTest, CrossingPostOnlyIsRejected) {
  SymbolConfig cfg = base_config();
  cfg.post_only_action = PostOnlyAction::Reject;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();

  auto r = ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1});
  r.post_only = true;
  e.submit(r);
  EXPECT_EQ(last(e).type, EventType::Rejected);
  EXPECT_EQ(last(e).reject_code, RejectCode::WouldCrossPostOnly);
  EXPECT_EQ(count_of(e, EventType::Trade), 0u);
}

TEST_F(EngineTest, CrossingPostOnlySlidesBehindTheTouch) {
  SymbolConfig cfg = base_config();
  cfg.post_only_action = PostOnlyAction::Slide;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();

  auto r = ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1});
  r.post_only = true;
  e.submit(r);
  ASSERT_EQ(e.events()[0].type, EventType::Accepted);
  // Slid one tick below the best ask so it rests passively.
  EXPECT_EQ(e.events()[0].price.value, 99);
  EXPECT_FALSE(e.book(kSym).top_of_book().crossed());
  EXPECT_EQ(count_of(e, EventType::Trade), 0u);
  const TopOfBook tob = e.book(kSym).top_of_book();
  ASSERT_TRUE(tob.has_bid);
  EXPECT_EQ(tob.best_bid.value, 99);
  ExpectInvariants(e);
}

TEST_F(EngineTest, PostOnlyMarketOrderIsInvalid) {
  Engine e = make_one();
  auto r = ord(OrderId{1}, Side::Buy, Price{0}, Quantity{10}, ParticipantId{1}, OrderType::Market);
  r.post_only = true;
  e.submit(r);
  EXPECT_EQ(last(e).reject_code, RejectCode::InvalidOrderType);
}

TEST_F(EngineTest, PostOnlyWithFokIsInvalidTimeInForce) {
  Engine e = make_one();
  auto r = ord(OrderId{1}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}, OrderType::Limit,
               TimeInForce::FOK);
  r.post_only = true;
  e.submit(r);
  EXPECT_EQ(last(e).reject_code, RejectCode::InvalidTimeInForce);
}

// ---------------------------------------------------------------------------
// Iceberg
// ---------------------------------------------------------------------------

TEST_F(EngineTest, IcebergShowsOnlyItsDisplayedSlice) {
  Engine e = make_one();
  auto r = ord(OrderId{1}, Side::Sell, Price{100}, Quantity{100}, ParticipantId{2});
  r.display_qty = Quantity{10};
  e.submit(r);
  // The level aggregate covers the whole order, including the reserve.
  EXPECT_EQ(e.book(kSym).top_of_book().best_ask_qty.value, 100);
  ExpectInvariants(e);
}

TEST_F(EngineTest, IcebergReplenishLosesPriority) {
  Engine e = make_one();
  auto iceberg = ord(OrderId{1}, Side::Sell, Price{100}, Quantity{100}, ParticipantId{2});
  iceberg.display_qty = Quantity{10};
  e.submit(iceberg);
  // A second order joins behind the iceberg's first slice.
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{50}, ParticipantId{3}));
  e.clear_events();

  // Take 10: exactly one iceberg slice.
  e.submit(ord(OrderId{3}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  const auto trades = of_type(e, EventType::Trade);
  ASSERT_EQ(trades.size(), 1u);
  // Priority moved to order 2, so the iceberg is now at the back.
  EXPECT_EQ(e.book(kSym).top_of_book().best_ask_orders, 2u);
  const OrderIndex head = e.book(kSym).best_ask_order();
  ASSERT_NE(head, kNullOrder);
  EXPECT_EQ(e.book(kSym)[head].order_id.value, 2u);
  ExpectInvariants(e);
}

TEST_F(EngineTest, IcebergReserveIsUnreachableUntilPriorityIsRegained) {
  Engine e = make_one();
  auto iceberg = ord(OrderId{1}, Side::Sell, Price{100}, Quantity{30}, ParticipantId{2});
  iceberg.display_qty = Quantity{10};
  e.submit(iceberg);
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{15}, ParticipantId{3}));
  e.clear_events();

  // 25 asked for. Order 2 (15) fills next, then one iceberg slice (10).
  e.submit(ord(OrderId{3}, Side::Buy, Price{100}, Quantity{25}, ParticipantId{1}));
  // The iceberg has queue priority, so it fills one slice first, then loses its
  // place to the order behind it.
  const auto makers = maker_ids(e);
  ASSERT_EQ(makers.size(), 2u);
  EXPECT_EQ(makers[0], 1u);
  EXPECT_EQ(makers[1], 2u);
  // Iceberg still has 20 left, having shown and consumed one slice.
  EXPECT_EQ(e.book(kSym).top_of_book().best_ask_qty.value, 20);
  ExpectInvariants(e);
}

TEST_F(EngineTest, IcebergDisplayMustBeSmallerThanTotal) {
  Engine e = make_one();
  auto r = ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2});
  r.display_qty = Quantity{10};
  e.submit(r);
  EXPECT_EQ(last(e).reject_code, RejectCode::InvalidQuantity);
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------

TEST_F(EngineTest, ReplaceQuantityDownKeepsPriority) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{3}));
  e.clear_events();

  ReplaceRequest rr;
  rr.seq = Sequence{++g_seq};
  rr.ts = Timestamp{++g_ts};
  rr.symbol = kSym;
  rr.order_id = OrderId{1};
  rr.participant = ParticipantId{2};
  rr.new_quantity = Quantity{5};
  e.submit(rr);

  ASSERT_EQ(e.events()[0].type, EventType::Replaced);
  // Order 1 must still be at the front of the queue.
  EXPECT_EQ(e.book(kSym)[e.book(kSym).best_ask_order()].order_id.value, 1u);
  ExpectInvariants(e);
}

TEST_F(EngineTest, ReplaceQuantityUpLosesPriority) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{3}));
  e.clear_events();

  ReplaceRequest rr;
  rr.seq = Sequence{++g_seq};
  rr.ts = Timestamp{++g_ts};
  rr.symbol = kSym;
  rr.order_id = OrderId{1};
  rr.participant = ParticipantId{2};
  rr.new_quantity = Quantity{20};
  e.submit(rr);

  ASSERT_EQ(e.events()[0].type, EventType::Replaced);
  EXPECT_EQ(e.book(kSym)[e.book(kSym).best_ask_order()].order_id.value, 2u);
  ExpectInvariants(e);
}

TEST_F(EngineTest, ReplacePriceChangeLosesPriority) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{2}, Side::Sell, Price{99}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{3}, Side::Sell, Price{99}, Quantity{10}, ParticipantId{3}));
  e.clear_events();

  // Move order 1 down to the 99 level, where two orders are already queued
  // ahead of it. A price change must cost it its place at the back.
  ReplaceRequest rr;
  rr.seq = Sequence{++g_seq};
  rr.ts = Timestamp{++g_ts};
  rr.symbol = kSym;
  rr.order_id = OrderId{1};
  rr.participant = ParticipantId{2};
  rr.new_price = Price{99};
  rr.new_quantity = Quantity{10};  // restate the total; 0 would mean cancel
  e.submit(rr);

  ASSERT_EQ(e.events()[0].type, EventType::Replaced);
  const Book& book = e.book(kSym);
  const LevelIndex lvl = book.best_ask_level();
  ASSERT_NE(lvl, kNullLevel);
  EXPECT_EQ(book.price_of(lvl).value, 99);
  EXPECT_EQ(book.level_at(lvl).order_count, 3u);
  // Order 1 is now last in the 99 queue, behind the two that were resting there.
  OrderIndex last_idx = book.level_at(lvl).head;
  while (book[last_idx].next != kNullOrder) {
    last_idx = book[last_idx].next;
  }
  EXPECT_EQ(book[last_idx].order_id.value, 1u);
  // The 100 level it left is now empty.
  EXPECT_FALSE(book.top_of_book().crossed());
  ExpectInvariants(e);
}

TEST_F(EngineTest, ReplaceBelowFilledIsRejected) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{2}, Side::Buy, Price{100}, Quantity{6}, ParticipantId{1}));
  e.clear_events();

  ReplaceRequest rr;
  rr.seq = Sequence{++g_seq};
  rr.ts = Timestamp{++g_ts};
  rr.symbol = kSym;
  rr.order_id = OrderId{1};
  rr.participant = ParticipantId{2};
  rr.new_quantity = Quantity{3};
  e.submit(rr);
  EXPECT_EQ(last(e).reject_code, RejectCode::ReplaceWouldReduceBelowFilled);
}

TEST_F(EngineTest, ReplaceToZeroCancels) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();

  ReplaceRequest rr;
  rr.seq = Sequence{++g_seq};
  rr.ts = Timestamp{++g_ts};
  rr.symbol = kSym;
  rr.order_id = OrderId{1};
  rr.participant = ParticipantId{2};
  rr.new_quantity = Quantity{0};
  e.submit(rr);
  EXPECT_EQ(count_of(e, EventType::Cancelled), 1u);
  EXPECT_FALSE(e.book(kSym).top_of_book().has_ask);
  ExpectInvariants(e);
}

TEST_F(EngineTest, ReplaceThatChangesNothingIsRejected) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();

  ReplaceRequest rr;
  rr.seq = Sequence{++g_seq};
  rr.ts = Timestamp{++g_ts};
  rr.symbol = kSym;
  rr.order_id = OrderId{1};
  rr.participant = ParticipantId{2};
  rr.new_quantity = Quantity{10};
  e.submit(rr);
  EXPECT_EQ(last(e).reject_code, RejectCode::ReplaceNoPriceChange);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST_F(EngineTest, CancelRemovesRestingOrder) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();

  CancelRequest cr;
  cr.seq = Sequence{++g_seq};
  cr.ts = Timestamp{++g_ts};
  cr.symbol = kSym;
  cr.order_id = OrderId{1};
  cr.participant = ParticipantId{2};
  e.submit(cr);
  EXPECT_EQ(count_of(e, EventType::Cancelled), 1u);
  EXPECT_FALSE(e.book(kSym).top_of_book().has_ask);
  ExpectInvariants(e);
}

TEST_F(EngineTest, CancelOfFilledOrderIsUnknownOrder) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  e.clear_events();

  CancelRequest cr;
  cr.seq = Sequence{++g_seq};
  cr.ts = Timestamp{++g_ts};
  cr.symbol = kSym;
  cr.order_id = OrderId{1};
  cr.participant = ParticipantId{2};
  e.submit(cr);
  EXPECT_EQ(last(e).reject_code, RejectCode::UnknownOrder);
}

TEST_F(EngineTest, CancelOfAnotherParticipantsOrderIsUnknownOrder) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();

  CancelRequest cr;
  cr.seq = Sequence{++g_seq};
  cr.ts = Timestamp{++g_ts};
  cr.symbol = kSym;
  cr.order_id = OrderId{1};
  cr.participant = ParticipantId{99};
  e.submit(cr);
  // Reported as unknown rather than forbidden: a client must not be able to
  // probe for the existence of someone else's orders.
  EXPECT_EQ(last(e).reject_code, RejectCode::UnknownOrder);
  EXPECT_TRUE(e.book(kSym).top_of_book().has_ask);
}

TEST_F(EngineTest, MassCancelRemovesOnlyThatParticipantsOrders) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{2}, Side::Sell, Price{101}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{3}, Side::Sell, Price{102}, Quantity{10}, ParticipantId{3}));
  e.clear_events();

  MassCancelRequest mr;
  mr.seq = Sequence{++g_seq};
  mr.ts = Timestamp{++g_ts};
  mr.participant = ParticipantId{2};
  e.submit(mr);
  EXPECT_EQ(count_of(e, EventType::Cancelled), 2u);
  const TopOfBook tob = e.book(kSym).top_of_book();
  ASSERT_TRUE(tob.has_ask);
  EXPECT_EQ(tob.best_ask.value, 102);
  ExpectInvariants(e);
}

// ---------------------------------------------------------------------------
// Self-trade prevention
// ---------------------------------------------------------------------------

TEST_F(EngineTest, StpCancelOldestRemovesTheMakerAndKeepsTheTaker) {
  SymbolConfig cfg = base_config();
  cfg.stp_mode = StpMode::CancelOldest;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{1}));
  e.clear_events();

  e.submit(ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  // The maker is cancelled and nothing trades.
  EXPECT_EQ(count_of(e, EventType::Cancelled), 1u);
  EXPECT_EQ(count_of(e, EventType::Trade), 0u);
  EXPECT_FALSE(e.book(kSym).top_of_book().has_ask);
}

TEST_F(EngineTest, StpCancelNewestKeepsTheMakerAndDropsTheTaker) {
  SymbolConfig cfg = base_config();
  cfg.stp_mode = StpMode::CancelNewest;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{1}));
  e.clear_events();

  e.submit(ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  EXPECT_EQ(count_of(e, EventType::Trade), 0u);
  EXPECT_EQ(e.book(kSym).top_of_book().best_ask_qty.value, 10);
  EXPECT_FALSE(e.book(kSym).index_contains(OrderId{2}));
}

TEST_F(EngineTest, StpCancelBothRemovesMakerAndTaker) {
  SymbolConfig cfg = base_config();
  cfg.stp_mode = StpMode::CancelBoth;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{1}));
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();

  // Order 3 meets order 1, its own participant: both sides go.
  e.submit(ord(OrderId{3}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  EXPECT_EQ(count_of(e, EventType::Trade), 0u);
  EXPECT_FALSE(e.book(kSym).index_contains(OrderId{1}));
  EXPECT_FALSE(e.book(kSym).index_contains(OrderId{3}));
  // Order 2 is a different participant and is left alone.
  EXPECT_TRUE(e.book(kSym).index_contains(OrderId{2}));
  const TopOfBook tob = e.book(kSym).top_of_book();
  ASSERT_TRUE(tob.has_ask);
  EXPECT_EQ(tob.best_ask_qty.value, 10);
  ExpectInvariants(e);
}

TEST_F(EngineTest, StpDecrementAndCancelReducesTheMakerAndFillsTheTaker) {
  SymbolConfig cfg = base_config();
  cfg.stp_mode = StpMode::DecrementAndCancel;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{4}, ParticipantId{1}));
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();

  e.submit(ord(OrderId{3}, Side::Buy, Price{100}, Quantity{6}, ParticipantId{1}));
  // Maker 1 is decremented away entirely by the first fill, so the aggressor
  // moves on to maker 2 for the remaining 2.
  const auto makers = maker_ids(e);
  ASSERT_EQ(makers.size(), 2u);
  EXPECT_EQ(makers[0], 1u);
  EXPECT_EQ(makers[1], 2u);
  EXPECT_EQ(e.book(kSym).top_of_book().best_ask_qty.value, 8);
  ExpectInvariants(e);
}

TEST_F(EngineTest, StpDoesNotBlockDifferentParticipants) {
  SymbolConfig cfg = base_config();
  cfg.stp_mode = StpMode::CancelBoth;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();
  e.submit(ord(OrderId{3}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  EXPECT_EQ(count_of(e, EventType::Trade), 1u);
}

// ---------------------------------------------------------------------------
// Stops
// ---------------------------------------------------------------------------

TEST_F(EngineTest, StopOrderDoesNotOccupyLiquidityUntilTriggered) {
  Engine e = make_one();
  auto r = ord(OrderId{1}, Side::Buy, Price{0}, Quantity{10}, ParticipantId{1}, OrderType::Stop);
  r.trigger_price = Price{110};
  e.submit(r);
  // Resting in the stop book only.
  EXPECT_EQ(e.book(kSym).top_of_book().empty(), true);
  EXPECT_EQ(e.stop_book(kSym).top_of_book().best_bid.value, 110);
  ExpectInvariants(e);
}

TEST_F(EngineTest, BuyStopTriggersWhenPriceReachesTheTrigger) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  auto r = ord(OrderId{2}, Side::Buy, Price{0}, Quantity{10}, ParticipantId{1}, OrderType::Stop);
  r.trigger_price = Price{100};
  e.submit(r);
  e.clear_events();

  // A buy stop fires when the last trade reaches *or exceeds* its trigger, so a
  // trade printing exactly at 100 does trigger a 100 stop.
  e.submit(ord(OrderId{3}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  ASSERT_GE(count_of(e, EventType::StopTriggered), 1u);
  // Having triggered, the stop became a market order and worked immediately:
  // it hit the resting sell it triggered on.
  EXPECT_GE(count_of(e, EventType::Trade), 1u);
  EXPECT_TRUE(e.stop_book(kSym).top_of_book().empty());
  ExpectInvariants(e);
}

TEST_F(EngineTest, BuyStopBelowTheLastTradeIsRejectedAsWrongSide) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  // Establish a last trade at 100 first.
  e.submit(ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  e.clear_events();

  // A stop that is already through its trigger would fire on arrival; the
  // direction check rejects it rather than letting it trigger immediately.
  auto r = ord(OrderId{3}, Side::Buy, Price{0}, Quantity{10}, ParticipantId{1}, OrderType::Stop);
  r.trigger_price = Price{99};
  e.submit(r);
  EXPECT_EQ(last(e).reject_code, RejectCode::InvalidStopDirection);
}

TEST_F(EngineTest, StopLimitRestsAtItsLimitAfterTriggering) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  // Triggers at 100 but is willing to pay only 95, so once fired it rests
  // instead of lifting the offer.
  auto r =
      ord(OrderId{2}, Side::Buy, Price{95}, Quantity{10}, ParticipantId{1}, OrderType::StopLimit);
  r.trigger_price = Price{100};
  e.submit(r);
  e.clear_events();

  e.submit(ord(OrderId{3}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  ASSERT_GE(count_of(e, EventType::StopTriggered), 1u);
  const TopOfBook tob = e.book(kSym).top_of_book();
  ASSERT_TRUE(tob.has_bid);
  EXPECT_EQ(tob.best_bid.value, 95);
  EXPECT_EQ(tob.best_bid_qty.value, 10);
  ExpectInvariants(e);
}

TEST_F(EngineTest, StopCascadeTriggersInPriceOrder) {
  Engine e = make_one();
  // One resting bid at 99, so a sell printed at 99 cannot also lift a higher bid.
  e.submit(ord(OrderId{1}, Side::Buy, Price{99}, Quantity{10}, ParticipantId{2}));
  // Two buy stops at 100 that must fire together, and one at 101 that must not.
  for (OrderId id : {OrderId{10}, OrderId{11}}) {
    auto r = ord(id, Side::Buy, Price{0}, Quantity{5}, ParticipantId{1}, OrderType::Stop);
    r.trigger_price = Price{100};
    e.submit(r);
  }
  {
    auto r = ord(OrderId{12}, Side::Buy, Price{0}, Quantity{5}, ParticipantId{1}, OrderType::Stop);
    r.trigger_price = Price{101};
    e.submit(r);
  }
  e.clear_events();

  // Printing at 99 is below the 100 trigger, so nothing fires.
  e.submit(ord(OrderId{50}, Side::Sell, Price{99}, Quantity{10}, ParticipantId{1}));
  EXPECT_EQ(count_of(e, EventType::Trade), 1u);
  EXPECT_EQ(count_of(e, EventType::StopTriggered), 0u);
  e.clear_events();

  // A bid at 100 lets the next sell print at 100, which reaches the trigger.
  e.submit(ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();
  e.submit(ord(OrderId{51}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{1}));

  std::vector<std::uint64_t> triggered_ids;
  for (std::size_t i = 0; i < e.events().size(); ++i) {
    if (e.events()[i].type == EventType::StopTriggered) {
      triggered_ids.push_back(e.events()[i].order_id.value);
    }
  }
  ASSERT_EQ(triggered_ids.size(), 2u);
  // Both stops at the same trigger fire, in arrival order.
  EXPECT_EQ(triggered_ids[0], 10u);
  EXPECT_EQ(triggered_ids[1], 11u);
  // The 101 stop is untouched.
  EXPECT_TRUE(e.stop_book(kSym).index_contains(OrderId{12}));
  ExpectInvariants(e);
}

TEST_F(EngineTest, StopOnTheWrongSideOfTheLastTradeIsRejected) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.submit(ord(OrderId{2}, Side::Buy, Price{100}, Quantity{10}, ParticipantId{1}));
  e.clear_events();

  // Last trade was at 100; a buy stop must be strictly above it.
  auto r = ord(OrderId{3}, Side::Buy, Price{0}, Quantity{10}, ParticipantId{1}, OrderType::Stop);
  r.trigger_price = Price{99};
  e.submit(r);
  EXPECT_EQ(last(e).reject_code, RejectCode::InvalidStopDirection);

  auto ok = ord(OrderId{4}, Side::Buy, Price{0}, Quantity{10}, ParticipantId{1}, OrderType::Stop);
  ok.trigger_price = Price{101};
  e.submit(ok);
  EXPECT_EQ(e.events()[e.events().size() - 1].type, EventType::Accepted);
}

TEST_F(EngineTest, UnfilledStopsAreDroppedAtSessionEnd) {
  Engine e = make_one();
  auto r = ord(OrderId{1}, Side::Buy, Price{0}, Quantity{10}, ParticipantId{1}, OrderType::Stop);
  r.trigger_price = Price{110};
  e.submit(r);
  e.clear_events();
  e.on_session_end(Timestamp{1'000});
  const auto cancels = of_type(e, EventType::Cancelled);
  ASSERT_EQ(cancels.size(), 1u);
  EXPECT_NE(cancels[0].find("stop_cancelled"), std::string::npos);
  ExpectInvariants(e);
}

// ---------------------------------------------------------------------------
// Rejections and risk
// ---------------------------------------------------------------------------

TEST_F(EngineTest, DuplicateOrderIdIsRejected) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.clear_events();
  e.submit(ord(OrderId{1}, Side::Sell, Price{101}, Quantity{10}, ParticipantId{2}));
  EXPECT_EQ(last(e).reject_code, RejectCode::DuplicateOrderId);
}

TEST_F(EngineTest, UnknownSymbolIsRejected) {
  Engine e = make_one();
  auto r = ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10});
  r.symbol = SymbolId{99};
  e.submit(r);
  EXPECT_EQ(last(e).reject_code, RejectCode::UnknownSymbol);
}

TEST_F(EngineTest, ZeroAndNegativeQuantitiesAreRejected) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{0}));
  EXPECT_EQ(last(e).reject_code, RejectCode::InvalidQuantity);
  e.clear_events();
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{-5}));
  EXPECT_EQ(last(e).reject_code, RejectCode::InvalidQuantity);
  ExpectInvariants(e);
}

TEST_F(EngineTest, PriceMustBeOnTickAndQuantityOnLot) {
  SymbolConfig cfg = base_config();
  cfg.tick_size = 5;
  cfg.lot_size = 10;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{101}, Quantity{10}));
  EXPECT_EQ(last(e).reject_code, RejectCode::PriceNotOnTick);
  e.clear_events();
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{7}));
  EXPECT_EQ(last(e).reject_code, RejectCode::QuantityNotOnLot);
  e.clear_events();
  e.submit(ord(OrderId{3}, Side::Sell, Price{100}, Quantity{10}));
  EXPECT_EQ(e.events()[0].type, EventType::Accepted);
}

TEST_F(EngineTest, OrderSizeAndNotionalLimitsAreEnforced) {
  SymbolConfig cfg = base_config();
  cfg.max_order_qty = 100;
  cfg.max_notional = 1000;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{200}));
  EXPECT_EQ(last(e).reject_code, RejectCode::OrderSizeExceeded);
  e.clear_events();
  // 100 lots at 100 = 10,000 notional, over the 1,000 cap.
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{100}));
  EXPECT_EQ(last(e).reject_code, RejectCode::NotionalExceeded);
}

TEST_F(EngineTest, NotionalOverflowIsRejectedRatherThanWrapping) {
  SymbolConfig cfg = base_config();
  cfg.min_price = -1'000'000;
  cfg.max_price = 1'000'000;
  cfg.max_notional = 9'000'000'000'000'000'000LL;
  cfg.max_order_qty = 9'000'000'000'000'000'000LL;
  Engine e = make({cfg});
  // price * qty overflows int64; must be caught, not silently wrapped to a
  // small number that would then pass the notional limit.
  e.submit(ord(OrderId{1}, Side::Sell, Price{1'000'000}, Quantity{9'000'000'000'000'000'000LL}));
  EXPECT_EQ(last(e).reject_code, RejectCode::NotionalExceeded);
}

TEST_F(EngineTest, PriceCollarIsEnforced) {
  SymbolConfig cfg = base_config();
  cfg.collar_low = 90;
  cfg.collar_high = 110;
  Engine e = make({cfg});
  e.submit(ord(OrderId{1}, Side::Sell, Price{80}, Quantity{1}));
  EXPECT_EQ(last(e).reject_code, RejectCode::PriceBelowCollar);
  e.clear_events();
  e.submit(ord(OrderId{2}, Side::Sell, Price{120}, Quantity{1}));
  EXPECT_EQ(last(e).reject_code, RejectCode::PriceAboveCollar);
  e.clear_events();
  e.submit(ord(OrderId{3}, Side::Sell, Price{100}, Quantity{1}));
  EXPECT_EQ(e.events()[0].type, EventType::Accepted);
}

TEST_F(EngineTest, PriceOutsideTheSymbolDomainIsRejected) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{0}, Quantity{1}));
  EXPECT_EQ(last(e).reject_code, RejectCode::InvalidPrice);
  e.clear_events();
  e.submit(ord(OrderId{2}, Side::Sell, Price{20'000}, Quantity{1}));
  EXPECT_EQ(last(e).reject_code, RejectCode::PriceOutOfRange);
}

TEST_F(EngineTest, BookFullIsReportedRatherThanGrown) {
  SymbolConfig cfg = base_config();
  cfg.max_open_orders = 4;
  Engine e = make({cfg});
  for (std::uint64_t i = 0; i < 4; ++i) {
    e.submit(ord(OrderId{i + 1}, Side::Sell, Price{100 + static_cast<std::int64_t>(i)}, Quantity{1},
                 ParticipantId{2}));
  }
  e.clear_events();
  e.submit(ord(OrderId{99}, Side::Sell, Price{110}, Quantity{1}, ParticipantId{2}));
  EXPECT_EQ(last(e).reject_code, RejectCode::BookFull);
}

TEST_F(EngineTest, RateLimitRejectsBeyondTheAllowance) {
  SymbolConfig cfg = base_config();
  cfg.rate_limit_per_second = 3;
  Engine e = make({cfg});
  for (std::uint64_t i = 0; i < 3; ++i) {
    e.submit(ord(OrderId{i + 1}, Side::Sell, Price{100}, Quantity{1}));
  }
  e.clear_events();
  e.submit(ord(OrderId{99}, Side::Sell, Price{100}, Quantity{1}));
  EXPECT_EQ(last(e).reject_code, RejectCode::RateLimitExceeded);
  // A different participant has its own allowance.
  e.clear_events();
  e.submit(ord(OrderId{100}, Side::Sell, Price{100}, Quantity{1}, ParticipantId{7}));
  EXPECT_EQ(count_of(e, EventType::Accepted), 1u);
  EXPECT_EQ(count_of(e, EventType::Rejected), 0u);
}

TEST_F(EngineTest, RateLimitWindowResetsWithInjectedTime) {
  SymbolConfig cfg = base_config();
  cfg.rate_limit_per_second = 2;
  Engine e = make({cfg});
  g_ts = 0;
  for (std::uint64_t i = 0; i < 2; ++i) {
    auto r = ord(OrderId{i + 1}, Side::Sell, Price{100}, Quantity{1});
    r.ts = Timestamp{0};
    e.submit(r);
  }
  e.clear_events();
  auto r = ord(OrderId{50}, Side::Sell, Price{100}, Quantity{1});
  r.ts = Timestamp{0};
  e.submit(r);
  EXPECT_EQ(last(e).reject_code, RejectCode::RateLimitExceeded);
  // Same participant, new window: accepted again.
  e.clear_events();
  auto r2 = ord(OrderId{51}, Side::Sell, Price{100}, Quantity{1});
  r2.ts = Timestamp{2'000'000'000};
  e.submit(r2);
  EXPECT_EQ(count_of(e, EventType::Accepted), 1u);
  EXPECT_EQ(count_of(e, EventType::Rejected), 0u);
}

TEST_F(EngineTest, DrainingRejectsNewOrdersButAllowsCancels) {
  Engine e = make_one();
  e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  e.begin_drain();
  e.clear_events();
  e.submit(ord(OrderId{2}, Side::Sell, Price{101}, Quantity{10}, ParticipantId{2}));
  EXPECT_EQ(last(e).reject_code, RejectCode::ShuttingDown);
  e.clear_events();
  CancelRequest cr;
  cr.seq = Sequence{++g_seq};
  cr.ts = Timestamp{++g_ts};
  cr.symbol = kSym;
  cr.order_id = OrderId{1};
  cr.participant = ParticipantId{2};
  e.submit(cr);
  EXPECT_EQ(count_of(e, EventType::Cancelled), 1u);
}

// ---------------------------------------------------------------------------
// Determinism and invariants
// ---------------------------------------------------------------------------

TEST_F(EngineTest, SameInputProducesIdenticalEventStreams) {
  const auto run = [] {
    Engine e = Engine({base_config()}, EngineConfig{});
    g_seq = 0;
    g_ts = 0;
    e.submit(ord(OrderId{1}, Side::Sell, Price{100}, Quantity{5}, ParticipantId{2}));
    e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{5}, ParticipantId{3}));
    e.submit(ord(OrderId{3}, Side::Buy, Price{100}, Quantity{7}, ParticipantId{1}));
    e.submit(ord(OrderId{4}, Side::Sell, Price{103}, Quantity{9}, ParticipantId{2}));
    e.submit(ord(OrderId{5}, Side::Buy, Price{103}, Quantity{20}, ParticipantId{1},
                 OrderType::Limit, TimeInForce::IOC));
    return std::make_pair(drain(e), e.state_hash());
  };
  const auto a = run();
  const auto b = run();
  EXPECT_EQ(a.first, b.first);
  EXPECT_EQ(a.second, b.second);
}

TEST_F(EngineTest, StateHashDistinguishesDifferentOrderings) {
  const auto run = [](OrderId first, OrderId second) {
    Engine e = Engine({base_config()}, EngineConfig{});
    g_seq = 0;
    g_ts = 0;
    e.submit(ord(first, Side::Sell, Price{100}, Quantity{5}, ParticipantId{2}));
    e.submit(ord(second, Side::Sell, Price{100}, Quantity{5}, ParticipantId{2}));
    return e.state_hash();
  };
  EXPECT_NE(run(OrderId{1}, OrderId{2}), run(OrderId{2}, OrderId{1}));
}

TEST_F(EngineTest, VolumeIsConservedAcrossManySymbols) {
  std::vector<SymbolConfig> cfgs{base_config("A"), base_config("B")};
  Engine e = make(std::move(cfgs));
  for (std::uint64_t i = 0; i < 200; ++i) {
    const SymbolId sym{static_cast<std::uint32_t>(i % 2)};
    const auto price = Price{100 + static_cast<std::int64_t>(i % 7)};
    auto r1 = ord(OrderId{i * 4 + 1}, Side::Sell, price, Quantity{3}, ParticipantId{2});
    r1.symbol = sym;
    e.submit(r1);
    auto r2 = ord(OrderId{i * 4 + 2}, Side::Buy, price, Quantity{2}, ParticipantId{1});
    r2.symbol = sym;
    e.submit(r2);
  }
  ExpectInvariants(e);
  // Every resting sell is crossed by an incoming buy, so all the volume is
  // aggressor-buy. The two sides deliberately need not balance; what must hold
  // is the conservation law, which ExpectInvariants checked.
  EXPECT_GT(e.aggressor_buy_volume(SymbolId{0}), 0u);
  EXPECT_GT(e.aggressor_buy_volume(SymbolId{1}), 0u);
  EXPECT_EQ(e.aggressor_sell_volume(SymbolId{0}), 0u);
}

TEST_F(EngineTest, AggressorCrossingMultipleLevelsNeverCrossesTheBook) {
  Engine e = make_one();
  for (std::int64_t p = 100; p < 110; ++p) {
    e.submit(ord(OrderId{static_cast<std::uint64_t>(p)}, Side::Sell, Price{p}, Quantity{5},
                 ParticipantId{2}));
  }
  for (std::int64_t p = 90; p < 100; ++p) {
    e.submit(ord(OrderId{static_cast<std::uint64_t>(p) + 500}, Side::Buy, Price{p}, Quantity{5},
                 ParticipantId{3}));
  }
  e.clear_events();

  e.submit(
      ord(OrderId{9999}, Side::Buy, Price{0}, Quantity{60}, ParticipantId{1}, OrderType::Market));
  ExpectInvariants(e);
  EXPECT_FALSE(e.book(kSym).top_of_book().crossed());
  // Only 50 lots of offer exist (10 levels x 5), so the market order fills 50
  // and the remaining 10 is cancelled as an unfilled market remainder.
  EXPECT_EQ(e.aggressor_buy_volume(kSym), 50u);
  EXPECT_EQ(count_of(e, EventType::Cancelled), 1u);
}

TEST_F(EngineTest, StopOrdersAndLiquidityNeverShareAQueue) {
  Engine e = make_one();
  auto r = ord(OrderId{1}, Side::Sell, Price{0}, Quantity{10}, ParticipantId{2}, OrderType::Stop);
  r.trigger_price = Price{200};
  e.submit(r);
  e.submit(ord(OrderId{2}, Side::Sell, Price{100}, Quantity{10}, ParticipantId{2}));
  ExpectInvariants(e);
  EXPECT_EQ(e.book(kSym).top_of_book().best_ask.value, 100);
  EXPECT_EQ(e.stop_book(kSym).top_of_book().best_ask.value, 200);
}

TEST_F(EngineTest, EventSequenceNumbersAreStrictlyIncreasing) {
  Engine e = make_one();
  for (std::uint64_t i = 0; i < 50; ++i) {
    e.submit(ord(OrderId{i + 1}, Side::Sell, Price{100 + static_cast<std::int64_t>(i)}, Quantity{1},
                 ParticipantId{2}));
  }
  e.submit(
      ord(OrderId{500}, Side::Buy, Price{0}, Quantity{100}, ParticipantId{1}, OrderType::Market));
  for (std::size_t i = 1; i < e.events().size(); ++i) {
    EXPECT_LT(e.events()[i - 1].seq.value, e.events()[i].seq.value);
  }
}

}  // namespace
}  // namespace lob