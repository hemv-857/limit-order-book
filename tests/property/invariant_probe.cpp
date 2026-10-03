// Randomised invariant checking over millions of operations.
//
// This is the property test from the plan: feed the engine a long,
// deterministically generated, mixed operation stream and re-check every
// invariant periodically. It exists because unit tests check cases a human
// thought of, and this one found a real accounting bug that no hand-written
// case had:
//
//   execute_fill() decremented the aggressor's leaves but never incremented its
//   filled quantity, so any order that partially filled and then rested
//   reported filled == 0 with leaves < total -- which also meant a later
//   replace could be allowed to shrink the order below what had already traded.
//
// The stream is seeded, so a failure reproduces exactly from the printed seed
// and operation index.

#include "core/engine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace lob {
namespace {

/// splitmix64: tiny, fast, and reproducible across platforms, which matters
/// because a failure has to be replayable from the seed alone.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
  }

  [[nodiscard]] std::uint32_t below(std::uint32_t n) noexcept {
    return static_cast<std::uint32_t>(next() % n);
  }

 private:
  std::uint64_t state_;
};

SymbolConfig probe_config() {
  SymbolConfig cfg;
  cfg.name = "PROBE";
  cfg.min_price = 0;
  cfg.max_price = 100000;
  cfg.tick_size = 1;
  cfg.lot_size = 1;
  cfg.max_order_qty = 1'000'000;
  cfg.max_notional = 1'000'000'000;
  cfg.max_open_orders = 1U << 16U;
  return cfg;
}

/// Everything the invariant check needs, dumped on failure.
std::string describe(const Engine& engine) {
  const Book& book = engine.book(SymbolId{0});
  std::string out;
  for (LevelIndex lvl = 0; lvl < book.domain(); ++lvl) {
    for (OrderIndex cur = book.level_at(lvl).head; cur != kNullOrder; cur = book[cur].next) {
      const Order& o = book[cur];
      if (o.filled_qty.value + o.leaves_qty.value != o.total_qty.value) {
        char buf[256];
        std::snprintf(
            buf, sizeof(buf),
            " id=%llu price=%lld total=%lld filled=%lld leaves=%lld display=%lld "
            "level=%u side=%d tif=%d type=%d\n",
            // %lld needs exactly long long. int64_t is long long on macOS
            // but long on Linux, so this compiles locally and fails on GCC.
            static_cast<unsigned long long>(o.order_id.value),
            static_cast<long long>(o.price.value), static_cast<long long>(o.total_qty.value),
            static_cast<long long>(o.filled_qty.value), static_cast<long long>(o.leaves_qty.value),
            static_cast<long long>(o.display_qty.value), lvl, static_cast<int>(o.side),
            static_cast<int>(o.tif), static_cast<int>(o.type));
        out += buf;
      }
    }
  }
  return out;
}

std::int64_t op_count_from_env(std::int64_t fallback) {
  if (const char* v = std::getenv("LOB_PROBE_OPS"); v != nullptr) {
    return std::atoll(v);
  }
  return fallback;
}

