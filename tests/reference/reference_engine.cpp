// Implementation of the naive reference engine. See the header for why it is
// written this way.

#include "reference_engine.hpp"

#include "core/stp.hpp"
#include "core/validate.hpp"

#include <algorithm>
#include <utility>

namespace lob::reference {
namespace {

bool crosses(Side side, Price order_price, Price level_price) {
  return side == Side::Buy ? level_price <= order_price : level_price >= order_price;
}

Quantity qmin(Quantity a, Quantity b) {
  return a.value <= b.value ? a : b;
}

/// A stop's arrival sequence doubles as its timestamp when it fires.
/// Narrowed once, in one place: Sequence is unsigned and Timestamp is signed.
Timestamp ts_of(Sequence seq) {
  return Timestamp{static_cast<std::int64_t>(seq.value)};
}

}  // namespace

ReferenceEngine::ReferenceEngine(std::vector<SymbolConfig> configs) {
  symbols_.resize(configs.size());
  for (std::size_t i = 0; i < configs.size(); ++i) {
    symbols_[i].config = configs[i];
    symbols_[i].symbol_id = SymbolId{static_cast<std::uint32_t>(i)};
  }
}

// ---------------------------------------------------------------------------
// Plumbing
// ---------------------------------------------------------------------------

Event ReferenceEngine::new_event(SymbolId symbol, EventType type, Timestamp ts) {
  Event e;
  e.seq = Sequence{next_seq_++};
  e.ts = ts;
  e.symbol = symbol;
  e.type = type;
  return e;
}

std::map<Price, ReferenceEngine::Level>& ReferenceEngine::liquidity(Symbol& s, Side side) {
  return side == Side::Buy ? s.bids : s.asks;
}

const std::map<Price, ReferenceEngine::Level>& ReferenceEngine::liquidity(const Symbol& s,
                                                                          Side side) {
  return side == Side::Buy ? s.bids : s.asks;
}

std::map<Price, ReferenceEngine::Level>& ReferenceEngine::stops(Symbol& s, Side side) {
  return side == Side::Buy ? s.stop_bids : s.stop_asks;
}

const std::map<Price, ReferenceEngine::Level>& ReferenceEngine::stops(const Symbol& s, Side side) {
  return side == Side::Buy ? s.stop_bids : s.stop_asks;
}

std::map<Price, ReferenceEngine::Level>* ReferenceEngine::book_of(Symbol& s, Side side,
                                                                  bool is_stop) {
  return is_stop ? &stops(s, side) : &liquidity(s, side);
}

std::optional<Price> ReferenceEngine::best_bid_of(const Symbol& s) const {
  if (s.bids.empty()) {
    return std::nullopt;
  }
  return s.bids.rbegin()->first;
}

std::optional<Price> ReferenceEngine::best_ask_of(const Symbol& s) const {
  if (s.asks.empty()) {
    return std::nullopt;
  }
  return s.asks.begin()->first;
}

std::vector<OrderId> ReferenceEngine::visit_order(const Symbol& s) const {
  std::vector<OrderId> out;
  // Merge the two liquidity maps into one ascending walk over price.
  auto bit = s.bids.begin();
  auto ait = s.asks.begin();
  while (bit != s.bids.end() || ait != s.asks.end()) {
    if (ait == s.asks.end() || (bit != s.bids.end() && bit->first.value <= ait->first.value)) {
      for (const OrderId id : bit->second.queue) {
        out.push_back(id);
      }
      ++bit;
    } else {
      for (const OrderId id : ait->second.queue) {
        out.push_back(id);
      }
      ++ait;
    }
  }
  for (const auto& [price, lvl] : s.stop_bids) {
    for (const OrderId id : lvl.queue) {
      out.push_back(id);
    }
  }
  for (const auto& [price, lvl] : s.stop_asks) {
    for (const OrderId id : lvl.queue) {
      out.push_back(id);
    }
  }
  return out;
}

Quantity ReferenceEngine::aggregate_at(const Symbol& s, Side side, Price price) const {
  const auto& book = liquidity(s, side);
  const auto it = book.find(price);
  return it == book.end() ? Quantity{0} : it->second.aggregate;
}

const Order& ReferenceEngine::order_at(const Symbol& s, OrderId id) const {
  return s.live.find(id)->second.order;
}

Order& ReferenceEngine::order_ref(Symbol& s, OrderId id) {
  return s.live.find(id)->second.order;
}

void ReferenceEngine::emit_delta(SymbolId symbol, Price price, Side side, Quantity aggregate,
                                 UpdateAction action) {
  Event d = new_event(symbol, EventType::BookUpdate, Timestamp{});
  d.price = price;
  d.qty = aggregate;
  d.side = side;
  d.action = action;
  events_.push_back(d);
}

void ReferenceEngine::emit_level_delta(SymbolId symbol, Side side, Price price, bool emptied) {
  // Stops are never displayed liquidity, so they produce no delta. Callers only
  // invoke this for the liquidity book.
  if (emptied) {
    emit_delta(symbol, price, side, Quantity{0}, UpdateAction::Removed);
  } else {
    emit_delta(symbol, price, side, aggregate_at(sym(symbol), side, price), UpdateAction::Changed);
  }
}

bool ReferenceEngine::unlink(Symbol& s, OrderId id, Price* out_price, bool* out_is_stop,
                             bool* out_was_queued) {
  auto it = s.live.find(id);
  if (it == s.live.end()) {
    return false;
  }
  const Entry entry = it->second;
  if (out_is_stop != nullptr) {
    *out_is_stop = entry.is_stop;
  }
  if (out_price != nullptr) {
    *out_price = entry.price;
  }
  if (out_was_queued != nullptr) {
    *out_was_queued = entry.queued;
  }
  if (!entry.queued) {
    // Live but never rested: an aggressor, or an order whose remainder is
    // being cancelled. There is no queue to unlink from and no L2 delta.
    s.live.erase(it);
    return true;
  }
  std::map<Price, Level>& book = *book_of(s, entry.side, entry.is_stop);
  const auto lit = book.find(entry.price);
  if (lit == book.end()) {
    s.live.erase(it);
    return true;
  }
  Level& lvl = lit->second;
  for (auto qit = lvl.queue.begin(); qit != lvl.queue.end(); ++qit) {
    if (*qit == id) {
      lvl.aggregate -= entry.order.leaves;
      lvl.queue.erase(qit);
      break;
    }
  }
  s.live.erase(it);
  if (lvl.queue.empty()) {
    // Erasing empty levels keeps best bid/ask a plain begin()/rbegin().
    book.erase(lit);
  }
  return true;
}

// ---------------------------------------------------------------------------
// New orders
// ---------------------------------------------------------------------------

void ReferenceEngine::submit(const NewOrderRequest& request) {
  if (request.symbol.value >= symbols_.size()) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.participant = request.participant;
    e.reject_code = RejectCode::UnknownSymbol;
    events_.push_back(e);
    return;
  }
  Symbol& s = sym(request.symbol);

