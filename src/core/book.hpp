#pragma once

#include "core/arena.hpp"
#include "core/config.hpp"
#include "core/order.hpp"
#include "core/order_index.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <vector>

namespace lob {

/// One price level: an intrusive FIFO of orders plus its aggregate size.
///
/// `side` is stored per level rather than inferred. A level can only ever hold
/// orders of one side, because the book is never allowed to cross; recording it
/// explicitly makes that a checkable invariant instead of an assumption, and it
/// costs nothing because the struct already had padding.
struct Level {
  OrderIndex head{kNullOrder};  ///< best-priority order (front of the queue)
  OrderIndex tail{kNullOrder};  ///< worst-priority order (back of the queue)
  Quantity aggregate_qty{0};    ///< sum of leaves_qty across the queue
  std::uint32_t order_count{0};
  Side side{Side::Buy};

  [[nodiscard]] bool empty() const noexcept {
    return head == kNullOrder;
  }
};

static_assert(sizeof(Level) <= 24, "Level grew past its cache-friendly budget");

/// Snapshot of the top of book, used by market data and the CLI. A plain value
/// type so that callers can hold it without touching the book.
struct TopOfBook {
  Price best_bid{};
  Price best_ask{};
  Quantity best_bid_qty{};
  Quantity best_ask_qty{};
  std::uint32_t best_bid_orders{0};
  std::uint32_t best_ask_orders{0};
  bool has_bid{false};
  bool has_ask{false};

  [[nodiscard]] bool crossed() const noexcept {
    return has_bid && has_ask && best_bid >= best_ask;
  }
  [[nodiscard]] bool empty() const noexcept {
    return !has_bid && !has_ask;
  }
};

/// A single-symbol limit order book.
///
/// The price side is a flat array of levels indexed by `price - min_price`, which
/// makes level lookup O(1) and, more importantly, turns "is there liquidity
/// here" into a single cache line probe instead of a tree descent. Levels are
/// never removed: an empty level is simply an empty Level, so there is no
/// allocator churn and no lazy initialisation branch.
///
/// Keeping the extremes current is the only part that needs care. `best_bid_`
/// and `best_ask_` are cached indices, so reading the top of book is a load.
/// When an extreme level empties, the new extreme is found by scanning the
/// occupancy bitmap for the nearest set bit, which is O(1) in practice because
/// the scan stops at the first non-empty word; its worst case is bounded by the
/// configured price domain.
class Book {
 public:
  /// The book owns price structure only. Order storage and the id index belong
  /// to the caller, because a symbol needs two books over the *same* orders:
  /// the resting book and the stop book. If each book owned an arena, a stop
  /// order's index would mean different things in the two of them.
  Book(const SymbolConfig& cfg, OrderArena& arena, OrderIndexTable& index);

  ~Book() = default;

  Book(const Book&) = delete;
  Book& operator=(const Book&) = delete;
  Book(Book&&) = delete;
  Book& operator=(Book&&) = delete;

  // ---- price domain -------------------------------------------------------

  [[nodiscard]] LevelIndex index_of(Price p) const noexcept {
    return static_cast<LevelIndex>(p.value - min_price_);
  }
  [[nodiscard]] Price price_of(LevelIndex idx) const noexcept {
    return Price{min_price_ + static_cast<std::int64_t>(idx)};
  }
  [[nodiscard]] std::uint32_t domain() const noexcept {
    return domain_;
  }
  [[nodiscard]] bool in_domain(Price p) const noexcept {
    return p.value >= min_price_ && p.value <= max_price_;
  }

  // ---- queue manipulation -------------------------------------------------

  /// Append an order to the back of its price level, giving it the worst
  /// priority at that price. Returns false if the order is not linked or its
  /// level index is out of range.
  bool add_to_queue(OrderIndex idx) noexcept;

  /// Unlink an order from its level, reducing the level aggregate by the
  /// quantity the order currently contributes and fixing the cached extremes.
  void remove_from_queue(OrderIndex idx) noexcept;

  /// Reduce a level's aggregate by `qty` without touching the queue.
  ///
  /// Needed by partial fills. A fill decrements an order's leaves in place, so
  /// unlinking it afterwards would subtract the *post*-fill quantity (possibly
  /// zero) and leave the level aggregate stale. Unlinking before the decrement
  /// is only correct when the order is leaving the level entirely.
  void reduce_level(LevelIndex idx, Quantity qty) noexcept;

  /// Move an order from the back of its current level to the back of its new
  /// level, which is how a price change or a quantity increase loses priority.
  void move_to_back_of_level(OrderIndex idx, LevelIndex new_level) noexcept;

  // ---- lookup -------------------------------------------------------------

  [[nodiscard]] OrderIndex find(OrderId id) const noexcept {
    return index_->find_any(id);
  }

  /// Whether an order id is live in this book (resting liquidity).
  [[nodiscard]] bool index_contains(OrderId id) const noexcept {
    return index_->contains(id);
  }

  [[nodiscard]] Order& operator[](OrderIndex idx) noexcept {
    return (*arena_)[idx];
  }
  [[nodiscard]] const Order& operator[](OrderIndex idx) const noexcept {
    return (*arena_)[idx];
  }

  /// Direct access to a level, so callers can walk occupied levels without
  /// repeating the emptiness test.
  [[nodiscard]] Level& level_at(LevelIndex idx) noexcept {
    return levels_[idx];
  }
  [[nodiscard]] const Level& level_at(LevelIndex idx) const noexcept {
    return levels_[idx];
  }

