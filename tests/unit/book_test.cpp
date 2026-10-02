#include "core/book.hpp"

#include "book_fixture.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace lob {
namespace {

// A 0..1000 price domain with 64+1 levels so the bitmap word boundary at bit 63
// is inside the tested range rather than just past its edge.
constexpr std::int64_t kMin = 0;
constexpr std::int64_t kMax = 1000;

SymbolConfig test_config() {
  SymbolConfig cfg;
  cfg.name = "TEST";
  cfg.min_price = kMin;
  cfg.max_price = kMax;
  cfg.tick_size = 1;
  cfg.lot_size = 1;
  cfg.max_open_orders = 4096;
  return cfg;
}

/// Insert an order the way the engine will: take a slot, populate it, register
/// the id, then link it into the level.
OrderIndex insert_order(Book& book, OrderId id, Side side, Price price, Quantity qty,
                        Sequence seq) {
  const OrderIndex idx = book.arena().allocate();
  EXPECT_NE(idx, kNullOrder);
  if (idx == kNullOrder) {
    return kNullOrder;
  }
  Order& o = book.arena()[idx];
  o.order_id = id;
  o.side = side;
  o.price = price;
  o.total_qty = qty;
  o.leaves_qty = qty;
  o.filled_qty = Quantity{0};
  o.arrival_seq = seq;
  o.level = book.index_of(price);
  EXPECT_TRUE(book.id_index().insert(id, idx, o.generation));
  EXPECT_TRUE(book.add_to_queue(idx));
  return idx;
}

bool cancel_order(Book& book, OrderId id) {
  const OrderIndex idx = book.find(id);
  if (idx == kNullOrder) {
    return false;
  }
  book.remove_from_queue(idx);
  book.id_index().erase(id);
  book.arena().release(idx);
  return true;
}

class BookTest : public ::testing::Test {
 protected:
  BookTest() : fixture_(test_config()), book_(fixture_.book) {}

  void ExpectInvariants() {
    std::string_view why;
    ASSERT_TRUE(book_.check_invariants(&why)) << "invariant violated: " << why;
  }