  if (s.live.count(request.order_id) != 0U) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.participant = request.participant;
    e.reject_code = RejectCode::DuplicateOrderId;
    events_.push_back(e);
    return;
  }
  const RejectCode rc =
      validate_new_order(SymbolRules{&s.config, s.has_last_trade, s.last_trade}, request);
  if (rc != RejectCode::None) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.participant = request.participant;
    e.reject_code = rc;
    events_.push_back(e);
    return;
  }

  pending_.clear();
  handle_new(s, request, /*counts_as_acceptance=*/true);
  drain_pending(s);
}

void ReferenceEngine::fail_new(Symbol& s, const NewOrderRequest& r, RejectCode code,
                               bool counts_as_acceptance) {
  if (!counts_as_acceptance) {
    s.removed_qty += static_cast<std::uint64_t>(r.quantity.value);
  }
  Event e = new_event(r.symbol, EventType::Rejected, r.ts);
  e.order_id = r.order_id;
  e.participant = r.participant;
  e.reject_code = code;
  events_.push_back(e);
}

void ReferenceEngine::handle_new(Symbol& s, NewOrderRequest request, bool counts_as_acceptance) {
  const SymbolId symbol_id = request.symbol;
  const SymbolConfig& cfg = s.config;

  if (request.post_only) {
    // Ticks, not Price, throughout: Price's constructor is explicit so it will
    // not silently compare against a raw integer.
    const bool has_bid = best_bid_of(s).has_value();
    const bool has_ask = best_ask_of(s).has_value();
    const std::int64_t best_bid_tick = has_bid ? best_bid_of(s)->value : 0;
    const std::int64_t best_ask_tick = has_ask ? best_ask_of(s)->value : 0;
    bool would_cross = request.type == OrderType::Market;
    if (!would_cross) {
      would_cross = request.side == Side::Buy ? (has_ask && best_ask_tick <= request.price.value)
                                              : (has_bid && best_bid_tick >= request.price.value);
    }
    if (would_cross) {
      if (cfg.post_only_action == PostOnlyAction::Reject) {
        fail_new(s, request, RejectCode::WouldCrossPostOnly, counts_as_acceptance);
        return;
      }
      if (request.side == Side::Buy ? !has_ask : !has_bid) {
        return;
      }
      const std::int64_t slid_tick =
          request.side == Side::Buy ? best_ask_tick - cfg.tick_size : best_bid_tick + cfg.tick_size;
      if (slid_tick < cfg.min_price || slid_tick > cfg.max_price) {
        fail_new(s, request, RejectCode::PriceOutOfRange, counts_as_acceptance);
        return;
      }
      request.price = Price{slid_tick};
    }
  }

  if (request.tif == TimeInForce::FOK) {
    std::int64_t available = 0;
    if (request.side == Side::Buy) {
      for (const auto& [price, lvl] : s.asks) {
        if (request.type != OrderType::Market && !crosses(request.side, request.price, price)) {
          break;
        }
        available += lvl.aggregate.value;
      }
    } else {
      for (auto it = s.bids.rbegin(); it != s.bids.rend(); ++it) {
        if (request.type != OrderType::Market && !crosses(request.side, request.price, it->first)) {
          break;
        }
        available += it->second.aggregate.value;
      }
    }
    if (available < request.quantity.value) {
      fail_new(s, request, RejectCode::FokInsufficientLiquidity, counts_as_acceptance);
      return;
    }
  }

  Order order;
  order.id = request.order_id;
  order.participant = request.participant;
  order.side = request.side;
  order.type = request.type;
  order.tif = request.tif;
  order.price = request.price;
  order.stop_limit = request.price;
  order.trigger_price = request.trigger_price;
  order.total = request.quantity;
  order.leaves = request.quantity;
  order.filled = Quantity{0};
  order.display = request.display_qty;
  order.seq = request.seq;
  order.post_only = request.post_only;

  {
    Event e = new_event(symbol_id, EventType::Accepted, request.ts);
    e.order_id = order.id;
    e.participant = order.participant;
    e.side = order.side;
    e.price = order.price;
    e.qty = order.total;
    e.leaves_qty = order.leaves;
    e.order_type = order.type;
    e.tif = order.tif;
    e.post_only = order.post_only;
    events_.push_back(e);
  }

  if (request.type == OrderType::Stop || request.type == OrderType::StopLimit) {
    // A pending stop's price is its trigger, so an order's price always equals
    // the price of the level it is linked to.
    Entry entry;
    entry.order = order;
    entry.price = request.trigger_price;
    entry.side = order.side;
    entry.is_stop = true;
    entry.queued = true;
    entry.order.price = request.trigger_price;
    s.live[order.id] = entry;

    Level& lvl = stops(s, order.side)[request.trigger_price];
    lvl.side = order.side;
    lvl.queue.push_back(order.id);
    lvl.aggregate += order.leaves;
    return;
  }

  if (counts_as_acceptance) {
    s.accepted_qty += static_cast<std::uint64_t>(request.quantity.value);
  }

  Entry entry;
  entry.order = order;
  entry.price = order.price;
  entry.side = order.side;
  entry.is_stop = false;
  entry.queued = false;
  s.live[order.id] = entry;

  if (!match(s, request)) {
    return;
  }
  rest_remainder(s, request.order_id);
}

