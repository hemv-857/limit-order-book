// A deliberately naive reference implementation of the matching rules.
//
// It exists to be obviously correct, not fast. Prices live in std::map, the
// queue at each price is a std::deque, and every order attribute lives in one
// std::map keyed by order id -- level queues hold ids only, so there is exactly
// one copy of an order and no way for two structures to disagree about it.
// Nothing is pre-allocated, index-based or clever, and each operation is written
// the way docs/MATCHING_RULES.md reads, so a disagreement with the production
// engine indicts the production engine.
//
// It shares exactly one thing with the production code: the pure request
// validator (validate_new_order). Re-implementing 27 reject-code rules here
// would test the copy rather than the code; the matching and book semantics,
// which is where the complexity actually lives, are implemented independently.
//
// It emits the same lob::Event values, compared through the same
// Event::to_string(), so the differential test compares one textual contract
// rather than two comparators that could both be wrong the same way.

#pragma once

#include "core/config.hpp"
#include "core/engine.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lob::reference {

/// One order's complete state. There is exactly one of these per live order.
struct Order {
  OrderId id{};
  ParticipantId participant{};
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  TimeInForce tif{TimeInForce::GTC};
  Price price{};       ///< resting price, or the trigger while a stop is pending
  Price stop_limit{};  ///< the limit a StopLimit works at once triggered
  Price trigger_price{};
  Quantity total{};
  Quantity leaves{};
  Quantity filled{};
  Quantity display{};  ///< 0 means fully displayed
  Sequence seq{};
  bool post_only{false};

  /// Displayed size: one slice for an iceberg, everything otherwise.
  [[nodiscard]] Quantity visible() const {
    if (display.value == 0) {
      return leaves;
    }
    return leaves.value < display.value ? leaves : display;
  }
};

/// Where an order is, so removal can find it without a scan.
///
/// `queued` separates "known to the id map" from "linked into a price level".
/// An aggressor is live from the moment it is accepted but is in no queue until
/// its remainder rests, so an order can be live and unqueued at the same time.
/// Conflating the two makes an unqueued order impossible to remove.
struct Entry {
  Order order;
  Price price{};
  Side side{Side::Buy};
  bool is_stop{false};
  bool queued{false};
};

class ReferenceEngine {
 public:
  explicit ReferenceEngine(std::vector<SymbolConfig> configs);

  void submit(const NewOrderRequest& request);
  void submit(const CancelRequest& request);
  void submit(const ReplaceRequest& request);
  void submit(const MassCancelRequest& request);
  void on_session_end(Timestamp now);

  [[nodiscard]] const std::vector<Event>& events() const noexcept {
    return events_;
  }
  void clear_events() {
    events_.clear();
  }

  /// Top of book, for divergence reporting.
  [[nodiscard]] TopOfBook book_top(SymbolId symbol) const;

  /// Canonical rendering of all resting liquidity, price order then queue order.
  /// Compared against the production engine's equivalent to prove the two books
  /// hold the same orders in the same priority order.
  [[nodiscard]] std::vector<std::string> book_digest() const;

 private:
  /// The queue at one price, holding order ids only.
  ///
  /// Deliberately not lob::Level: every attribute of every order lives in the
  /// single `live` map, so the two structures can never disagree about an
  /// order, which is exactly the class of bug this engine exists to catch.
  struct Level {
    Side side{Side::Buy};
    std::deque<OrderId> queue;
    Quantity aggregate{};
  };

  struct Symbol {
    SymbolId symbol_id{};
    SymbolConfig config{};
    std::map<Price, Level> bids;  ///< ascending; best bid is the last entry
    std::map<Price, Level> asks;  ///< ascending; best ask is the first entry
    std::map<Price, Level> stop_bids;
    std::map<Price, Level> stop_asks;
    /// The single source of truth for every live order's attributes.
    std::map<OrderId, Entry> live;

    bool has_last_trade{false};
    Price last_trade{};
    std::uint64_t trade_count{0};
    std::uint64_t accepted_qty{0};
    std::uint64_t filled_qty{0};
    std::uint64_t removed_qty{0};
  };

  [[nodiscard]] Symbol& sym(SymbolId id) {
    return symbols_[id.value];
  }
  [[nodiscard]] const Symbol& sym(SymbolId id) const {
    return symbols_[id.value];
  }

  [[nodiscard]] static std::map<Price, Level>& liquidity(Symbol& s, Side side);
  [[nodiscard]] static const std::map<Price, Level>& liquidity(const Symbol& s, Side side);
  [[nodiscard]] static std::map<Price, Level>& stops(Symbol& s, Side side);
  [[nodiscard]] static const std::map<Price, Level>& stops(const Symbol& s, Side side);
  [[nodiscard]] static std::map<Price, Level>* book_of(Symbol& s, Side side, bool is_stop);

  [[nodiscard]] std::optional<Price> best_bid_of(const Symbol& s) const;
  [[nodiscard]] std::optional<Price> best_ask_of(const Symbol& s) const;
  [[nodiscard]] Quantity aggregate_at(const Symbol& s, Side side, Price price) const;

  /// Every live order id in the order the production engine would visit them:
  /// liquidity first, in ascending price order across *both* sides (the engine
  /// has a single price grid, so a buy at 8 is visited before a sell at 30),
  /// then pending stop-buy orders, then pending stop-sell orders.
  ///
  /// Cancellation order is observable in the event stream, so the reference has
  /// to collect victims the same way or the two engines disagree on sequences
  /// that are semantically equivalent.
  [[nodiscard]] std::vector<OrderId> visit_order(const Symbol& s) const;

  Event new_event(SymbolId symbol, EventType type, Timestamp ts);
  void emit_delta(SymbolId symbol, Price price, Side side, Quantity aggregate, UpdateAction action);
  void emit_level_delta(SymbolId symbol, Side side, Price price, bool emptied);

  /// Remove `id` from its queue. Reports the price it sat at and whether it was
  /// a pending stop. Empty levels are erased so best bid/ask needs no special
  /// cases.
  [[nodiscard]] bool unlink(Symbol& s, OrderId id, Price* out_price, bool* out_is_stop,
                            bool* out_was_queued);

  [[nodiscard]] const Order& order_at(const Symbol& s, OrderId id) const;
  [[nodiscard]] Order& order_ref(Symbol& s, OrderId id);

  void fail_new(Symbol& s, const NewOrderRequest& r, RejectCode code, bool counts_as_acceptance);
  void handle_new(Symbol& s, NewOrderRequest request, bool counts_as_acceptance);
  [[nodiscard]] bool match(Symbol& s, const NewOrderRequest& request);
  void rest_remainder(Symbol& s, OrderId id);
  void cancel_live(Symbol& s, OrderId id, CancelReason reason, Timestamp ts);
  /// Unlink and unindex with no events. Used for fully filled orders and for an
  /// aggressor discarded by self-trade prevention.
  void retire(Symbol& s, OrderId id);
  void replace_stop(Symbol& s, const ReplaceRequest& request);

  void collect_triggers(Symbol& s, Price trade_price);
  void dispatch_stop_level(Symbol& s, Side side, Price trigger);
  void drain_pending(Symbol& s);

  std::vector<Symbol> symbols_;
  std::vector<Event> events_;
  std::vector<NewOrderRequest> pending_;
  std::uint64_t next_seq_{1};
};

}  // namespace lob::reference