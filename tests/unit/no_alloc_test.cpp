#include "core/book.hpp"

#include "book_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace alloc_guard {
void enable() noexcept;
void disable() noexcept;
std::size_t count() noexcept;
}  // namespace alloc_guard

namespace lob {
namespace {

SymbolConfig alloc_config() {
  SymbolConfig cfg;
  cfg.name = "ALLOC";
  cfg.min_price = 0;
  cfg.max_price = 10000;
  cfg.tick_size = 1;
  cfg.lot_size = 1;
  cfg.max_open_orders = 8192;
  return cfg;
}

// The whole point of pre-sized arenas and a non-rehashing id index is that the
// book never grows while it is working. If any of these fail, the venue will
// eventually take an unbounded allocation spike under exactly the load it is
// least able to absorb.

// Guard against a vacuous guard: if the replacement operator new were not
// actually wired in, every "no allocation" assertion below would pass trivially
// and prove nothing at all.
TEST(NoAlloc, GuardActuallyObservesAllocations) {
  alloc_guard::enable();
  auto* v = new std::vector<int>(1024);
  const std::size_t seen = alloc_guard::count();
  alloc_guard::disable();
  EXPECT_GE(seen, 1u) << "allocation guard is not counting; every other test here is meaningless";
  delete v;
}

TEST(NoAlloc, ArenaDoesNotGrowWhileWorking) {
  OrderArena arena(1024);
  // Take every slot, then release half of them, repeatedly.
  const OrderIndex first = arena.allocate();
  ASSERT_NE(first, kNullOrder);
  alloc_guard::enable();
  for (int round = 0; round < 64; ++round) {
    for (std::uint32_t i = 0; i < 1024; ++i) {
      const OrderIndex idx = arena.allocate();
      (void)idx;
      arena.release(static_cast<OrderIndex>(i));
    }
  }
  const std::size_t allocations = alloc_guard::count();
  alloc_guard::disable();
  EXPECT_EQ(allocations, 0u) << "arena allocated " << allocations << " times while working";
}

TEST(NoAlloc, BookOpsDoNotAllocate) {
  testing::BookFixture fixture(alloc_config());
  Book& book = fixture.book;
  // Warm up outside the guard: the constructor legitimately allocates the
  // level array, the bitmap, the arena and the id index exactly once.
  for (std::int64_t p = 0; p < 100; ++p) {
    const OrderIndex idx = book.arena().allocate();
    book.arena()[idx].order_id = OrderId{static_cast<std::uint64_t>(p) + 1};
    book.arena()[idx].price = Price{p};
    book.arena()[idx].total_qty = Quantity{1};
    book.arena()[idx].leaves_qty = Quantity{1};
    book.arena()[idx].side = Side::Buy;
    book.arena()[idx].level = book.index_of(Price{p});
    (void)book.id_index().insert(book.arena()[idx].order_id, idx, book.arena()[idx].generation);
    (void)book.add_to_queue(idx);
  }
  const std::uint64_t state_hash_before = book.state_hash();

  alloc_guard::enable();
  for (std::int64_t p = 0; p < 100; ++p) {
    const OrderIndex idx = book.find(OrderId{static_cast<std::uint64_t>(p) + 1});
    book.remove_from_queue(idx);
    book.id_index().erase(OrderId{static_cast<std::uint64_t>(p) + 1});
    book.arena().release(idx);
  }
  // Reinsert a different way round: still no allocation expected.
  for (std::int64_t p = 99; p >= 0; --p) {
    const OrderIndex idx = book.arena().allocate();
    book.arena()[idx].order_id = OrderId{static_cast<std::uint64_t>(p) + 1000};
    book.arena()[idx].price = Price{p};
    book.arena()[idx].total_qty = Quantity{2};
    book.arena()[idx].leaves_qty = Quantity{2};
    book.arena()[idx].side = p % 2 == 0 ? Side::Buy : Side::Sell;
    book.arena()[idx].level = book.index_of(Price{p});
    (void)book.id_index().insert(book.arena()[idx].order_id, idx, book.arena()[idx].generation);
    (void)book.add_to_queue(idx);
  }
  // Moving levels is the operation most likely to touch an allocator, since it
  // is the only one that can empty a level and force an extreme rescan.
  for (std::int64_t p = 0; p < 100; ++p) {
    const OrderIndex idx = book.find(OrderId{static_cast<std::uint64_t>(p) + 1000});
    book.move_to_back_of_level(idx, book.index_of(Price{p}));
  }
  volatile std::uint64_t sink = book.state_hash();
  (void)sink;
  const std::uint64_t hash = book.state_hash();
  const std::size_t allocations = alloc_guard::count();
  alloc_guard::disable();

  EXPECT_EQ(allocations, 0u) << "book allocated " << allocations << " times while working";
  EXPECT_NE(hash, state_hash_before);
}

TEST(NoAlloc, IdIndexDoesNotRehashWhenFull) {
  OrderIndexTable table;
  table.reset(64);
  // Fill to the load limit.
  for (std::uint32_t i = 0; i < table.max_size(); ++i) {
    ASSERT_TRUE(table.insert(OrderId{i + 1}, static_cast<OrderIndex>(i), 1));
  }
  alloc_guard::enable();
  bool refused = false;
  for (int i = 0; i < 1000; ++i) {
    refused = !table.insert(OrderId{10'000 + static_cast<std::uint64_t>(i)}, OrderIndex{0}, 1);
  }
  const std::size_t allocations = alloc_guard::count();
  alloc_guard::disable();
  EXPECT_TRUE(refused) << "a full index must refuse inserts, not rehash";
  EXPECT_EQ(allocations, 0u) << "full index reallocated";
}

}  // namespace
}  // namespace lob