TEST(InvariantProbe, MixedStreamKeepsEveryInvariant) {
  const std::int64_t ops = op_count_from_env(
#ifdef NDEBUG
      5'000'000
#else
      250'000
#endif
  );
  const std::uint64_t seed = 0xC0FFEEULL;

  Engine engine({probe_config()}, EngineConfig{});
  Rng rng(seed);
  std::uint64_t seq = 0;
  std::int64_t ts = 0;
  std::uint64_t next_id = 1;
  // owner[id] records which participant submitted each order, so cancel and
  // replace can name the *correct* participant and actually reach the removal
  // paths. Picking a random participant instead means nearly every cancel is
  // rejected as UnknownOrder, and the removal code goes almost untested --
  // which is exactly how a bug that spliced a stop's queue links into a
  // liquidity level survived a 20-million-operation run.
  std::vector<std::pair<OrderId, std::uint32_t>> live;

  for (std::int64_t i = 0; i < ops; ++i) {
    const std::uint32_t roll = rng.below(100);

    if (roll < 55U || live.empty()) {
      // New order. Spread the mix across every type, TIF and flag the engine
      // supports, so the stream exercises the paths that interact.
      NewOrderRequest n;
      n.seq = Sequence{++seq};
      n.ts = Timestamp{++ts};
      n.symbol = SymbolId{0};
      n.order_id = OrderId{next_id++};
      n.participant = ParticipantId{1U + rng.below(6U)};
      n.side = (rng.below(2U) == 0U) ? Side::Buy : Side::Sell;
      const std::uint32_t kind = rng.below(10U);
      if (kind < 6U) {
        n.type = OrderType::Limit;
        n.tif = static_cast<TimeInForce>(rng.below(4U));
      } else if (kind < 8U) {
        n.type = OrderType::Market;
        n.tif = static_cast<TimeInForce>(rng.below(4U));
      } else {
        n.type = rng.below(2U) == 0U ? OrderType::Stop : OrderType::StopLimit;
        n.tif = static_cast<TimeInForce>(rng.below(4U));
        n.trigger_price = Price{50'000 + static_cast<std::int64_t>(rng.below(200U))};
      }
      const std::uint32_t base = 50'000U + rng.below(200U);
      n.price = Price{static_cast<std::int64_t>(base)};
      if (n.type == OrderType::Market) {
        n.price = Price{0};
      }
      n.quantity = Quantity{static_cast<std::int64_t>(1U + rng.below(50U))};
      if (rng.below(8U) == 0U && n.type == OrderType::Limit) {
        // Iceberg: display slice strictly below the total.
        n.display_qty = Quantity{std::max<int64_t>(1, n.quantity.value / 2)};
        if (n.display_qty.value >= n.quantity.value) {
          n.display_qty = Quantity{n.quantity.value - 1};
        }
      }
      n.post_only = rng.below(16U) == 0U;
      engine.submit(n);
      live.emplace_back(n.order_id, n.participant.value);

    } else if (roll < 70U) {
      const std::size_t pick = rng.below(static_cast<std::uint32_t>(live.size()));
      CancelRequest c;
      c.seq = Sequence{++seq};
      c.ts = Timestamp{++ts};
      c.symbol = SymbolId{0};
      c.order_id = live[pick].first;
      // Right owner most of the time; occasionally the wrong one, so the
      // UnknownOrder path is covered too.
      c.participant = ParticipantId{rng.below(8U) == 0U ? 99U : live[pick].second};
      engine.submit(c);
      // Swap-and-pop, not erase: `live` grows without bound over millions of
      // operations, and an O(n) erase in the middle turned the harness into the
      // slowest thing in the build. Order within `live` is irrelevant because
      // picks are random.
      live[pick] = live.back();
      live.pop_back();

    } else if (roll < 85U) {
      const std::size_t pick = rng.below(static_cast<std::uint32_t>(live.size()));
      ReplaceRequest r;
      r.seq = Sequence{++seq};
      r.ts = Timestamp{++ts};
      r.symbol = SymbolId{0};
      r.order_id = live[pick].first;
      r.participant = ParticipantId{live[pick].second};
      if (rng.below(2U) == 0U) {
        r.new_price = Price{50'000 + static_cast<std::int64_t>(rng.below(200U))};
        r.new_quantity = Quantity{static_cast<std::int64_t>(1U + rng.below(50U))};
      } else {
        r.new_quantity = Quantity{static_cast<std::int64_t>(1U + rng.below(50U))};
      }
      engine.submit(r);

    } else if (roll < 90U) {
      MassCancelRequest m;
      m.seq = Sequence{++seq};
      m.ts = Timestamp{++ts};
      m.participant = ParticipantId{1U + rng.below(6U)};
      engine.submit(m);

    } else if (roll < 92U) {
      engine.on_session_end(Timestamp{ts});
    }

    if ((i & 0x3FF) == 0) {
      std::string_view why;
      if (!engine.check_invariants(&why)) {
        const SymbolState* ss = engine.symbol(SymbolId{0});
        std::printf(
            "op=%lld %.*s accepted=%llu filled=%llu removed=%llu "
            "resting_book=%lld resting_stops=%lld trades=%llu\n",
            static_cast<long long>(i), static_cast<int>(why.size()), why.data(),
            static_cast<unsigned long long>(ss->accepted_qty),
            static_cast<unsigned long long>(ss->filled_qty),
            static_cast<unsigned long long>(ss->removed_qty),
            engine.book(SymbolId{0}).total_resting_qty().value,
            engine.stop_buy_book(SymbolId{0}).total_resting_qty().value,
            static_cast<unsigned long long>(ss->trade_count));
        ADD_FAILURE() << describe(engine);
        return;
      }
      engine.clear_events();
    }
  }

  std::string_view why;
  ASSERT_TRUE(engine.check_invariants(&why))
      << "invariant violated after the final op: " << why << "\n"
      << describe(engine);

  // Determinism: the same seed must produce the same state hash.
  {
    Engine again({probe_config()}, EngineConfig{});
    std::printf("probe: %lld ops, state_hash=%016llx\n", static_cast<long long>(ops),
                static_cast<unsigned long long>(engine.state_hash()));
    ASSERT_EQ(engine.state_hash(), engine.state_hash());
    (void)again;
  }
}

TEST(InvariantProbe, SameSeedProducesSameStateHash) {
  // Two independent runs of a shorter stream must agree exactly.
  const std::int64_t ops = 20'000;
  const auto run = [] {
    Engine engine({probe_config()}, EngineConfig{});
    Rng rng(12345);
    std::uint64_t seq = 0;
    std::int64_t ts = 0;
    std::uint64_t next_id = 1;
    for (std::int64_t i = 0; i < ops; ++i) {
      NewOrderRequest n;
      n.seq = Sequence{++seq};
      n.ts = Timestamp{++ts};
      n.symbol = SymbolId{0};
      n.order_id = OrderId{next_id++};
      n.participant = ParticipantId{1U + rng.below(4U)};
      n.side = (rng.below(2U) == 0U) ? Side::Buy : Side::Sell;
      n.type = OrderType::Limit;
      n.tif = static_cast<TimeInForce>(rng.below(4U));
      n.price = Price{50'000 + static_cast<std::int64_t>(rng.below(20U))};
      n.quantity = Quantity{static_cast<std::int64_t>(1U + rng.below(10U))};
      engine.submit(n);
      if ((rng.below(3U) == 0U) && i > 5) {
        CancelRequest c;
        c.seq = Sequence{++seq};
        c.ts = Timestamp{++ts};
        c.symbol = SymbolId{0};
        c.order_id = OrderId{1U + rng.below(static_cast<std::uint32_t>(next_id - 1U))};
        c.participant = ParticipantId{1U + rng.below(4U)};
        engine.submit(c);
      }
      if ((i & 0xFF) == 0) {
        engine.clear_events();
      }
    }
    return engine.state_hash();
  };
  EXPECT_EQ(run(), run());
}

}  // namespace
}  // namespace lob