// ---------------------------------------------------------------------------
// Matching
// ---------------------------------------------------------------------------

bool ReferenceEngine::match(Symbol& s, const NewOrderRequest& request) {
  const SymbolId symbol_id = request.symbol;
  const Timestamp ts = ts_of(request.seq);
  const OrderId taker_id = request.order_id;

  for (;;) {
    Order taker = order_at(s, taker_id);
    if (taker.leaves.value <= 0) {
      return true;
    }
    const Side book_side = taker.side == Side::Buy ? Side::Sell : Side::Buy;
    std::map<Price, Level>& book = liquidity(s, book_side);
    if (book.empty()) {
      return true;
    }
    const Price level_price = taker.side == Side::Buy ? book.begin()->first : book.rbegin()->first;
    if (taker.type != OrderType::Market && !crosses(taker.side, taker.price, level_price)) {
      return true;
    }
    Level& lvl = book[level_price];
    if (lvl.queue.empty()) {
      return true;
    }
    const OrderId maker_id = lvl.queue.front();
    Order maker = order_at(s, maker_id);

    const StpOutcome outcome = stp_decide(s.config.stp_mode, maker.participant, taker.participant);
    if (outcome == StpOutcome::CancelMaker) {
      cancel_live(s, maker_id, CancelReason::SelfTradePrevention, ts);
      continue;
    }
    if (outcome == StpOutcome::CancelTaker) {
      retire(s, taker_id);
      return false;
    }
    if (outcome == StpOutcome::CancelBoth) {
      cancel_live(s, maker_id, CancelReason::SelfTradePrevention, ts);
      retire(s, taker_id);
      return false;
    }
    if (outcome == StpOutcome::DecrementMaker && maker.leaves.value <= 0) {
      cancel_live(s, maker_id, CancelReason::SelfTradePrevention, ts);
      continue;
    }

    const bool decrement_only = outcome == StpOutcome::DecrementMaker;
    const Quantity shown = maker.visible();
    Quantity fill = qmin(taker.leaves, shown);
    if (decrement_only) {
      fill = qmin(taker.leaves, maker.leaves);
    }
    if (fill.value <= 0) {
      return true;
    }

    const Price fill_price = maker.price;
    const Price maker_limit = maker.price;
    const ParticipantId maker_pid = maker.participant;
    const bool maker_exhausted = maker.leaves.value == fill.value;

    taker.leaves -= fill;
    taker.filled += fill;
    maker.leaves -= fill;
    maker.filled += fill;
    lvl.aggregate -= fill;
    order_ref(s, taker_id) = taker;
    order_ref(s, maker_id) = maker;

    s.trade_count += 1;
    // A fill reduces two orders' leaves, so the conservation counter advances by
    // twice the traded quantity.
    s.filled_qty += static_cast<std::uint64_t>(fill.value) * 2U;
    s.has_last_trade = true;
    s.last_trade = fill_price;

    Event e = new_event(symbol_id, EventType::Trade, ts);
    e.trade_id = TradeId{s.trade_count};
    e.order_id = taker_id;
    e.maker_order_id = maker_id;
    e.participant = taker.participant;
    e.side = taker.side;
    e.price = fill_price;
    e.qty = fill;
    e.maker_price = maker_limit;
    e.maker = maker_pid;
    events_.push_back(e);

    if (maker_exhausted) {
      // Fully filled: no Cancelled, the fill already told the client.
      Price ignored{0};
      bool is_stop{false};
      bool was_queued{false};
      (void)unlink(s, maker_id, &ignored, &is_stop, &was_queued);
      // The level is only gone if this maker was its last order. unlink() erases
      // a level once its queue empties, so a zero aggregate means Removed.
      const bool emptied = aggregate_at(s, book_side, level_price).value == 0;
      emit_level_delta(symbol_id, book_side, level_price, emptied);
    } else {
      emit_level_delta(symbol_id, book_side, level_price, /*emptied=*/false);
      if (maker.display.value > 0 && fill.value == shown.value && maker.leaves.value > 0) {
        // Displayed slice consumed: replenish and drop to the back of the queue.
        lvl.queue.pop_front();
        lvl.queue.push_back(maker_id);
      }
    }

    collect_triggers(s, fill_price);
  }
}

