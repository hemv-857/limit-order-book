#include "core/validate.hpp"

#include "core/engine.hpp"

#include <cstdlib>
#include <string_view>

namespace lob {
namespace {

/// True when `value` is an exact multiple of `multiple`. A multiple of zero
/// would divide by zero, so it is treated as "no constraint", which is how a
/// symbol with no tick or lot restriction is expressed.
[[nodiscard]] constexpr bool on_grid(std::int64_t value, std::int64_t multiple) noexcept {
  return multiple <= 1 || value % multiple == 0;
}

}  // namespace

RejectCode validate_new_order(const SymbolState& state, const NewOrderRequest& request) {
  const SymbolConfig& cfg = state.config;

  // 1. quantity must be a positive multiple of the lot size, within limits.
  if (request.quantity.value <= 0) {
    return RejectCode::InvalidQuantity;
  }
  if (!on_grid(request.quantity.value, cfg.lot_size)) {
    return RejectCode::QuantityNotOnLot;
  }
  if (request.quantity.value > cfg.max_order_qty) {
    return RejectCode::OrderSizeExceeded;
  }

  // 2. Iceberg display slice must be positive, smaller than the total, and on
  //    the lot grid, otherwise it cannot produce a sensible replenishment.
  if (request.display_qty.value != 0) {
    if (request.display_qty.value <= 0 || request.display_qty.value >= request.quantity.value) {
      return RejectCode::InvalidQuantity;
    }
    if (!on_grid(request.display_qty.value, cfg.lot_size)) {
      return RejectCode::QuantityNotOnLot;
    }
  }

  // 3. Order type must be one the venue supports.
  if (static_cast<std::uint8_t>(request.type) > static_cast<std::uint8_t>(OrderType::StopLimit)) {
    return RejectCode::InvalidOrderType;
  }
  if (static_cast<std::uint8_t>(request.tif) > static_cast<std::uint8_t>(TimeInForce::FOK)) {
    return RejectCode::InvalidTimeInForce;
  }
  // A market order has no price, so it can never be made passive: asking for
  // post-only on a market order is a contradiction rather than a rejection of
  // the order's intent.
  if (request.type == OrderType::Market && request.post_only) {
    return RejectCode::InvalidOrderType;
  }
  // Likewise post-only with fill-or-kill: it can never match, so FOK could only
  // ever resolve to a cancel.
  if (request.post_only && request.tif == TimeInForce::FOK) {
    return RejectCode::InvalidTimeInForce;
  }

  // 4. Price checks. Only Limit and StopLimit carry a limit price: a Market
  //    order has none, and neither does a Stop, which becomes a market order
  //    when it triggers. Validating a price field those orders do not have would
  //    reject every stop order a client sends correctly.
  const bool has_limit_price =
      request.type == OrderType::Limit || request.type == OrderType::StopLimit;
  if (!has_limit_price) {
    // nothing to check
  } else if (request.price.value <= 0) {
    return RejectCode::InvalidPrice;
  } else if (!on_grid(request.price.value, cfg.tick_size)) {
    return RejectCode::PriceNotOnTick;
  } else if (!cfg.contains(request.price.value)) {
    return RejectCode::PriceOutOfRange;
  } else if (cfg.collar_low != 0 && request.price.value < cfg.collar_low) {
    return RejectCode::PriceBelowCollar;
  } else if (cfg.collar_high != 0 && request.price.value > cfg.collar_high) {
    return RejectCode::PriceAboveCollar;
  }

  // 5. Stop trigger checks.
  if (is_stop_type(request.type)) {
    if (!cfg.stop_enabled) {
      return RejectCode::InvalidOrderType;
    }
    if (request.trigger_price.value <= 0) {
      return RejectCode::InvalidStopPrice;
    }
    if (!on_grid(request.trigger_price.value, cfg.tick_size)) {
      return RejectCode::PriceNotOnTick;
    }
    if (!cfg.contains(request.trigger_price.value)) {
      return RejectCode::InvalidStopPrice;
    }
    if (cfg.collar_low != 0 && request.trigger_price.value < cfg.collar_low) {
      return RejectCode::InvalidStopPrice;
    }
    if (cfg.collar_high != 0 && request.trigger_price.value > cfg.collar_high) {
      return RejectCode::InvalidStopPrice;
    }
    // A buy stop must sit above the market and a sell stop below it, otherwise
    // it is not a stop: it would be triggerable on arrival. Checked against the
    // last trade price, and skipped when the symbol has never traded, since
    // there is then no reference to be wrong about.
    if (state.has_last_trade) {
      const std::int64_t trigger = request.trigger_price.value;
      const std::int64_t last = state.last_trade_price.value;
      if (request.side == Side::Buy && trigger <= last) {
        return RejectCode::InvalidStopDirection;
      }
      if (request.side == Side::Sell && trigger >= last) {
        return RejectCode::InvalidStopDirection;
      }
    }
    // A stop-limit needs both a trigger and a limit price.
    if (request.type == OrderType::StopLimit) {
      if (request.price.value <= 0) {
        return RejectCode::InvalidPrice;
      }
      if (!on_grid(request.price.value, cfg.tick_size)) {
        return RejectCode::PriceNotOnTick;
      }
      if (!cfg.contains(request.price.value)) {
        return RejectCode::PriceOutOfRange;
      }
    }
  }

  // 6. Notional. Checked in 128 bits: for institutional sizes a 64-bit product
  //    overflows, and an overflowed notional would silently defeat the limit.
  //    Only meaningful when the order has a price to multiply by.
  if (has_limit_price) {
    const auto notional = checked_mul(request.price.value, request.quantity.value);
    if (!notional.ok || notional.value > cfg.max_notional) {
      return RejectCode::NotionalExceeded;
    }
  }

  return RejectCode::None;
}

// ---------------------------------------------------------------------------
// Human-readable names. These strings appear in the wire protocol's symbolic
// documentation and in the golden files, so they are part of the test contract.
// ---------------------------------------------------------------------------

std::string_view to_string(RejectCode code) noexcept {
  switch (code) {
    case RejectCode::None:
      return "none";
    case RejectCode::UnknownSymbol:
      return "unknown_symbol";
    case RejectCode::InvalidQuantity:
      return "invalid_quantity";
    case RejectCode::InvalidPrice:
      return "invalid_price";
    case RejectCode::PriceNotOnTick:
      return "price_not_on_tick";
    case RejectCode::QuantityNotOnLot:
      return "quantity_not_on_lot";
    case RejectCode::OrderSizeExceeded:
      return "order_size_exceeded";
    case RejectCode::NotionalExceeded:
      return "notional_exceeded";
    case RejectCode::PriceBelowCollar:
      return "price_below_collar";
    case RejectCode::PriceAboveCollar:
      return "price_above_collar";
    case RejectCode::BookFull:
      return "book_full";
    case RejectCode::RateLimitExceeded:
      return "rate_limit_exceeded";
    case RejectCode::DuplicateOrderId:
      return "duplicate_order_id";
    case RejectCode::UnknownOrder:
      return "unknown_order";
    case RejectCode::WouldCrossPostOnly:
      return "would_cross_post_only";
    case RejectCode::FokInsufficientLiquidity:
      return "fok_insufficient_liquidity";
    case RejectCode::InvalidTimeInForce:
      return "invalid_time_in_force";
    case RejectCode::InvalidStopPrice:
      return "invalid_stop_price";
    case RejectCode::InvalidStopDirection:
      return "invalid_stop_direction";
    case RejectCode::InvalidOrderType:
      return "invalid_order_type";
    case RejectCode::ParticipantNotFound:
      return "participant_not_found";
    case RejectCode::SessionClosed:
      return "session_closed";
    case RejectCode::PriceOutOfRange:
      return "price_out_of_range";
    case RejectCode::ReplaceWouldReduceBelowFilled:
      return "replace_below_filled";
    case RejectCode::ReplaceNoPriceChange:
      return "replace_no_price_change";
    case RejectCode::ShuttingDown:
      return "shutting_down";
    case RejectCode::InvalidField:
      return "invalid_field";
    case RejectCode::CancelExceedsLeaves:
      return "cancel_exceeds_leaves";
  }
  return "unknown";
}

std::string_view to_string(OrderType type) noexcept {
  switch (type) {
    case OrderType::Limit:
      return "limit";
    case OrderType::Market:
      return "market";
    case OrderType::Stop:
      return "stop";
    case OrderType::StopLimit:
      return "stop_limit";
  }
  return "unknown";
}

std::string_view to_string(UpdateAction action) noexcept {
  switch (action) {
    case UpdateAction::Changed:
      return "changed";
    case UpdateAction::Added:
      return "added";
    case UpdateAction::Removed:
      return "removed";
  }
  return "unknown";
}

std::string_view to_string(CancelReason reason) noexcept {
  switch (reason) {
    case CancelReason::ClientRequest:
      return "client";
    case CancelReason::ImmediateOrCancel:
      return "immediate_or_cancel";
    case CancelReason::TimeInForce:
      return "time_in_force";
    case CancelReason::StopCancelled:
      return "stop_cancelled";
    case CancelReason::SelfTradePrevention:
      return "self_trade_prevention";
    case CancelReason::SessionEnd:
      return "session_end";
  }
  return "unknown";
}

std::string_view to_string(EventType type) noexcept {
  switch (type) {
    case EventType::Accepted:
      return "accepted";
    case EventType::Rejected:
      return "rejected";
    case EventType::Cancelled:
      return "cancelled";
    case EventType::Replaced:
      return "replaced";
    case EventType::Trade:
      return "trade";
    case EventType::BookUpdate:
      return "book_update";
    case EventType::StopTriggered:
      return "stop_triggered";
    case EventType::Expired:
      return "expired";
  }
  return "unknown";
}

std::string_view to_string(StpMode mode) noexcept {
  switch (mode) {
    case StpMode::None:
      return "none";
    case StpMode::CancelOldest:
      return "cancel_oldest";
    case StpMode::CancelNewest:
      return "cancel_newest";
    case StpMode::CancelBoth:
      return "cancel_both";
    case StpMode::DecrementAndCancel:
      return "decrement_and_cancel";
  }
  return "unknown";
}

std::string_view to_string(PostOnlyAction action) noexcept {
  switch (action) {
    case PostOnlyAction::Reject:
      return "reject";
    case PostOnlyAction::Slide:
      return "slide";
  }
  return "unknown";
}

}  // namespace lob