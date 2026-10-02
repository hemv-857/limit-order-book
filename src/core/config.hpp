#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace lob {

// ---------------------------------------------------------------------------
// Hard structural limits
// ---------------------------------------------------------------------------
//
// These are compile-time ceilings, not tunables. They exist so that a
// misconfigured venue fails at startup with a clear message instead of
// allocating a gigabyte of price levels and then OOMing under load.

/// Largest number of price ticks a single symbol's grid may span.
///
/// The grid is a flat array indexed by `price - min_price`, so this number *is*
/// the memory cost of the book (8 bytes per tick per level struct, plus one bit
/// of occupancy). 2^20 ticks is 1,048,576 levels, roughly 12 MiB per symbol,
/// which is a sane venue-wide bound: it accommodates, for example, a symbol
/// quoted in 0.01 ticks across a 10,000-unit price band.
inline constexpr std::uint32_t kMaxPriceDomain = 1U << 20U;

/// Default per-symbol order arena capacity.
inline constexpr std::uint32_t kDefaultMaxOrdersPerSymbol = 1U << 16U;

// ---------------------------------------------------------------------------
// Order-facing enums
// ---------------------------------------------------------------------------

enum class Side : std::uint8_t {
  Buy = 0,
  Sell = 1,
};

[[nodiscard]] constexpr bool is_buy(Side s) noexcept {
  return s == Side::Buy;
}

/// Opposite side, for aggressor/participant comparison.
[[nodiscard]] constexpr Side opposite(Side s) noexcept {
  return s == Side::Buy ? Side::Sell : Side::Buy;
}

/// Human/machine readable names. These strings are part of the wire protocol
/// and of the reject-code documentation, so they must not drift.
[[nodiscard]] constexpr std::string_view to_string(Side s) noexcept {
  return s == Side::Buy ? "buy" : "sell";
}

/// Order type. Stop orders live outside the book until triggered.
enum class OrderType : std::uint8_t {
  Limit = 0,
  Market = 1,
  Stop = 2,       ///< stop-market: triggers, then executes at the market
  StopLimit = 3,  ///< triggers, then rests as a limit order at `price`
};

/// Time in force.
enum class TimeInForce : std::uint8_t {
  Day = 0,  ///< expires at session end
  GTC = 1,  ///< good until cancelled
  IOC = 2,  ///< fill what is available now, cancel the rest immediately
  FOK = 3,  ///< fill entirely now or cancel entirely
};

[[nodiscard]] constexpr std::string_view to_string(TimeInForce tif) noexcept {
  switch (tif) {
    case TimeInForce::Day:
      return "day";
    case TimeInForce::GTC:
      return "gtc";
    case TimeInForce::IOC:
      return "ioc";
    case TimeInForce::FOK:
      return "fok";
  }
  return "unknown";
}

/// What to do when an aggressor would trade with its own resting order.
enum class StpMode : std::uint8_t {
  /// Self trades are allowed (only correct for a single-participant venue).
  None = 0,
  /// Cancel the resting order (keep the incoming aggressor).
  CancelOldest = 1,
  /// Cancel the incoming aggressor (keep the resting order).
  CancelNewest = 2,
  /// Cancel both sides.
  CancelBoth = 3,
  /// Decrement the resting order by the fill quantity and cancel it if it
  /// reaches zero; the aggressor is filled in full.
  DecrementAndCancel = 4,
};

/// What the venue does with a post-only order that would cross on arrival.
enum class PostOnlyAction : std::uint8_t {
  /// Reject the order. The default, and the only lossless option.
  Reject = 0,
  /// Rest it one tick behind the touching price, converting it to passive.
  Slide = 1,
};