// ---------------------------------------------------------------------------
// Stops
// ---------------------------------------------------------------------------

void ReferenceEngine::collect_triggers(Symbol& s, Price trade_price) {
  if (!s.config.stop_enabled) {
    return;
  }
  // Buy stops fire at or above the trigger, walked upward from the lowest; sell
  // stops at or below, downward from the highest. Nearest trigger first is what
  // makes a cascade reproducible.
  //
  // The trigger prices are snapshotted before any dispatch: dispatching unlinks
  // orders and erases the emptied level, which invalidates the iterator being
  // walked. (The production engine walks level indices via an occupancy bitmap,
  // so it cannot hit this; std::map has no equivalent cheap walk.)
  std::vector<Price> buy_triggers;
  for (const auto& [price, lvl] : s.stop_bids) {
    if (price.value > trade_price.value) {
      break;
    }
    buy_triggers.push_back(price);
  }
  std::vector<Price> sell_triggers;
  for (auto it = s.stop_asks.rbegin(); it != s.stop_asks.rend(); ++it) {
    if (it->first.value < trade_price.value) {
      break;
    }
    sell_triggers.push_back(it->first);
  }

  for (const Price trigger : buy_triggers) {
    dispatch_stop_level(s, Side::Buy, trigger);
  }
  for (const Price trigger : sell_triggers) {
    dispatch_stop_level(s, Side::Sell, trigger);
  }
}