  testing::BookFixture fixture_;
  Book& book_;
};

// ---------------------------------------------------------------------------
// Construction and domain
// ---------------------------------------------------------------------------

TEST_F(BookTest, EmptyBookHasNoExtremes) {
  EXPECT_EQ(book_.best_bid_level(), kNullLevel);
  EXPECT_EQ(book_.best_ask_level(), kNullLevel);
  EXPECT_EQ(book_.best_bid_order(), kNullOrder);
  EXPECT_EQ(book_.best_ask_order(), kNullOrder);
  EXPECT_TRUE(book_.top_of_book().empty());
  ExpectInvariants();
}

TEST_F(BookTest, PriceIndexRoundTrips) {
  for (std::int64_t p = kMin; p <= kMax; p += 137) {
    const Price price{p};
    EXPECT_EQ(book_.price_of(book_.index_of(price)).value, p);
    EXPECT_TRUE(book_.in_domain(price));
  }
  EXPECT_FALSE(book_.in_domain(Price{kMin - 1}));
  EXPECT_FALSE(book_.in_domain(Price{kMax + 1}));
}

// ---------------------------------------------------------------------------
// Extrema
// ---------------------------------------------------------------------------

TEST_F(BookTest, BestBidAndAskTrackExtremes) {
  insert_order(book_, OrderId{1}, Side::Buy, Price{100}, Quantity{10}, Sequence{1});
  insert_order(book_, OrderId{2}, Side::Buy, Price{99}, Quantity{10}, Sequence{2});
  insert_order(book_, OrderId{3}, Side::Sell, Price{200}, Quantity{10}, Sequence{3});
  insert_order(book_, OrderId{4}, Side::Sell, Price{201}, Quantity{10}, Sequence{4});

  const TopOfBook tob = book_.top_of_book();
  EXPECT_TRUE(tob.has_bid);
  EXPECT_TRUE(tob.has_ask);
  EXPECT_EQ(tob.best_bid.value, 100);
  EXPECT_EQ(tob.best_ask.value, 200);
  EXPECT_EQ(tob.best_bid_qty.value, 10);
  EXPECT_FALSE(tob.crossed());
  ExpectInvariants();
}

TEST_F(BookTest, RemovingBestBidExposesNextLevel) {
  insert_order(book_, OrderId{1}, Side::Buy, Price{100}, Quantity{5}, Sequence{1});
  insert_order(book_, OrderId{2}, Side::Buy, Price{95}, Quantity{5}, Sequence{2});
  insert_order(book_, OrderId{3}, Side::Buy, Price{90}, Quantity{5}, Sequence{3});
  ASSERT_EQ(book_.price_of(book_.best_bid_level()).value, 100);

  ASSERT_TRUE(cancel_order(book_, OrderId{1}));
  EXPECT_EQ(book_.price_of(book_.best_bid_level()).value, 95);
  ASSERT_TRUE(cancel_order(book_, OrderId{2}));
  EXPECT_EQ(book_.price_of(book_.best_bid_level()).value, 90);
  ASSERT_TRUE(cancel_order(book_, OrderId{3}));
  EXPECT_EQ(book_.best_bid_level(), kNullLevel);
  ExpectInvariants();
}

TEST_F(BookTest, RemovingBestAskExposesNextLevel) {
  insert_order(book_, OrderId{1}, Side::Sell, Price{100}, Quantity{5}, Sequence{1});
  insert_order(book_, OrderId{2}, Side::Sell, Price{105}, Quantity{5}, Sequence{2});
  insert_order(book_, OrderId{3}, Side::Sell, Price{110}, Quantity{5}, Sequence{3});

  ASSERT_TRUE(cancel_order(book_, OrderId{1}));
  EXPECT_EQ(book_.price_of(book_.best_ask_level()).value, 105);
  ASSERT_TRUE(cancel_order(book_, OrderId{2}));
  EXPECT_EQ(book_.price_of(book_.best_ask_level()).value, 110);
  ASSERT_TRUE(cancel_order(book_, OrderId{3}));
  EXPECT_EQ(book_.best_ask_level(), kNullLevel);
  ExpectInvariants();
}

// The occupancy bitmap is indexed 64 levels per word, so level 63/64 and
// 127/128 straddle a word boundary. These are the cases where a naive
// "current word, then adjacent word" scan goes wrong.
TEST_F(BookTest, ExtremesScanAcrossBitmapWordBoundaries) {
  // Levels 63/64 and 127/128 straddle a 64-bit occupancy word. A scan that only
  // looks at the current word, or that mishandles bit 63, finds the wrong
  // neighbour here and nowhere else.
  const std::vector<std::int64_t> inserted{0,   1,   62,  63,  64,  65,  126,
                                           127, 128, 129, 190, 191, 192, 1000};
  for (std::int64_t p : inserted) {
    SCOPED_TRACE("price=" + std::to_string(p));
    insert_order(book_, OrderId{static_cast<std::uint64_t>(p) + 1000}, Side::Buy, Price{p},
                 Quantity{1}, Sequence{static_cast<std::uint64_t>(p) + 1});
  }
  ExpectInvariants();
  EXPECT_EQ(book_.price_of(book_.best_bid_level()).value, 1000);

  // Remove the current best repeatedly; each step must land on the next
  // occupied level below, in strictly descending price order.
  std::vector<std::int64_t> expected{192, 191, 190, 129, 128, 127, 126, 65, 64, 63, 62, 1, 0};
  for (std::int64_t want : expected) {
    SCOPED_TRACE("want=" + std::to_string(want));
    const OrderIndex head = book_.best_bid_order();
    ASSERT_NE(head, kNullOrder);
    ASSERT_TRUE(cancel_order(book_, book_[head].order_id));
    ASSERT_NE(book_.best_bid_level(), kNullLevel);
    EXPECT_EQ(book_.price_of(book_.best_bid_level()).value, want);
  }
  ASSERT_TRUE(cancel_order(book_, book_[book_.best_bid_order()].order_id));
  EXPECT_EQ(book_.best_bid_level(), kNullLevel);
  ExpectInvariants();
}

TEST_F(BookTest, BestBidLookupIsAReadOfACachedIndex) {
  // Populate a deep book; best bid must be correct regardless of insertion
  // order, which is what proves the cached index is maintained on every path.
  for (std::int64_t p = 0; p <= 200; ++p) {
    insert_order(book_, OrderId{static_cast<std::uint64_t>(p) + 1}, Side::Buy, Price{p},
                 Quantity{1}, Sequence{static_cast<std::uint64_t>(p) + 1});
  }
  for (std::int64_t p = 200; p >= 0; --p) {
    ASSERT_TRUE(cancel_order(book_, OrderId{static_cast<std::uint64_t>(p) + 1}));
    if (p == 0) {
      EXPECT_EQ(book_.best_bid_level(), kNullLevel);
      break;
    }
    ASSERT_NE(book_.best_bid_level(), kNullLevel);
    EXPECT_EQ(book_.price_of(book_.best_bid_level()).value, p - 1);
  }
  ExpectInvariants();
}

// ---------------------------------------------------------------------------
// Price-time priority
// ---------------------------------------------------------------------------

TEST_F(BookTest, QueueIsStrictlyFifo) {
  const OrderIndex a =
      insert_order(book_, OrderId{1}, Side::Buy, Price{100}, Quantity{10}, Sequence{1});
  const OrderIndex b =
      insert_order(book_, OrderId{2}, Side::Buy, Price{100}, Quantity{20}, Sequence{2});
  const OrderIndex c =
      insert_order(book_, OrderId{3}, Side::Buy, Price{100}, Quantity{30}, Sequence{3});

  EXPECT_EQ(book_.best_bid_order(), a);
  EXPECT_EQ(book_[a].next, b);
  EXPECT_EQ(book_[b].next, c);
  EXPECT_EQ(book_[c].next, kNullOrder);
  EXPECT_EQ(book_[a].prev, kNullOrder);
  EXPECT_EQ(book_.top_of_book().best_bid_qty.value, 60);
  ExpectInvariants();
}

TEST_F(BookTest, CancelFromMiddleKeepsQueueIntact) {
  const OrderIndex a =
      insert_order(book_, OrderId{1}, Side::Buy, Price{100}, Quantity{10}, Sequence{1});
  const OrderIndex b =
      insert_order(book_, OrderId{2}, Side::Buy, Price{100}, Quantity{20}, Sequence{2});
  const OrderIndex c =
      insert_order(book_, OrderId{3}, Side::Buy, Price{100}, Quantity{30}, Sequence{3});

  // b is in the middle, so unlinking it exercises both the head-side and
  // tail-side fixups of the doubly linked queue.
  ASSERT_EQ(book_[a].next, b);
  ASSERT_EQ(book_[b].next, c);
  ASSERT_TRUE(cancel_order(book_, OrderId{2}));
  EXPECT_EQ(book_[a].next, c);
  EXPECT_EQ(book_[c].prev, a);
  // c survives with a as its new predecessor; the level tail is now c.
  EXPECT_EQ(book_[book_.find(OrderId{3})].prev, a);
  EXPECT_EQ(book_.top_of_book().best_bid_orders, 2u);
  EXPECT_EQ(book_.top_of_book().best_bid_qty.value, 40);
  EXPECT_EQ(book_.top_of_book().best_bid_orders, 2u);
  ExpectInvariants();
}

TEST_F(BookTest, MoveToBackOfLevelLosesPriority) {
  const OrderIndex a =
      insert_order(book_, OrderId{1}, Side::Buy, Price{100}, Quantity{10}, Sequence{1});
  insert_order(book_, OrderId{2}, Side::Buy, Price{100}, Quantity{20}, Sequence{2});
  ASSERT_EQ(book_.best_bid_order(), a);

  book_.move_to_back_of_level(a, book_.index_of(Price{100}));
  EXPECT_NE(book_.best_bid_order(), a);
  EXPECT_EQ(book_[a].prev, book_.find(OrderId{2}));
  ExpectInvariants();
}

TEST_F(BookTest, LevelCannotHoldBothSides) {
  insert_order(book_, OrderId{1}, Side::Buy, Price{100}, Quantity{10}, Sequence{1});
  const OrderIndex s = book_.arena().allocate();
  ASSERT_NE(s, kNullOrder);
  Order& o = book_.arena()[s];
  o.order_id = OrderId{2};
  o.side = Side::Sell;
  o.price = Price{100};
  o.total_qty = Quantity{10};
  o.leaves_qty = Quantity{10};
  o.arrival_seq = Sequence{2};
  o.level = book_.index_of(Price{100});

  // Must refuse: a level with both sides is a crossed book by construction.
  EXPECT_FALSE(book_.add_to_queue(s));
}

TEST_F(BookTest, BookIsNeverCrossed) {
  insert_order(book_, OrderId{1}, Side::Buy, Price{100}, Quantity{10}, Sequence{1});
  insert_order(book_, OrderId{2}, Side::Sell, Price{101}, Quantity{10}, Sequence{2});
  for (std::int64_t p = 100; p <= 101; ++p) {
    const TopOfBook tob = book_.top_of_book();
    EXPECT_FALSE(tob.crossed());
    (void)p;
  }
  ExpectInvariants();
}

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------

TEST_F(BookTest, ArenaExhaustionIsReportedNotGrown) {
  // Slot 0 is the reserved null sentinel, so usable slots are 1..capacity.
  OrderArena arena(4);
  EXPECT_EQ(arena.capacity(), 4u);
  EXPECT_EQ(arena.allocate(), OrderIndex{1});
  EXPECT_EQ(arena.allocate(), OrderIndex{2});
  EXPECT_EQ(arena.allocate(), OrderIndex{3});
  EXPECT_EQ(arena.allocate(), OrderIndex{4});
  EXPECT_EQ(arena.free_count(), 0u);
  // Full: must return the sentinel rather than allocate.
  EXPECT_EQ(arena.allocate(), kNullOrder);
  EXPECT_EQ(arena.live_count(), 4u);

  arena.release(2);
  EXPECT_EQ(arena.allocate(), OrderIndex{2});
  EXPECT_EQ(arena.free_count(), 0u);
}

TEST_F(BookTest, ReleasedSlotIsReusedWithNewGeneration) {
  OrderArena arena(2);
  const OrderIndex idx = arena.allocate();
  const std::uint32_t gen = arena[idx].generation;
  arena.release(idx);
  EXPECT_NE(arena[idx].generation, gen);

  const OrderIndex again = arena.allocate();
  EXPECT_EQ(again, idx);
  EXPECT_NE(arena[again].generation, gen);
}

TEST_F(BookTest, ReleaseResetsLinksSoStaleSlotIsNotMistakenForValid) {
  OrderArena arena(4);
  const OrderIndex idx = arena.allocate();
  arena[idx].prev = OrderIndex{2};
  arena[idx].next = OrderIndex{3};
  arena[idx].level = LevelIndex{7};
  arena.release(idx);
  EXPECT_EQ(arena[idx].prev, kNullOrder);
  EXPECT_EQ(arena[idx].next, kNullOrder);
  EXPECT_EQ(arena[idx].level, kNullLevel);
}

// ---------------------------------------------------------------------------
// Id index
// ---------------------------------------------------------------------------

TEST_F(BookTest, IdIndexFindsAndErases) {
  insert_order(book_, OrderId{42}, Side::Buy, Price{100}, Quantity{1}, Sequence{1});
  EXPECT_NE(book_.find(OrderId{42}), kNullOrder);
  EXPECT_EQ(book_.find(OrderId{43}), kNullOrder);
  EXPECT_TRUE(book_.id_index().erase(OrderId{42}));
  EXPECT_EQ(book_.find(OrderId{42}), kNullOrder);
  EXPECT_FALSE(book_.id_index().erase(OrderId{42}));
}

TEST_F(BookTest, IdIndexRejectsDuplicateLiveId) {
  insert_order(book_, OrderId{7}, Side::Buy, Price{100}, Quantity{1}, Sequence{1});
  const OrderIndex idx = book_.arena().allocate();
  EXPECT_FALSE(book_.id_index().insert(OrderId{7}, idx, book_.arena()[idx].generation));
}

TEST_F(BookTest, IdIndexFillsCompletelyWithoutLosingEntries) {
  // Backward-shift deletion is the easy thing to get subtly wrong: a single
  // misplaced entry makes every later lookup in that cluster miss. Exercise it
  // with a churn pattern that leaves gaps everywhere.
  constexpr int kCount = 512;
  for (int i = 0; i < kCount; ++i) {
    EXPECT_TRUE(book_.id_index().insert(OrderId{static_cast<std::uint64_t>(i)},
                                        static_cast<OrderIndex>(i), 1));
  }
  EXPECT_EQ(book_.id_index().size(), static_cast<std::uint32_t>(kCount));

  // Delete every third key.
  for (int i = 0; i < kCount; i += 3) {
    ASSERT_TRUE(book_.id_index().erase(OrderId{static_cast<std::uint64_t>(i)}));
  }
  for (int i = 0; i < kCount; ++i) {
    const bool present =
        book_.id_index().find_any(OrderId{static_cast<std::uint64_t>(i)}) != kNullOrder;
    EXPECT_EQ(present, i % 3 != 0) << "key " << i;
  }
  // Re-inserting the deleted keys must work and must be findable.
  for (int i = 0; i < kCount; i += 3) {
    EXPECT_TRUE(book_.id_index().insert(OrderId{static_cast<std::uint64_t>(i)},
                                        static_cast<OrderIndex>(i), 1));
  }
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(book_.id_index().find_any(OrderId{static_cast<std::uint64_t>(i)}),
              static_cast<OrderIndex>(i))
        << "key " << i;
  }
  EXPECT_EQ(book_.id_index().size(), static_cast<std::uint32_t>(kCount));
}