/// Reason an order was rejected. Every value is distinct and documented in
/// docs/MATCHING_RULES.md; the numeric values are part of the wire protocol.
enum class RejectCode : std::uint8_t {
  None = 0,
  UnknownSymbol = 1,
  InvalidQuantity = 2,                 ///< zero, negative, or overflowed on the wire
  InvalidPrice = 3,                    ///< negative for a limit, present for a market
  PriceNotOnTick = 4,                  ///< not an exact multiple of the tick size
  QuantityNotOnLot = 5,                ///< not an exact multiple of the lot size
  OrderSizeExceeded = 6,               ///< above the per-order maximum
  NotionalExceeded = 7,                ///< price x quantity above the per-order maximum
  PriceBelowCollar = 8,                ///< below the lower price band
  PriceAboveCollar = 9,                ///< above the upper price band
  BookFull = 10,                       ///< arena or price index at capacity, never resized
  RateLimitExceeded = 11,              ///< participant message rate limit
  DuplicateOrderId = 12,               ///< client reused a live order id
  UnknownOrder = 13,                   ///< cancel/replace of an order that is not live
  WouldCrossPostOnly = 14,             ///< post-only order crossed on arrival
  FokInsufficientLiquidity = 15,       ///< FOK could not fill in full
  InvalidTimeInForce = 16,             ///< e.g. FOK with no price
  InvalidStopPrice = 17,               ///< stop trigger outside the price band
  InvalidStopDirection = 18,           ///< stop price on the wrong side of the market
  InvalidOrderType = 19,               ///< unknown order type on the wire
  ParticipantNotFound = 20,            ///< session is not logged in
  SessionClosed = 21,                  ///< trading session is not open
  PriceOutOfRange = 22,                ///< outside the symbol's representable domain
  ReplaceWouldReduceBelowFilled = 23,  ///< replace below already-executed quantity
  ReplaceNoPriceChange = 24,           ///< replace that changes nothing
  ShuttingDown = 25,                   ///< engine is draining; no new risk is accepted
  InvalidField = 26,                   ///< malformed protocol field the decoder accepted
  CancelExceedsLeaves = 27,            ///< cancel quantity above the resting quantity
};

[[nodiscard]] std::string_view to_string(RejectCode code) noexcept;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/// Per-symbol trading rules and risk limits.
///
/// Every field has a safe default, and `validate()` is called once at startup so
/// that an impossible configuration is a startup error rather than a runtime
/// surprise.
struct SymbolConfig {
  /// Human-readable name. Kept for logs, config round-tripping and the CLI;
  /// the engine only ever uses the numeric SymbolId.
  std::string_view name;

  /// Inclusive bounds of the flat price grid. Together they define both the
  /// legal price range and the memory footprint of the book.
  std::int64_t min_price{0};
  std::int64_t max_price{65535};

  std::int64_t tick_size{1};  ///< prices must be an exact multiple of this
  std::int64_t lot_size{1};   ///< quantities must be an exact multiple of this

  std::int64_t max_order_qty{1'000'000};
  std::int64_t max_notional{1'000'000'000'000};

  /// Absolute price band. Orders outside it are rejected. Zero disables the
  /// bound, in which case min_price/max_price remain the hard limit.
  std::int64_t collar_low{0};
  std::int64_t collar_high{0};

  std::uint32_t max_open_orders{kDefaultMaxOrdersPerSymbol};

  StpMode stp_mode{StpMode::None};
  PostOnlyAction post_only_action{PostOnlyAction::Reject};

  /// Messages per second allowed per participant. Zero disables the limit.
  std::uint32_t rate_limit_per_second{0};

  /// Compute the number of price ticks in the grid, rejecting a domain that is
  /// too large to pre-allocate. This is what keeps the book allocation-free.
  [[nodiscard]] constexpr std::uint32_t price_domain() const noexcept {
    return static_cast<std::uint32_t>(max_price - min_price + 1);
  }
};

/// Validate a symbol configuration. `error` receives a stable, human-readable
/// reason on failure so the caller can log something actionable.
[[nodiscard]] bool validate_symbol_config(const SymbolConfig& cfg,
                                          std::string_view* error) noexcept;

/// Engine-wide settings.
struct EngineConfig {
  /// Number of single-threaded matching shards. Symbols are assigned by a
  /// stable hash of their SymbolId, so a given symbol always lands on the same
  /// shard regardless of how many shards are running.
  std::uint32_t shard_count{1};

  /// Capacity of each shard's inbound and outbound ring buffers.
  std::uint32_t ring_capacity{1U << 14U};

  /// Verify invariants after every operation. Costs throughput, so it is a
  /// debug-only switch driven by NDEBUG rather than a runtime flag.
  static constexpr bool invariants_enabled() noexcept {
#ifdef NDEBUG
    return false;
#else
    return true;
#endif
  }
};

}  // namespace lob