void ReferenceEngine::dispatch_stop_level(Symbol& s, Side side, Price trigger) {
  for (;;) {
    std::map<Price, Level>& book = stops(s, side);
    const auto lit = book.find(trigger);
    if (lit == book.end() || lit->second.queue.empty()) {
      return;
    }
    const OrderId id = lit->second.queue.front();
    const Order o = order_at(s, id);
    Price ignored{0};
    bool is_stop{false};
    bool was_queued{false};
    (void)unlink(s, id, &ignored, &is_stop, &was_queued);

    Event e = new_event(s.symbol_id, EventType::StopTriggered, ts_of(o.seq));
    e.order_id = o.id;
    e.participant = o.participant;
    e.side = o.side;
    e.price = o.trigger_price;
    e.qty = o.leaves;
    e.order_type = o.type;
    events_.push_back(e);

    NewOrderRequest triggered;
    triggered.seq = o.seq;
    triggered.ts = e.ts;
    triggered.symbol = s.symbol_id;
    triggered.order_id = o.id;
    triggered.participant = o.participant;
    triggered.side = o.side;
    triggered.type = o.type == OrderType::StopLimit ? OrderType::Limit : OrderType::Market;
    triggered.tif = o.tif;
    triggered.price = o.type == OrderType::StopLimit ? o.stop_limit : Price{0};
    triggered.trigger_price = o.trigger_price;
    triggered.quantity = o.leaves;
    triggered.display_qty = o.display;
    pending_.push_back(triggered);
  }
}

void ReferenceEngine::drain_pending(Symbol& s) {
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    handle_new(s, pending_[i], /*counts_as_acceptance=*/false);
  }
  pending_.clear();
}

// ---------------------------------------------------------------------------
// Resting and removal
// ---------------------------------------------------------------------------

void ReferenceEngine::rest_remainder(Symbol& s, OrderId id) {
  const Order o = order_at(s, id);
  if (o.leaves.value <= 0) {
    retire(s, id);
    return;
  }
  if (o.type == OrderType::Market || o.tif == TimeInForce::IOC || o.tif == TimeInForce::FOK) {
    cancel_live(s, id,
                o.tif == TimeInForce::FOK ? CancelReason::SelfTradePrevention
                                          : CancelReason::ImmediateOrCancel,
                ts_of(o.seq));
    return;
  }
  Entry& entry = s.live[id];
  entry.price = o.price;
  entry.queued = true;
  Level& lvl = liquidity(s, o.side)[o.price];
  const bool was_empty = lvl.queue.empty();
  lvl.side = o.side;
  lvl.queue.push_back(id);
  lvl.aggregate += o.leaves;
  emit_delta(s.symbol_id, o.price, o.side, lvl.aggregate,
             was_empty ? UpdateAction::Added : UpdateAction::Changed);
}

void ReferenceEngine::cancel_live(Symbol& s, OrderId id, CancelReason reason, Timestamp ts) {
  const Order o = order_at(s, id);
  Price price{0};
  bool is_stop{false};
  bool was_queued{false};
  if (!unlink(s, id, &price, &is_stop, &was_queued)) {
    return;
  }
  s.removed_qty += static_cast<std::uint64_t>(o.leaves.value);
  // A delta only for an order that was actually linked into a liquidity level.
  // An unqueued aggressor being cancelled never appeared in the book, so
  // reporting its level as removed would be a phantom update.
  if (was_queued && !is_stop) {
    emit_level_delta(s.symbol_id, o.side, price, aggregate_at(s, o.side, price).value == 0);
  }

  Event e = new_event(s.symbol_id, EventType::Cancelled, ts);
  e.order_id = o.id;
  e.participant = o.participant;
  e.side = o.side;
  e.price = o.price;
  e.qty = Quantity{0};
  e.leaves_qty = o.leaves;
  e.reason = reason;
  events_.push_back(e);
}

