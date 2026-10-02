#pragma once

#include "core/book.hpp"
#include "core/config.hpp"
#include "core/events.hpp"
#include "core/stp.hpp"
#include "core/types.hpp"

namespace lob {

struct NewOrderRequest;
struct SymbolState;

/// True for order types that rest outside the book until triggered.
[[nodiscard]] constexpr bool is_stop_type(OrderType type) noexcept {
  return type == OrderType::Stop || type == OrderType::StopLimit;
}

/// True for order types that can never rest as liquidity.
[[nodiscard]] constexpr bool is_immediate_type(OrderType type, TimeInForce tif) noexcept {
  return type == OrderType::Market || tif == TimeInForce::IOC || tif == TimeInForce::FOK;
}

/// Static, configuration-driven validation of a new order.
///
/// Deliberately free of any dependency on the live book: every check here is a
/// property of the request and the symbol's rules, so it is trivially testable
/// and identical in the reference engine. Checks that need book state (such as
/// order-id uniqueness) belong to the engine.
///
/// Returns RejectCode::None when the order is acceptable. Checks run in a fixed
/// order so an order with several problems always reports the same one; the
/// full order is documented in docs/MATCHING_RULES.md.
[[nodiscard]] RejectCode validate_new_order(const SymbolState& state,
                                            const NewOrderRequest& request);

}  // namespace lob