TEST_F(BookTest, IdIndexRefusesToGrowPastLoadLimit) {
  OrderIndexTable table;
  table.reset(64);
  const std::uint32_t limit = table.max_size();
  for (std::uint32_t i = 0; i < limit; ++i) {
    ASSERT_TRUE(table.insert(OrderId{i + 1}, static_cast<OrderIndex>(i), 1)) << i;
  }
  // Must fail rather than rehash: a rehash here would allocate and stall.
  EXPECT_FALSE(table.insert(OrderId{9999}, OrderIndex{0}, 1));
  EXPECT_EQ(table.size(), limit);
}

TEST_F(BookTest, GenerationGuardsAgainstStaleHandles) {
  OrderIndexTable table;
  table.reset(16);
  ASSERT_TRUE(table.insert(OrderId{5}, OrderIndex{3}, /*generation=*/1));
  EXPECT_EQ(table.find(OrderId{5}, 1), OrderIndex{3});
  // Same key, different generation: the caller is holding a stale handle.
  EXPECT_EQ(table.find(OrderId{5}, 2), kNullOrder);
}

// ---------------------------------------------------------------------------
// State hash and invariants
// ---------------------------------------------------------------------------

TEST_F(BookTest, StateHashIsStableForIdenticalState) {
  testing::BookFixture fa(test_config());
  testing::BookFixture fb(test_config());
  Book& a = fa.book;
  Book& b = fb.book;
  for (int i = 0; i < 50; ++i) {
    const auto id = OrderId{static_cast<std::uint64_t>(i) + 1};
    // Bids below, asks above: a level may only ever hold one side.
    const auto side = (i % 2 == 0) ? Side::Buy : Side::Sell;
    const auto price = side == Side::Buy ? Price{100 + (i % 7)} : Price{200 + (i % 7)};
    const auto qty = Quantity{i + 1};
    const auto seq = Sequence{static_cast<std::uint64_t>(i) + 1};
    insert_order(a, id, side, price, qty, seq);
    insert_order(b, id, side, price, qty, seq);
  }
  EXPECT_EQ(a.state_hash(), b.state_hash());
}