void ReferenceEngine::retire(Symbol& s, OrderId id) {
  const Order o = order_at(s, id);
  s.removed_qty += static_cast<std::uint64_t>(o.leaves.value);
  Price ignored{0};
  bool is_stop{false};
  bool was_queued{false};
  (void)unlink(s, id, &ignored, &is_stop, &was_queued);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

void ReferenceEngine::submit(const CancelRequest& request) {
  if (request.symbol.value >= symbols_.size()) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::UnknownSymbol;
    events_.push_back(e);
    return;
  }
  Symbol& s = sym(request.symbol);
  const auto it = s.live.find(request.order_id);
  if (it == s.live.end() || it->second.order.participant.value != request.participant.value) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::UnknownOrder;
    events_.push_back(e);
    return;
  }
  cancel_live(s, request.order_id, CancelReason::ClientRequest, request.ts);
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------

void ReferenceEngine::replace_stop(Symbol& s, const ReplaceRequest& request) {
  const Timestamp ts = request.ts;
  Order o = order_at(s, request.order_id);

  if (request.new_quantity.value < o.filled.value) {
    Event e = new_event(request.symbol, EventType::Rejected, ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceWouldReduceBelowFilled;
    events_.push_back(e);
    return;
  }
  if (request.new_quantity.value == o.total.value) {
    Event e = new_event(request.symbol, EventType::Rejected, ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceNoPriceChange;
    events_.push_back(e);
    return;
  }
  // new_quantity is the order's new *total*. Anything at or below the already
  // filled quantity leaves zero remaining, which is a cancel, not a replace.
  // Getting this wrong rests a zero-quantity order.
  if (request.new_quantity.value <= o.filled.value) {
    cancel_live(s, request.order_id, CancelReason::ClientRequest, ts);
    return;
  }
  // A pending stop's price is its trigger, so a price change relocates it in the
  // stop book. It must not end up immediately executable against the resting
  // book. Checked in the same position as the engine so the two agree on which
  // rejection wins.
  if (request.new_price.value != 0 && request.new_price.value != o.price.value &&
      (o.side == Side::Buy
           ? (!s.asks.empty() && request.new_price.value >= s.asks.begin()->first.value)
           : (!s.bids.empty() && request.new_price.value <= s.bids.rbegin()->first.value))) {
    Event e = new_event(request.symbol, EventType::Rejected, ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceWouldCross;
    events_.push_back(e);
    return;
  }
  // A pending stop's price is its trigger, so there is no price to change.
  if (request.new_price.value != 0 && request.new_price.value != o.price.value) {
    Event e = new_event(request.symbol, EventType::Rejected, ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceNoPriceChange;
    events_.push_back(e);
    return;
  }

  std::map<Price, Level>& book = stops(s, o.side);
  const Price price = o.price;
  const std::int64_t old_leaves = o.leaves.value;
  const bool increased = request.new_quantity.value > o.total.value;
  const std::int64_t qty_delta = request.new_quantity.value - o.total.value;
  if (qty_delta < 0) {
    s.removed_qty += static_cast<std::uint64_t>(-qty_delta);
  } else if (qty_delta > 0) {
    s.accepted_qty += static_cast<std::uint64_t>(qty_delta);
  }

  if (increased) {
    // Growing a stop loses priority, exactly as for a resting order.
    Price ignored{0};
    bool is_stop{false};
    bool was_queued{false};
    (void)unlink(s, request.order_id, &ignored, &is_stop, &was_queued);
    o.total = request.new_quantity;
    o.leaves = Quantity{request.new_quantity.value - o.filled.value};
    // unlink() erased the Entry, so it must be rebuilt in full. Assigning only
    // `.order` would leave a default-constructed Entry whose `side` is Buy
    // regardless of the order's real side, and the next removal would look in
    // the wrong book.
    Entry rebuilt;
    rebuilt.order = o;
    rebuilt.price = price;
    rebuilt.side = o.side;
    rebuilt.is_stop = true;
    rebuilt.queued = true;
    s.live[request.order_id] = rebuilt;
    Level& lvl = book[price];
    lvl.side = o.side;
    lvl.queue.push_back(request.order_id);
    lvl.aggregate += o.leaves;
  } else {
    o.total = request.new_quantity;
    o.leaves = Quantity{request.new_quantity.value - o.filled.value};
    s.live[request.order_id].order = o;
    book[price].aggregate -= Quantity{old_leaves - o.leaves.value};
  }

  Event e = new_event(request.symbol, EventType::Replaced, ts);
  e.order_id = o.id;
  e.participant = o.participant;
  e.side = o.side;
  e.price = o.price;
  e.qty = o.total;
  e.leaves_qty = o.leaves;
  events_.push_back(e);
}

void ReferenceEngine::submit(const ReplaceRequest& request) {
  if (request.symbol.value >= symbols_.size()) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::UnknownSymbol;
    events_.push_back(e);
    return;
  }
  Symbol& s = sym(request.symbol);
  const auto it = s.live.find(request.order_id);
  if (it == s.live.end() || it->second.order.participant.value != request.participant.value) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::UnknownOrder;
    events_.push_back(e);
    return;
  }

  Order o = it->second.order;
  if (o.type == OrderType::Stop || o.type == OrderType::StopLimit) {
    replace_stop(s, request);
    return;
  }

  if (request.new_quantity.value < o.filled.value) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceWouldReduceBelowFilled;
    events_.push_back(e);
    return;
  }
  const Price target = request.new_price.value != 0 ? request.new_price : o.price;
  if (target.value == o.price.value && request.new_quantity.value == o.total.value) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceNoPriceChange;
    events_.push_back(e);
    return;
  }
  // new_quantity is the order's new *total*. Anything at or below the already
  // filled quantity leaves zero remaining, which is a cancel, not a replace.
  // Getting this wrong rests a zero-quantity order.
  if (request.new_quantity.value <= o.filled.value) {
    cancel_live(s, request.order_id, CancelReason::ClientRequest, request.ts);
    return;
  }
  const bool price_changed = target.value != o.price.value;
  const bool qty_increased = request.new_quantity.value > o.total.value;
  if (price_changed && (target.value < s.config.min_price || target.value > s.config.max_price)) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::PriceOutOfRange;
    events_.push_back(e);
    return;
  }
  // A replace never re-runs matching, so a price change that crosses the opposite
  // touch would rest the order on the far side of the book. Rejected rather than
  // matched, so the book cannot be left crossed. Mirrors the engine's ordering.
  if (price_changed &&
      (o.side == Side::Buy ? (!s.asks.empty() && target.value >= s.asks.begin()->first.value)
                           : (!s.bids.empty() && target.value <= s.bids.rbegin()->first.value))) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceWouldCross;
    events_.push_back(e);
    return;
  }

  const Price old_price = o.price;
  const std::int64_t old_leaves = o.leaves.value;
  const bool keeps_priority = !price_changed && !qty_increased;
  std::map<Price, Level>& book = liquidity(s, o.side);

  if (!keeps_priority) {
    Price ignored{0};
    bool is_stop{false};
    bool was_queued{false};
    (void)unlink(s, request.order_id, &ignored, &is_stop, &was_queued);
  }

  const std::int64_t qty_delta = request.new_quantity.value - o.total.value;
  if (qty_delta < 0) {
    s.removed_qty += static_cast<std::uint64_t>(-qty_delta);
  } else if (qty_delta > 0) {
    s.accepted_qty += static_cast<std::uint64_t>(qty_delta);
  }

  o.price = target;
  o.total = request.new_quantity;
  o.leaves = Quantity{request.new_quantity.value - o.filled.value};

  if (keeps_priority) {
    book[old_price].aggregate -= Quantity{old_leaves - o.leaves.value};
  } else {
    Level& lvl = book[target];
    lvl.side = o.side;
    lvl.queue.push_back(request.order_id);
    lvl.aggregate += o.leaves;
  }
  // unlink() erased the Entry when priority was lost, so rebuild it in full --
  // see the note in replace_stop.
  Entry rebuilt;
  rebuilt.order = o;
  rebuilt.price = target;
  rebuilt.side = o.side;
  rebuilt.is_stop = false;
  rebuilt.queued = true;
  s.live[request.order_id] = rebuilt;

  Event e = new_event(request.symbol, EventType::Replaced, request.ts);
  e.order_id = o.id;
  e.participant = o.participant;
  e.side = o.side;
  e.price = o.price;
  e.qty = o.total;
  e.leaves_qty = o.leaves;
  events_.push_back(e);

  if (price_changed) {
    emit_level_delta(request.symbol, o.side, old_price,
                     aggregate_at(s, o.side, old_price).value == 0);
    emit_delta(request.symbol, target, o.side, aggregate_at(s, o.side, target),
               UpdateAction::Changed);
  } else if (keeps_priority) {
    emit_level_delta(request.symbol, o.side, o.price, false);
  }
}