  /// Occupancy navigation. These are the same bitmap scans that maintain the
  /// cached extremes; the engine needs them to walk stop triggers.
  [[nodiscard]] LevelIndex next_occupied_index(LevelIndex idx) const noexcept {
    return next_occupied(static_cast<std::size_t>(idx));
  }
  [[nodiscard]] LevelIndex prev_occupied_index(LevelIndex idx) const noexcept {
    return prev_occupied(static_cast<std::size_t>(idx));
  }
  /// Lowest / highest occupied level in the whole domain.
  ///
  /// Not the same as prev_occupied(domain_) and next_occupied(0): those return
  /// the *nearest* occupied index to a cursor, which at the domain edge is the
  /// highest and lowest respectively -- exactly inverted. These scan words in
  /// the right direction, which is O(words) worst case; callers use them once
  /// and then walk with the O(1) next/prev_occupied_index steps.
  [[nodiscard]] LevelIndex lowest_occupied() const noexcept {
    for (std::size_t word = 0; word < bitmap_words_; ++word) {
      if (occupied_[word] != 0) {
        return static_cast<LevelIndex>((word * 64U) +
                                       static_cast<unsigned>(__builtin_ctzll(occupied_[word])));
      }
    }
    return kNullLevel;
  }
  [[nodiscard]] LevelIndex highest_occupied() const noexcept {
    for (std::size_t word = bitmap_words_; word-- > 0;) {
      if (occupied_[word] != 0) {
        const unsigned msb = 63U - static_cast<unsigned>(__builtin_clzll(occupied_[word]));
        return static_cast<LevelIndex>((word * 64U) + msb);
      }
    }
    return kNullLevel;
  }

  [[nodiscard]] OrderArena& arena() noexcept {
    return *arena_;
  }
  [[nodiscard]] const OrderArena& arena() const noexcept {
    return *arena_;
  }
  [[nodiscard]] OrderIndexTable& id_index() noexcept {
    return *index_;
  }
  [[nodiscard]] const OrderIndexTable& id_index() const noexcept {
    return *index_;
  }

  [[nodiscard]] const SymbolConfig& config() const noexcept {
    return cfg_;
  }

  // ---- extremes -----------------------------------------------------------

  [[nodiscard]] LevelIndex best_bid_level() const noexcept {
    return best_bid_;
  }
  [[nodiscard]] LevelIndex best_ask_level() const noexcept {
    return best_ask_;
  }

  [[nodiscard]] OrderIndex best_bid_order() const noexcept {
    return best_bid_ == kNullLevel ? kNullOrder : levels_[best_bid_].head;
  }
  [[nodiscard]] OrderIndex best_ask_order() const noexcept {
    return best_ask_ == kNullLevel ? kNullOrder : levels_[best_ask_].head;
  }

  [[nodiscard]] TopOfBook top_of_book() const noexcept;

  /// Count of non-empty levels, maintained incrementally.
  [[nodiscard]] std::uint32_t active_levels() const noexcept {
    return active_levels_;
  }

  /// Total resting quantity across both sides.
  [[nodiscard]] Quantity total_resting_qty() const noexcept {
    return total_qty_;
  }

  // ---- determinism --------------------------------------------------------

  /// Order-independent, pointer-free digest of the whole book.
  ///
  /// Two books that have seen the same input stream must produce the same hash,
  /// and a replayed engine must reproduce the original's hash exactly. It is
  /// computed by walking levels in price order and orders in queue order, so it
  /// depends only on observable state.
  [[nodiscard]] std::uint64_t state_hash() const noexcept;

  /// Full structural check of every invariant. Returns false and writes a
  /// human-readable reason on the first violation. Only called from
  /// debug/invariant builds.
  [[nodiscard]] bool check_invariants(std::string_view* error) const noexcept;

 private:
  [[nodiscard]] Level& level(LevelIndex idx) noexcept {
    return levels_[idx];
  }
  [[nodiscard]] const Level& level(LevelIndex idx) const noexcept {
    return levels_[idx];
  }

  void set_occupied(LevelIndex idx, bool occupied) noexcept;
  /// Nearest occupied level strictly below / above `idx`, or kNullLevel.
  ///
  /// Side-agnostic: the bitmap marks a level occupied regardless of which side
  /// it holds. Use the side-aware scans below when maintaining an extreme.
  [[nodiscard]] LevelIndex prev_occupied(std::size_t idx) const noexcept;
  [[nodiscard]] LevelIndex next_occupied(std::size_t idx) const noexcept;

  /// Nearest occupied level *holding the given side* strictly below / above
  /// `idx`.
  ///
  /// The occupancy bitmap is shared by both sides, so a plain bitmap scan can
  /// return a level holding the opposite side. Using one of those to re-point
  /// best_bid_ or best_ask_ makes an aggressor match against the wrong side,
  /// which leaves the book crossed -- a trade printing where the opposite side
  /// was resting. These are the only correct way to refresh an extreme.
 public:
  [[nodiscard]] LevelIndex find_bid_below(std::size_t idx) const noexcept;
  [[nodiscard]] LevelIndex find_ask_above(std::size_t idx) const noexcept;

 private:
  SymbolConfig cfg_;
  std::int64_t min_price_{0};
  std::int64_t max_price_{0};
  std::uint32_t domain_{0};
  std::size_t bitmap_words_{0};

  std::vector<Level> levels_;
  std::vector<std::uint64_t> occupied_;
  OrderArena* arena_;
  OrderIndexTable* index_;

  LevelIndex best_bid_{kNullLevel};
  LevelIndex best_ask_{kNullLevel};
  std::uint32_t active_levels_{0};
  Quantity total_qty_{0};
};

}  // namespace lob