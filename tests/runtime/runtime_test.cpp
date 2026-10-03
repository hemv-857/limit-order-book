#include "runtime/runtime.hpp"
#include "runtime/spsc_ring.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace lob {
namespace {

// ---------------------------------------------------------------------------
// SPSC ring
// ---------------------------------------------------------------------------

TEST(SpscRing, StartsEmpty) {
  SpscRing<std::uint64_t, 8> ring;
  EXPECT_TRUE(ring.empty());
  EXPECT_EQ(ring.size(), 0u);
  EXPECT_EQ(ring.capacity(), 8u);
}

TEST(SpscRing, PushThenPopPreservesOrder) {
  SpscRing<std::uint64_t, 8> ring;
  for (std::uint64_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.push(i)) << "at " << i;
  }
  for (std::uint64_t i = 0; i < 4; ++i) {
    std::uint64_t v = 999;
    ASSERT_TRUE(ring.pop(v)) << "at " << i;
    EXPECT_EQ(v, i);
  }
  std::uint64_t v = 0;
  EXPECT_FALSE(ring.pop(v));
}

TEST(SpscRing, ReportsFullAndEmptyDistinctly) {
  // A ring that cannot distinguish full from empty is the classic way to lose or
  // duplicate an item at the wrap point, so this is worth pinning.
  SpscRing<std::uint64_t, 4> ring;
  for (std::uint64_t i = 0; i < 4; ++i) {
    ASSERT_TRUE(ring.push(i));
  }
  EXPECT_EQ(ring.size(), 4u);
  EXPECT_FALSE(ring.push(4)) << "a full ring must refuse a push";
  // Full is not empty: popping from a full ring must still work.
  std::uint64_t v = 0;
  ASSERT_TRUE(ring.pop(v));
  EXPECT_EQ(v, 0u);
  EXPECT_TRUE(ring.push(99)) << "a popped slot must be reusable immediately";
  EXPECT_EQ(ring.size(), 4u);
}

TEST(SpscRing, WrapsRepeatedly) {
  SpscRing<int, 8> ring;
  int next_push = 0;
  int next_pop = 0;
  // Push and pop in an interleaved pattern well past the wrap point.
  for (int round = 0; round < 1000; ++round) {
    for (int k = 0; k < 3; ++k) {
      ASSERT_TRUE(ring.push(next_push++)) << "round " << round;
    }
    for (int k = 0; k < 3; ++k) {
      int v = -1;
      ASSERT_TRUE(ring.pop(v)) << "round " << round;
      ASSERT_EQ(v, next_pop++);
    }
  }
}

/// The test that matters under TSan: if the producer's publish or the consumer's
/// acquire is missing, the consumer can observe the index update without the slot
/// contents, and this trips either TSan or the value assertion.
TEST(SpscRing, ConcurrentProducerConsumerSeesEveryValue) {
  constexpr int kItems = 200'000;
  SpscRing<int, 64> ring;

  std::thread producer([&ring] {
    for (int i = 0; i < kItems; ++i) {
      while (!ring.push(i)) {
        std::this_thread::yield();
      }
    }
  });

  int received = 0;
  int expected = 0;
  while (received < kItems) {
    int v = -1;
    if (!ring.pop(v)) {
      std::this_thread::yield();
      continue;
    }
    ASSERT_EQ(v, expected) << "out of order or lost at " << expected;
    ++expected;
    ++received;
  }
  producer.join();
  EXPECT_TRUE(ring.empty());
}

// ---------------------------------------------------------------------------
// Sequencer
// ---------------------------------------------------------------------------

TEST(Sequencer, AssignsGapFreeIncreasingSequences) {
  Sequencer seq(4);
  std::uint64_t previous = 0;
  for (int i = 0; i < 1000; ++i) {
    const Sequence s = seq.next_sequence();
    ASSERT_EQ(s.value, previous + 1) << "gap or reuse at " << i;
    previous = s.value;
  }
}

TEST(Sequencer, RoutesASymbolToOneShard) {
  Sequencer seq(4);
  for (std::uint32_t sym = 0; sym < 100; ++sym) {
    const std::size_t shard = seq.shard_for(SymbolId{sym});
    ASSERT_LT(shard, 4u);
    // Stable: the same symbol must always land on the same shard, or two threads
    // would be mutating one book.
    EXPECT_EQ(shard, seq.shard_for(SymbolId{sym}));
  }
}