// ---------------------------------------------------------------------------
// Mass cancel and session end
// ---------------------------------------------------------------------------

void ReferenceEngine::submit(const MassCancelRequest& request) {
  for (Symbol& s : symbols_) {
    // One ordered visit, so the sequence of Cancelled events matches the engine.
    for (const OrderId id : visit_order(s)) {
      if (order_at(s, id).participant.value == request.participant.value) {
        cancel_live(s, id, CancelReason::ClientRequest, request.ts);
      }
    }
  }
}

void ReferenceEngine::on_session_end(Timestamp now) {
  for (Symbol& s : symbols_) {
    // Day orders expire first, then unfilled stops, each in the engine's visit
    // order. Victims are collected before removal because cancelling mutates the
    // very queue being walked.
    std::vector<OrderId> day_orders;
    std::vector<OrderId> stop_orders;
    for (const OrderId id : visit_order(s)) {
      if (s.live.find(id)->second.is_stop) {
        stop_orders.push_back(id);
      } else if (order_at(s, id).tif == TimeInForce::Day) {
        day_orders.push_back(id);
      }
    }
    for (const OrderId id : day_orders) {
      cancel_live(s, id, CancelReason::TimeInForce, now);
    }
    for (const OrderId id : stop_orders) {
      const Order o = order_at(s, id);
      Price ignored{0};
      bool is_stop{false};
      bool was_queued{false};
      (void)unlink(s, id, &ignored, &is_stop, &was_queued);
      s.removed_qty += static_cast<std::uint64_t>(o.leaves.value);
      Event e = new_event(s.symbol_id, EventType::Cancelled, now);
      e.order_id = o.id;
      e.participant = o.participant;
      e.side = o.side;
      e.price = o.trigger_price;
      e.qty = o.leaves;
      e.reason = CancelReason::StopCancelled;
      events_.push_back(e);
    }
  }
}

