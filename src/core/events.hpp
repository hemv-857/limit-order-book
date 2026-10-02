#pragma once

#include "core/config.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <string_view>

namespace lob {

/// What happened, as seen by a client or a market data consumer.
enum class EventType : std::uint8_t {
  Accepted = 0,       ///< the order is live (possibly immediately filled)
  Rejected = 1,       ///< the order never became live; carries a RejectCode
  Cancelled = 2,      ///< remainder removed: by request, expiry, or IOC leftover
  Replaced = 3,       ///< a replace request was applied
  Trade = 4,          ///< a fill, from the taker's point of view
  BookUpdate = 5,     ///< L2 delta for one price level
  StopTriggered = 6,  ///< a stop order fired and became a working order
  Expired = 7,        ///< a session boundary removed resting orders
};

[[nodiscard]] std::string_view to_string(EventType t) noexcept;

/// Why a Cancelled event happened.
enum class CancelReason : std::uint8_t {
  ClientRequest = 0,      ///< explicit cancel or replace
  ImmediateOrCancel = 1,  ///< remainder of an IOC or market order
  TimeInForce = 2,        ///< session end expired a Day order
  StopCancelled = 3,      ///< stop order removed without triggering
  SelfTradePrevention = 4,
  SessionEnd = 5,
};

[[nodiscard]] std::string_view to_string(CancelReason r) noexcept;

/// Why a BookUpdate delta happened, so a consumer can distinguish a level that
/// emptied from one that merely shrank.
enum class UpdateAction : std::uint8_t {
  Changed = 0,  ///< level still exists, aggregate changed
  Added = 1,    ///< level became occupied
  Removed = 2,  ///< level became empty
};

[[nodiscard]] std::string_view to_string(UpdateAction a) noexcept;

/// One output event.
///
/// A single flat struct rather than std::variant: the engine emits millions of
/// these per second and the flat form is a single cache-friendly copy, trivially
/// memcmp-able for the differential test, and directly serialisable into the
/// journal without a per-type writer. The tag plus a fixed payload costs a few
/// bytes of dead weight per event and buys all of that.
///
/// Payload by type (see docs/MATCHING_RULES.md for the full contract):
///
/// | field        | Accepted/Cancelled/Replaced | Rejected | Trade            | BookUpdate      |
/// |--------------|-----------------------------|----------|------------------|-----------------|
/// | order_id     | the order                   | the order| the taker order  | -               |
/// | trade_id     | -                           | -        | the trade        | -               |
/// | price        | order price                 | -        | fill price       | level price     |
/// | qty          | order quantity              | -        | fill quantity    | new aggregate   |
/// | leaves_qty   | quantity left resting       | -        | -                | -               |
/// | maker_price  | -                           | -        | maker limit      | -               |
/// | maker        | -                           | -        | maker participant| -               |
/// | maker_order_id | -                        | -        | maker order      | -               |
/// | participant  | order's participant         | -        | taker participant| -               |
/// | reject_code  | -                           | the code | -                | -               |
/// | type         | order type                  | -        | -                | -               |
/// | side         | order side                  | -        | taker side       | level side      |
/// | action       | -                           | -        | -                | UpdateAction    |
struct Event {
  Sequence seq{};
  Timestamp ts{};
  TradeId trade_id{};
  OrderId order_id{};
  /// The resting order that was filled. A client needs this to reconcile a fill
  /// against its own order book; reporting only the maker's participant and
  /// price is not enough to identify which order was consumed.
  OrderId maker_order_id{};
  SymbolId symbol{};
  ParticipantId participant{};
  ParticipantId maker{};
  Price price{};
  Price maker_price{};
  Quantity qty{};
  Quantity leaves_qty{};
  EventType type{EventType::Accepted};
  Side side{Side::Buy};
  RejectCode reject_code{RejectCode::None};
  CancelReason reason{CancelReason::ClientRequest};
  OrderType order_type{OrderType::Limit};
  UpdateAction action{UpdateAction::Changed};
  TimeInForce tif{TimeInForce::GTC};
  bool post_only{false};

  /// Deterministic textual form. Used by the differential test and the golden
  /// files, so its format is part of the test contract: it must not depend on
  /// anything but the struct's own fields.
  [[nodiscard]] std::string to_string() const;
};

[[nodiscard]] std::string_view to_string(OrderType type) noexcept;

}  // namespace lob