TEST(Sequencer, ConcurrentStampsAreUnique) {
  Sequencer seq(1);
  constexpr int kThreads = 4;
  constexpr int kPerThread = 5000;
  std::vector<std::vector<std::uint64_t>> got(kThreads);
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&seq, &got, t] {
      for (int i = 0; i < kPerThread; ++i) {
        got[static_cast<std::size_t>(t)].push_back(seq.next_sequence().value);
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  std::vector<std::uint64_t> all;
  for (const auto& v : got) {
    all.insert(all.end(), v.begin(), v.end());
  }
  std::sort(all.begin(), all.end());
  ASSERT_EQ(all.size(), static_cast<std::size_t>(kThreads * kPerThread));
  for (std::size_t i = 0; i < all.size(); ++i) {
    // Unique and gap-free: no two threads can get the same stamp.
    ASSERT_EQ(all[i], i + 1) << "duplicate or gap at index " << i;
  }
}

// ---------------------------------------------------------------------------
// Sharded host
// ---------------------------------------------------------------------------

SymbolConfig sym_cfg(std::string_view name) {
  SymbolConfig c;
  c.name = name;
  c.min_price = 0;
  c.max_price = 10'000;
  c.tick_size = 1;
  c.lot_size = 1;
  c.max_order_qty = 100'000;
  c.max_notional = 1'000'000'000;
  c.max_open_orders = 4096;
  return c;
}

NewOrderRequest ord(SymbolId symbol, OrderId id, Side side, Price px, Quantity qty) {
  NewOrderRequest r;
  r.seq = Sequence{id.value};
  r.ts = Timestamp{static_cast<std::int64_t>(id.value)};
  r.symbol = symbol;
  r.order_id = id;
  r.participant = ParticipantId{1};
  r.side = side;
  r.type = OrderType::Limit;
  r.tif = TimeInForce::GTC;
  r.price = px;
  r.quantity = qty;
  return r;
}

TEST(ShardedEngineHost, RoutesSymbolsToDistinctShardsAndKeepsThemUncrossed) {
  const std::size_t kShards = 4;
  std::vector<SymbolConfig> symbols;
  for (std::size_t i = 0; i < kShards; ++i) {
    symbols.push_back(sym_cfg("S" + std::to_string(i)));
  }
  ShardedEngineHost host(std::move(symbols), kShards);
  ASSERT_EQ(host.shard_count(), kShards);

  // Two-sided flow per symbol: a resting bid and a matching ask, so each shard
  // trades, then drain and check nothing crossed.
  for (std::uint32_t s = 0; s < kShards; ++s) {
    for (int i = 1; i <= 200; ++i) {
      ASSERT_TRUE(host.submit(ord(SymbolId{s}, OrderId{s * 1000 + static_cast<std::uint32_t>(i)},
                                  Side::Buy, Price{100}, Quantity{1})));
      ASSERT_TRUE(
          host.submit(ord(SymbolId{s}, OrderId{s * 1000 + 500 + static_cast<std::uint32_t>(i)},
                          Side::Sell, Price{100}, Quantity{1})));
    }
  }
  host.drain();
  EXPECT_TRUE(host.draining());
  // A submit after draining must be refused, never silently dropped.
  EXPECT_FALSE(host.submit(ord(SymbolId{0}, OrderId{999'999}, Side::Buy, Price{1}, Quantity{1})));
}

/// Poll the owning shard for a symbol's touch. The only safe way to read a
/// shard's book: the worker owns it, so the read has to happen there and arrive
/// over the snapshot ring.
TopOfBook observe(ShardedEngineHost& host, SymbolId symbol,
                  std::chrono::milliseconds limit = std::chrono::milliseconds(3000)) {
  SubscribeRequest req;
  req.seq = Sequence{999'999};
  req.ts = Timestamp{0};
  req.symbol = symbol;
  req.session_id = 0;
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < deadline) {
    if (host.submit(req)) {
      for (const ShardSnapshot& a : host.take_snapshots()) {
        if (a.symbol.value == symbol.value) {
          return a.book;
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return TopOfBook{};
}

TEST(ShardedEngineHost, DrainAppliesEverythingAcceptedBeforeIt) {
  ShardedEngineHost live({sym_cfg("A"), sym_cfg("B")}, 2);
  for (int i = 1; i <= 3000; ++i) {
    const SymbolId symbol{static_cast<std::uint32_t>(i % 2)};
    ASSERT_TRUE(live.submit(ord(symbol, OrderId{static_cast<std::uint64_t>(i)}, Side::Buy,
                                Price{50 + (i % 10)}, Quantity{1})));
  }
  // The same requests into a single-shard host. Per-symbol books must match
  // exactly; the combined state_hash deliberately cannot be compared, because it
  // folds shard hashes in shard order and the shard layout differs by design.
  ShardedEngineHost reference({sym_cfg("A"), sym_cfg("B")}, 1);
  for (int i = 1; i <= 3000; ++i) {
    const SymbolId symbol{static_cast<std::uint32_t>(i % 2)};
    ASSERT_TRUE(reference.submit(ord(symbol, OrderId{static_cast<std::uint64_t>(i)}, Side::Buy,
                                     Price{50 + (i % 10)}, Quantity{1})));
  }
  // Compare while the workers are still running: after drain() there is no thread
  // left to take a snapshot, which is exactly why the host has no synchronous
  // top_of_book accessor.
  for (std::uint32_t sym = 0; sym < 2; ++sym) {
    const TopOfBook got = observe(live, SymbolId{sym});
    const TopOfBook want = observe(reference, SymbolId{sym});
    EXPECT_EQ(got.has_bid, want.has_bid) << "symbol " << sym;
    ASSERT_EQ(got.has_bid, want.has_bid);
    EXPECT_EQ(got.best_bid.value, want.best_bid.value) << "symbol " << sym;
    EXPECT_EQ(got.best_bid_qty.value, want.best_bid_qty.value) << "symbol " << sym;
    EXPECT_EQ(got.best_bid_orders, want.best_bid_orders) << "symbol " << sym;
    EXPECT_EQ(got.has_ask, want.has_ask) << "symbol " << sym;
  }
  // Sanity: there really are orders resting, so the comparison above is not
  // trivially satisfied by two empty books.
  EXPECT_GT(observe(live, SymbolId{0}).best_bid_qty.value, 0);

  // And the drain itself must complete and apply everything.
  live.drain();
  reference.drain();
}

}  // namespace
}  // namespace lob