// ---------------------------------------------------------------------------

TopOfBook ReferenceEngine::book_top(SymbolId symbol) const {
  TopOfBook out;
  const Symbol& s = sym(symbol);
  if (!s.bids.empty()) {
    out.has_bid = true;
    out.best_bid = s.bids.rbegin()->first;
    out.best_bid_qty = s.bids.rbegin()->second.aggregate;
  }
  if (!s.asks.empty()) {
    out.has_ask = true;
    out.best_ask = s.asks.begin()->first;
    out.best_ask_qty = s.asks.begin()->second.aggregate;
  }
  return out;
}

std::vector<std::string> ReferenceEngine::book_digest() const {
  std::vector<std::string> out;
  for (const Symbol& s : symbols_) {
    for (int side_i = 0; side_i < 2; ++side_i) {
      const Side side = side_i == 0 ? Side::Buy : Side::Sell;
      for (const auto& [price, lvl] : liquidity(s, side)) {
        for (const OrderId id : lvl.queue) {
          const Order& o = order_at(s, id);
          out.push_back(std::to_string(s.symbol_id.value) + " " +
                        std::string(side == Side::Buy ? "buy" : "sell") + " " +
                        std::to_string(price.value) + " " + std::to_string(o.id.value) + " " +
                        std::to_string(o.total.value) + " " + std::to_string(o.filled.value) + " " +
                        std::to_string(o.leaves.value) + " " +
                        std::to_string(static_cast<int>(o.type)) + " " +
                        std::to_string(static_cast<int>(o.tif)));
        }
      }
    }
  }
  return out;
}

}  // namespace lob::reference
