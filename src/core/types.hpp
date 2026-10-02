#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace lob {

// ---------------------------------------------------------------------------
// Strong types
// ---------------------------------------------------------------------------
//
// Every domain integer gets its own type so a Price can never be passed where a
// Quantity belongs. That class of bug is invisible in review and catastrophic
// in a venue, and it is exactly the sort of mistake that -Wconversion cannot
// catch. The wrappers are thin: no virtuals, no padding games, trivially
// copyable, so they cost nothing on the hot path.

/// Venue-assigned order identifier. Opaque to the engine: the core never
/// assumes these are dense or ordered.
struct OrderId {
  std::uint64_t value{};

  constexpr OrderId() noexcept = default;
  constexpr explicit OrderId(std::uint64_t v) noexcept : value(v) {}

  friend auto operator<=>(const OrderId&, const OrderId&) noexcept = default;
};

/// Internal symbol index. Dense and small on purpose: it is the key of the
/// shard hash and of per-symbol configuration arrays.
struct SymbolId {
  std::uint32_t value{};

  constexpr SymbolId() noexcept = default;
  constexpr explicit SymbolId(std::uint32_t v) noexcept : value(v) {}

  friend auto operator<=>(const SymbolId&, const SymbolId&) noexcept = default;
};

/// Trading participant, used by risk limits and self-trade prevention.
struct ParticipantId {
  std::uint32_t value{};

  constexpr ParticipantId() noexcept = default;
  constexpr explicit ParticipantId(std::uint32_t v) noexcept : value(v) {}

  friend auto operator<=>(const ParticipantId&, const ParticipantId&) noexcept = default;
};

/// Venue-wide monotonic event counter. Assigned by the engine to every event.
struct Sequence {
  std::uint64_t value{};

  constexpr Sequence() noexcept = default;
  constexpr explicit Sequence(std::uint64_t v) noexcept : value(v) {}

  friend auto operator<=>(const Sequence&, const Sequence&) noexcept = default;
};

/// Monotonically increasing trade identifier, separate from Sequence so that
/// trade ids stay dense and can be used as a settlement reference.
struct TradeId {
  std::uint64_t value{};

  constexpr TradeId() noexcept = default;
  constexpr explicit TradeId(std::uint64_t v) noexcept : value(v) {}

  friend auto operator<=>(const TradeId&, const TradeId&) noexcept = default;
};

/// Nanoseconds since the venue epoch. Never read from a clock inside the core:
/// it is always injected by the caller so that replays are reproducible.
struct Timestamp {
  std::int64_t value{};

  constexpr Timestamp() noexcept = default;
  constexpr explicit Timestamp(std::int64_t v) noexcept : value(v) {}

  friend auto operator<=>(const Timestamp&, const Timestamp&) noexcept = default;
};

// ---------------------------------------------------------------------------
// Price
// ---------------------------------------------------------------------------

/// A price in integer ticks. Signed because a symbol may legitimately trade
/// below the "natural" origin (a spread bet, a negative-rate instrument), and
/// because level indices are computed as `price - min_price` internally.
///
/// The core never stores or computes a floating point price. The tick size is
/// applied by the protocol layer, which converts a decimal price into ticks on
/// ingest, so that the engine never has to think about rounding.
struct Price {
  std::int64_t value{};

  constexpr Price() noexcept = default;
  constexpr explicit Price(std::int64_t v) noexcept : value(v) {}

  friend constexpr bool operator==(const Price&, const Price&) noexcept = default;
  friend constexpr auto operator<=>(const Price&, const Price&) noexcept = default;

  /// True when the tick index would fit in the uint32 used by the level grid.
  /// Values outside this range cannot be represented in the grid and must be
  /// rejected by risk checks before they reach the book.
  [[nodiscard]] constexpr bool fits_tick_index() const noexcept {
    return value >= 0 &&
           value <= static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
  }
};

// ---------------------------------------------------------------------------
// Quantity
// ---------------------------------------------------------------------------

/// A quantity in integer lots. Signed because the wire protocol must be able to
/// carry a negative value that validation then rejects; the book itself never
/// stores a negative quantity (asserted by an invariant check).
struct Quantity {
  std::int64_t value{};

  constexpr Quantity() noexcept = default;
  constexpr explicit Quantity(std::int64_t v) noexcept : value(v) {}

  friend constexpr bool operator==(const Quantity&, const Quantity&) noexcept = default;
  friend constexpr auto operator<=>(const Quantity&, const Quantity&) noexcept = default;

  friend constexpr Quantity operator+(Quantity a, Quantity b) noexcept {
    return Quantity{a.value + b.value};
  }
  friend constexpr Quantity operator-(Quantity a, Quantity b) noexcept {
    return Quantity{a.value - b.value};
  }
  constexpr Quantity& operator+=(Quantity b) noexcept {
    value += b.value;
    return *this;
  }
  constexpr Quantity& operator-=(Quantity b) noexcept {
    value -= b.value;
    return *this;
  }
};

// ---------------------------------------------------------------------------
// Overflow-checked arithmetic
// ---------------------------------------------------------------------------

/// Result of a checked operation: either a value or the fact that it did not
/// fit. Deliberately not an exception, and deliberately not a bool, because the
/// caller needs the reason at the call site in the risk-check path.
template <typename T>
struct Checked {
  T value{};
  bool ok{true};

  constexpr explicit operator bool() const noexcept {
    return ok;
  }
};

/// Multiply two 64-bit quantities, reporting overflow instead of wrapping.
///
/// Notional is price x quantity, both of which are individually plausible, and
/// their product routinely exceeds 2^63 for institutional size. Computing it in
/// 128 bits and rejecting the overflow is the only honest option: silently
/// wrapping would turn a risk-limit check into a no-op exactly when it matters
/// most.
///
/// The widening is *signed*. A price may legitimately be negative (a spread bet,
/// a negative-rate instrument), and multiplying two negative operands as
/// unsigned would produce a value near 2^128 and falsely reject a perfectly
/// ordinary positive notional.
[[nodiscard]] inline Checked<std::int64_t> checked_mul(std::int64_t a, std::int64_t b) noexcept {
  const __int128 product = static_cast<__int128>(a) * static_cast<__int128>(b);
  if (product > static_cast<__int128>(std::numeric_limits<std::int64_t>::max()) ||
      product < static_cast<__int128>(std::numeric_limits<std::int64_t>::min())) {
    return Checked<std::int64_t>{0, false};
  }
  return Checked<std::int64_t>{static_cast<std::int64_t>(product), true};
}

/// Add two 64-bit quantities, reporting overflow instead of wrapping.
[[nodiscard]] inline Checked<std::int64_t> checked_add(std::int64_t a, std::int64_t b) noexcept {
  const __int128 sum = static_cast<__int128>(a) + static_cast<__int128>(b);
  if (sum > static_cast<__int128>(std::numeric_limits<std::int64_t>::max()) ||
      sum < static_cast<__int128>(std::numeric_limits<std::int64_t>::min())) {
    return Checked<std::int64_t>{0, false};
  }
  return Checked<std::int64_t>{static_cast<std::int64_t>(sum), true};
}

/// Value of the sequence at construction time, used as a sentinel for
/// "no previous sequence".
[[nodiscard]] constexpr Sequence no_sequence() noexcept {
  return Sequence{std::numeric_limits<std::uint64_t>::max()};
}

}  // namespace lob