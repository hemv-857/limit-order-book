#pragma once

#include "core/config.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <limits>

namespace lob {

/// Index into the per-symbol order arena.
///
/// The book links orders with indices rather than pointers, which buys three
/// things at once: the arena array is never reallocated so a link can never be
/// invalidated by unrelated growth, there are no owning pointers anywhere in the
/// core, and the hot fields of neighbouring orders stay on the same cache line
/// because the index is small and dense.
using OrderIndex = std::uint32_t;

/// Index into the per-symbol price grid. Same rationale as OrderIndex.
using LevelIndex = std::uint32_t;

/// Sentinel for "no order". Chosen as the maximum index so that a zeroed arena
/// (which would mean slot 0) can never be mistaken for a valid link.
inline constexpr OrderIndex kNullOrder = std::numeric_limits<OrderIndex>::max();

/// Sentinel for "no level".
inline constexpr LevelIndex kNullLevel = std::numeric_limits<LevelIndex>::max();

/// Bit flags on an Order. Plain constants rather than an enum or a bool trio:
/// flags combine with a single OR, cost one byte instead of three, and keep
/// "is this order special" to one test.
namespace flag {
inline constexpr std::uint8_t kNone = 0U;
inline constexpr std::uint8_t kPostOnly = 1U << 0U;
/// Set once a stop order has fired and been converted into a working order.
inline constexpr std::uint8_t kTriggered = 1U << 1U;
/// Set while the order currently shows a visible slice on the book (always
/// true for a non-iceberg order).
inline constexpr std::uint8_t kDisplayed = 1U << 2U;
}  // namespace flag

/// A resting or working order.
///
/// Layout is ordered hot-to-cold on purpose. The matching loop touches price,
/// leaves, side and the queue links for every maker it walks; participant, type
/// and the iceberg fields are read once per order at most. Field order is
/// verified against the expected size by a static_assert in order.cpp.
struct Order {
  // ---- hot: read or written on every matching step ----
  OrderId order_id;              ///< venue-assigned, opaque to the engine
  Price price;                   ///< limit price; the resting price for a limit
  Quantity leaves_qty;           ///< quantity still available to trade
  Quantity filled_qty;           ///< quantity already executed
  OrderIndex prev{kNullOrder};   ///< previous order at the same price level
  OrderIndex next{kNullOrder};   ///< next order at the same price level
  LevelIndex level{kNullLevel};  ///< owning price level, kNullLevel when unlinked
  Side side{Side::Buy};
  std::uint8_t flags{0};
  OrderType type{OrderType::Limit};
  TimeInForce tif{TimeInForce::GTC};

  // ---- cold: touched once per order, not once per match ----
  Price trigger_price;   ///< stop trigger; only meaningful for stop types
  Quantity total_qty;    ///< original order quantity
  Quantity display_qty;  ///< iceberg visible slice; 0 means fully visible
  Sequence arrival_seq;  ///< arrival order; the tiebreaker of last resort
  ParticipantId participant;
  OrderIndex free_next{kNullOrder};  ///< free-list link while the slot is unused
  /// Bumped on every reuse. Must never be 0: OrderIndexTable uses 0 as its
  /// free-slot marker, so a live order carrying 0 would be invisible to lookup.
  std::uint32_t generation{1};

  /// Visible size currently shown to the market. For a plain order this is the
  /// whole remaining quantity; for an iceberg it is one slice.
  [[nodiscard]] constexpr Quantity visible_qty() const noexcept {
    if (display_qty.value == 0) {
      return leaves_qty;
    }
    return leaves_qty.value < display_qty.value ? leaves_qty : display_qty;
  }

  /// True when the order holds reserve quantity behind a displayed slice.
  [[nodiscard]] constexpr bool is_iceberg() const noexcept {
    return display_qty.value > 0;
  }

  [[nodiscard]] constexpr bool is_buy() const noexcept {
    return side == Side::Buy;
  }

  /// True when the order will never rest, so it can never be cancelled as a
  /// resting order and has no queue position.
  [[nodiscard]] constexpr bool is_immediate() const noexcept {
    return type == OrderType::Market || tif == TimeInForce::IOC || tif == TimeInForce::FOK;
  }

  /// True while the order is linked into a price level.
  [[nodiscard]] constexpr bool is_resting() const noexcept {
    return level != kNullLevel;
  }
};

static_assert(sizeof(Order) <= 96, "Order grew past its cache-friendly budget");

}  // namespace lob