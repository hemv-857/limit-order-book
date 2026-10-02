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
  explicit Book(const SymbolConfig& cfg);

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

  /// Unlink an order from its level and update the level aggregate and the
  /// cached extremes.
  void remove_from_queue(OrderIndex idx) noexcept;

  /// Move an order from the back of its current level to the back of its new
  /// level, which is how a price change or a quantity increase loses priority.
  void move_to_back_of_level(OrderIndex idx, LevelIndex new_level) noexcept;

  // ---- lookup -------------------------------------------------------------

  [[nodiscard]] OrderIndex find(OrderId id) const noexcept {
    return index_.find_any(id);
  }

  [[nodiscard]] Order& operator[](OrderIndex idx) noexcept {
    return arena_[idx];
  }
  [[nodiscard]] const Order& operator[](OrderIndex idx) const noexcept {
    return arena_[idx];
  }

  [[nodiscard]] OrderArena& arena() noexcept {
    return arena_;
  }
  [[nodiscard]] const OrderArena& arena() const noexcept {
    return arena_;
  }
  OrderIndexTable& id_index() noexcept {
    return index_;
  }
  const OrderIndexTable& id_index() const noexcept {
    return index_;
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
  [[nodiscard]] LevelIndex prev_occupied(std::size_t idx) const noexcept;
  [[nodiscard]] LevelIndex next_occupied(std::size_t idx) const noexcept;
  void refresh_best_bid() noexcept;
  void refresh_best_ask() noexcept;

  SymbolConfig cfg_;
  std::int64_t min_price_{0};
  std::int64_t max_price_{0};
  std::uint32_t domain_{0};
  std::size_t bitmap_words_{0};

  std::vector<Level> levels_;
  std::vector<std::uint64_t> occupied_;
  OrderArena arena_;
  OrderIndexTable index_;

  LevelIndex best_bid_{kNullLevel};
  LevelIndex best_ask_{kNullLevel};
  std::uint32_t active_levels_{0};
  Quantity total_qty_{0};
};

}  // namespace lob