TEST_F(BookTest, StateHashIsSensitiveToPriorityOrder) {
  testing::BookFixture fa(test_config());
  testing::BookFixture fb(test_config());
  Book& a = fa.book;
  Book& b = fb.book;
  insert_order(a, OrderId{1}, Side::Buy, Price{100}, Quantity{10}, Sequence{1});
  insert_order(a, OrderId{2}, Side::Buy, Price{100}, Quantity{10}, Sequence{2});
  insert_order(b, OrderId{2}, Side::Buy, Price{100}, Quantity{10}, Sequence{2});
  insert_order(b, OrderId{1}, Side::Buy, Price{100}, Quantity{10}, Sequence{1});
  // Same set of orders, different queue order: the hashes must differ, or a
  // replay could produce a matching hash while having the wrong priority.
  EXPECT_NE(a.state_hash(), b.state_hash());
}

TEST_F(BookTest, StateHashChangesWhenQuantityChanges) {
  insert_order(book_, OrderId{1}, Side::Buy, Price{100}, Quantity{10}, Sequence{1});
  const std::uint64_t before = book_.state_hash();
  book_[book_.find(OrderId{1})].leaves_qty = Quantity{11};
  EXPECT_NE(book_.state_hash(), before);
}

TEST_F(BookTest, EmptyBookHasStableNonTrivialHash) {
  testing::BookFixture fa(test_config());
  testing::BookFixture fb(test_config());
  Book& a = fa.book;
  Book& b = fb.book;
  EXPECT_EQ(a.state_hash(), b.state_hash());
  // Different configuration must not hash the same, or a replay across configs
  // would compare equal while being semantically different.
  SymbolConfig other = test_config();
  other.tick_size = 5;
  testing::BookFixture fc(other);
  const Book& c = fc.book;
  EXPECT_NE(a.state_hash(), c.state_hash());
}

TEST_F(BookTest, InvariantsHoldUnderChurn) {
  std::uint64_t seq = 0;
  std::vector<OrderId> live;
  for (int i = 0; i < 2000; ++i) {
    ++seq;
    if (live.empty() || (i % 3 != 0)) {
      const auto id = OrderId{static_cast<std::uint64_t>(i) + 1};
      const auto side = (i % 2 == 0) ? Side::Buy : Side::Sell;
      // Buy below the mid, sell above it, so the book can never cross.
      const auto price = side == Side::Buy ? Price{100 + (i % 20)} : Price{200 + (i % 20)};
      if (insert_order(book_, id, side, price, Quantity{i % 50 + 1}, Sequence{seq}) != kNullOrder) {
        live.push_back(id);
      }
    } else {
      const std::size_t pick = static_cast<std::size_t>(i) % live.size();
      ASSERT_TRUE(cancel_order(book_, live[pick]));
      live.erase(live.begin() + static_cast<std::ptrdiff_t>(pick));
    }
    if (i % 97 == 0) {
      ExpectInvariants();
    }
  }
  ExpectInvariants();
}

}  // namespace
}  // namespace lob