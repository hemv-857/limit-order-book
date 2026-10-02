#include "core/engine.hpp"

#include "core/stp.hpp"
#include "core/validate.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <string>
#include <utility>

namespace lob {
namespace {

/// True when an order at `order_price` is aggressive enough to trade against
/// liquidity resting at `level_price`.
[[nodiscard]] constexpr bool crosses(Side side, Price order_price, Price level_price) noexcept {
  return side == Side::Buy ? level_price <= order_price : level_price >= order_price;
}

[[nodiscard]] constexpr Quantity qmin(Quantity a, Quantity b) noexcept {
  return a.value <= b.value ? a : b;
}

/// A stop order's arrival sequence doubles as its timestamp when it fires.
/// Narrowed explicitly: Sequence is unsigned and Timestamp is signed.
[[nodiscard]] constexpr Timestamp ts_of(Sequence seq) noexcept {
  return Timestamp{static_cast<std::int64_t>(seq.value)};
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

namespace {
/// Configuration for the out-of-range fallback state: the smallest book that is
/// still valid. Called from the Engine constructor, which is allowed to throw.
SymbolConfig invalid_symbol_config() {
  SymbolConfig cfg;
  cfg.name = "INVALID";
  cfg.min_price = 0;
  cfg.max_price = 0;
  cfg.max_open_orders = 1;
  return cfg;
}
}  // namespace

Engine::Engine(std::vector<SymbolConfig> configs, EngineConfig engine_config)
    : invalid_(invalid_symbol_config(), 1), engine_config_(engine_config) {
  states_.reserve(configs.size());
  std::uint32_t largest_orders = 1;
  for (std::uint32_t i = 0; i < configs.size(); ++i) {
    const std::uint32_t capacity = std::max(configs[i].max_open_orders, 1U);
    largest_orders = std::max(largest_orders, capacity);
    states_.push_back(std::make_unique<SymbolState>(configs[i], capacity));
    states_.back()->symbol_id = SymbolId{i};
  }
  events_.reset((static_cast<std::size_t>(largest_orders) * 4U) + 64U);
  risk_ = RiskManager(configs);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

const Book& Engine::book(SymbolId symbol) const noexcept {
  const SymbolState* st = state(symbol);
  return st != nullptr ? st->book : invalid_.book;
}

const Book& Engine::stop_buy_book(SymbolId symbol) const noexcept {
  const SymbolState* st = state(symbol);
  return st != nullptr ? st->stops_buy : invalid_.stops_buy;
}

const Book& Engine::stop_sell_book(SymbolId symbol) const noexcept {
  const SymbolState* st = state(symbol);
  return st != nullptr ? st->stops_sell : invalid_.stops_sell;
}

Book& Engine::mutable_book(SymbolId symbol) noexcept {
  return states_[symbol.value]->book;
}

const SymbolState* Engine::symbol(SymbolId symbol) const noexcept {
  return state(symbol);
}

const SymbolConfig* Engine::config(SymbolId symbol) const noexcept {
  const SymbolState* st = state(symbol);
  return st != nullptr ? &st->config : nullptr;
}

std::optional<Price> Engine::last_trade_price(SymbolId symbol) const noexcept {
  const SymbolState* st = state(symbol);
  if (st == nullptr || !st->has_last_trade) {
    return std::nullopt;
  }
  return st->last_trade_price;
}

// Quantity bought by *incoming buy* orders. A single sweep can absorb far more
// resting volume than was ever sold aggressively, so this is deliberately not
// equal to aggressor_sell_volume; symmetry is asserted by the conservation law
// in check_invariants instead.
std::uint64_t Engine::aggressor_buy_volume(SymbolId symbol) const noexcept {
  const SymbolState* st = state(symbol);
  return st != nullptr ? st->buy_volume : 0;
}

std::uint64_t Engine::aggressor_sell_volume(SymbolId symbol) const noexcept {
  const SymbolState* st = state(symbol);
  return st != nullptr ? st->sell_volume : 0;
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

Event Engine::new_event(SymbolId symbol, EventType type, Timestamp ts) noexcept {
  Event e;
  e.seq = Sequence{next_seq_++};
  e.ts = ts;
  e.symbol = symbol;
  e.type = type;
  return e;
}

void Engine::emit_delta(SymbolState& st, LevelIndex level, UpdateAction action) noexcept {
  const Level& lv = st.book.level_at(level);
  Event e = new_event(st.symbol_id, EventType::BookUpdate, Timestamp{});
  e.price = st.book.price_of(level);
  e.qty = lv.aggregate_qty;
  e.side = lv.side;
  e.action = action;
  events_.push_back(e);
}

// ---------------------------------------------------------------------------
// New orders
// ---------------------------------------------------------------------------

void Engine::reject(SymbolState& st, const NewOrderRequest& r, RejectCode code) noexcept {
  Event e = new_event(st.symbol_id, EventType::Rejected, r.ts);
  e.order_id = r.order_id;
  e.participant = r.participant;
  e.reject_code = code;
  events_.push_back(e);
}

void Engine::submit(const NewOrderRequest& request) noexcept {
  SymbolState* st = state(request.symbol);
  if (st == nullptr) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.participant = request.participant;
    e.reject_code = RejectCode::UnknownSymbol;
    events_.push_back(e);
    return;
  }

  // Fixed, documented check order (docs/MATCHING_RULES.md). The first two are
  // identity checks the engine owns; the rest live in validate_new_order.
  if (st->index.contains(request.order_id)) {
    reject(*st, request, RejectCode::DuplicateOrderId);
    return;
  }
  const RejectCode rc = validate_new_order(
      SymbolRules{&st->config, st->has_last_trade, st->last_trade_price}, request);
  if (rc != RejectCode::None) {
    reject(*st, request, rc);
    return;
  }
  if (draining_) {
    reject(*st, request, RejectCode::ShuttingDown);
    return;
  }
  if (!risk_.consume_rate_limit(request.participant, request.ts)) {
    reject(*st, request, RejectCode::RateLimitExceeded);
    return;
  }

  st->trigger_count = 0;
  handle_new_order(*st, request, /*counts_as_acceptance=*/true);
  drain_trigger_queue(*st);
}

void Engine::handle_new_order(SymbolState& st, NewOrderRequest request,
                              bool counts_as_acceptance) noexcept {
  const SymbolConfig& cfg = st.config;

  // ---- post-only: reject or slide before anything can match ----
  if (request.post_only) {
    const TopOfBook tob = st.book.top_of_book();
    bool would_cross = request.type == OrderType::Market;
    if (!would_cross) {
      would_cross = request.side == Side::Buy
                        ? (tob.has_ask && tob.best_ask.value <= request.price.value)
                        : (tob.has_bid && tob.best_bid.value >= request.price.value);
    }
    if (would_cross) {
      if (cfg.post_only_action == PostOnlyAction::Reject) {
        fail_new(st, request, RejectCode::WouldCrossPostOnly, counts_as_acceptance);
        return;
      }
      if (request.side == Side::Buy ? !tob.has_ask : !tob.has_bid) {
        return;  // nothing to slide behind and nothing to match
      }
      // Slide one tick to the passive side of the touch: a buy must sit *below*
      // the best ask and a sell *above* the best bid. Getting the direction
      // backwards would leave the order still crossing, which is the opposite of
      // what post-only is for.
      const Price slid = request.side == Side::Buy ? Price{tob.best_ask.value - cfg.tick_size}
                                                   : Price{tob.best_bid.value + cfg.tick_size};
      if (!st.book.in_domain(slid)) {
        fail_new(st, request, RejectCode::PriceOutOfRange, counts_as_acceptance);
        return;
      }
      request.price = slid;
    }
  }

  // ---- FOK: prove the liquidity exists before emitting any fill ----
  if (request.tif == TimeInForce::FOK) {
    std::int64_t available = 0;
    for (LevelIndex lvl = request.side == Side::Buy ? st.book.best_ask_level()
                                                    : st.book.best_bid_level();
         lvl != kNullLevel; lvl = request.side == Side::Buy ? st.book.next_occupied_index(lvl)
                                                            : st.book.prev_occupied_index(lvl)) {
      const Price lp = st.book.price_of(lvl);
      if (request.type != OrderType::Market && !crosses(request.side, request.price, lp)) {
        break;
      }
      available += st.book.level_at(lvl).aggregate_qty.value;
    }
    if (available < request.quantity.value) {
      fail_new(st, request, RejectCode::FokInsufficientLiquidity, counts_as_acceptance);
      return;
    }
  }

  const OrderIndex order_idx = st.arena.allocate();
  if (order_idx == kNullOrder) {
    fail_new(st, request, RejectCode::BookFull, counts_as_acceptance);
    return;
  }
  {
    Order& o = st.arena[order_idx];
    o.order_id = request.order_id;
    o.participant = request.participant;
    o.side = request.side;
    o.type = request.type;
    o.tif = request.tif;
    o.price = request.price;
    o.trigger_price = request.trigger_price;
    o.total_qty = request.quantity;
    o.leaves_qty = request.quantity;
    o.filled_qty = Quantity{0};
    o.display_qty = request.display_qty;
    o.arrival_seq = request.seq;
    o.flags =
        static_cast<std::uint8_t>((request.post_only ? flag::kPostOnly : flag::kNone) |
                                  (request.display_qty.value > 0 ? flag::kDisplayed : flag::kNone));
  }
  if (!st.index.insert(request.order_id, order_idx, st.arena[order_idx].generation)) {
    st.arena.release(order_idx);
    fail_new(st, request, RejectCode::BookFull, counts_as_acceptance);
    return;
  }

  if (counts_as_acceptance) {
    st.accepted_qty += static_cast<std::uint64_t>(request.quantity.value);
  }

  // ---- Accepted, before any fill, so a client sees its order come alive ----
  {
    Event e = new_event(st.symbol_id, EventType::Accepted, request.ts);
    e.order_id = request.order_id;
    e.participant = request.participant;
    e.side = request.side;
    e.price = request.price;
    e.qty = request.quantity;
    e.leaves_qty = request.quantity;
    e.order_type = request.type;
    e.tif = request.tif;
    e.post_only = request.post_only;
    events_.push_back(e);
  }

  if (is_stop_type(request.type)) {
    // Stop orders rest outside the book until triggered, so they occupy no
    // liquidity and cannot accidentally self-trigger against their own trigger.
    Order& o = st.arena[order_idx];
    // While pending, an order's price is its trigger, so that an order's price
    // always equals the price of the level it is linked to. The limit a
    // StopLimit will actually work at is held separately.
    o.stop_limit_price = request.price;
    o.price = request.trigger_price;
    Book& stop_book = request.side == Side::Buy ? st.stops_buy : st.stops_sell;
    o.level = stop_book.index_of(request.trigger_price);
    if (!stop_book.add_to_queue(order_idx)) {
      st.index.erase(request.order_id);
      st.arena.release(order_idx);
      fail_new(st, request, RejectCode::BookFull, counts_as_acceptance);
    }
    return;
  }

  if (!match(st, order_idx, request.ts)) {
    return;  // self-trade prevention already retired the aggressor
  }
  rest_remainder(st, order_idx);
}

bool Engine::match(SymbolState& st, OrderIndex aggressor_idx, Timestamp ts) noexcept {
  while (true) {
    Order& taker = st.arena[aggressor_idx];
    if (taker.leaves_qty.value <= 0) {
      return true;
    }
    const LevelIndex lvl =
        taker.side == Side::Buy ? st.book.best_ask_level() : st.book.best_bid_level();
    if (lvl == kNullLevel) {
      return true;
    }
    const Price level_price = st.book.price_of(lvl);
    if (taker.type != OrderType::Market && !crosses(taker.side, taker.price, level_price)) {
      return true;
    }
    const OrderIndex maker_idx =
        taker.side == Side::Buy ? st.book.best_ask_order() : st.book.best_bid_order();
    if (maker_idx == kNullOrder) {
      return true;
    }

    // ---- self-trade prevention, evaluated before every fill ----
    const Order& maker_ref = st.arena[maker_idx];
    const StpOutcome outcome =
        stp_decide(st.config.stp_mode, maker_ref.participant, taker.participant);
    if (outcome == StpOutcome::CancelMaker) {
      remove_order(st, maker_idx, CancelReason::SelfTradePrevention, ts);
      continue;
    }
    if (outcome == StpOutcome::CancelTaker) {
      retire_order(st, aggressor_idx);
      return false;
    }
    if (outcome == StpOutcome::CancelBoth) {
      remove_order(st, maker_idx, CancelReason::SelfTradePrevention, ts);
      retire_order(st, aggressor_idx);
      return false;
    }
    if (outcome == StpOutcome::DecrementMaker && maker_ref.leaves_qty.value <= 0) {
      // Cannot decrement a maker that is already empty; treat as cancel-maker so
      // the matching loop still makes progress instead of spinning.
      remove_order(st, maker_idx, CancelReason::SelfTradePrevention, ts);
      continue;
    }
    execute_fill(st, aggressor_idx, maker_idx, ts);
  }
}

void Engine::execute_fill(SymbolState& st, OrderIndex aggressor_idx, OrderIndex maker_idx,
                          Timestamp ts) noexcept {
  Order& taker = st.arena[aggressor_idx];
  Order& maker = st.arena[maker_idx];

  const bool decrement_only = stp_decide(st.config.stp_mode, maker.participant,
                                         taker.participant) == StpOutcome::DecrementMaker;

  // A fill is limited by the taker's remainder and by what the maker is
  // *currently showing*: for an iceberg that is one displayed slice, so its
  // reserve can never be reached without first losing priority.
  const Quantity shown = maker.visible_qty();
  Quantity fill = qmin(taker.leaves_qty, shown);
  if (decrement_only) {
    fill = qmin(taker.leaves_qty, maker.leaves_qty);
  }
  if (fill.value <= 0) {
    return;
  }

  const bool maker_exhausted = maker.leaves_qty.value == fill.value;
  const LevelIndex maker_lvl = maker.level;
  if (maker_exhausted) {
    // Unlink before the quantities change: remove_from_queue subtracts what the
    // order still contributes, which must be the pre-fill amount.
    st.book.remove_from_queue(maker_idx);
  } else {
    // Partial fill: the order stays in the queue, so the aggregate has to be
    // adjusted explicitly by the filled quantity.
    st.book.reduce_level(maker_lvl, fill);
  }

  const Price fill_price = maker.price;
  const OrderId taker_id = taker.order_id;
  const ParticipantId taker_pid = taker.participant;
  const Side taker_side = taker.side;
  const ParticipantId maker_pid = maker.participant;
  const OrderId maker_order_id = maker.order_id;
  const Price maker_limit = maker.price;
  const LevelIndex maker_level = maker_lvl;

  maker.leaves_qty -= fill;
  maker.filled_qty += fill;
  // The aggressor's filled quantity has to move too. It is the field that lets
  // a later replace know how much may not be taken away, and leaving it at zero
  // breaks filled + leaves == total for any order that partially fills and then
  // rests.
  taker.leaves_qty -= fill;
  taker.filled_qty += fill;

  st.trade_count += 1;
  // A fill removes the quantity from *two* orders -- the aggressor's and the
  // maker's -- so the conservation counter advances by twice the traded amount.
  // Counting it once would make every trade look like quantity created from
  // nowhere.
  st.filled_qty += static_cast<std::uint64_t>(fill.value) * 2U;
  const auto fill_volume = static_cast<std::uint64_t>(fill.value);
  if (taker_side == Side::Buy) {
    st.buy_volume += fill_volume;
  } else {
    st.sell_volume += fill_volume;
  }
  st.has_last_trade = true;
  st.last_trade_price = fill_price;

  Event e = new_event(st.symbol_id, EventType::Trade, ts);
  e.trade_id = TradeId{st.trade_count};
  e.order_id = taker_id;
  e.participant = taker_pid;
  e.side = taker_side;
  e.price = fill_price;
  e.qty = fill;
  e.maker_price = maker_limit;
  e.maker = maker_pid;
  e.maker_order_id = maker_order_id;
  events_.push_back(e);

  if (maker_exhausted) {
    // Fully filled: already unlinked above, so just free the slot. No Cancelled
    // event -- the fills already told the client everything.
    st.index.erase(maker.order_id);
    st.arena.release(maker_idx);
    // The level is only gone if this was its last order. Hardcoding Removed
    // would report a level as deleted while still quoting its remaining
    // aggregate, which corrupts a subscriber's L2 book.
    emit_delta(
        st, maker_level,
        st.book.level_at(maker_level).empty() ? UpdateAction::Removed : UpdateAction::Changed);
  } else {
    emit_delta(st, maker_level, UpdateAction::Changed);
    if (maker.is_iceberg() && fill.value == shown.value && maker.leaves_qty.value > 0) {
      // The whole displayed slice was consumed. Replenish and drop to the back
      // of the level: an iceberg that has been seen through loses its place in
      // the queue, which is the entire point of the order type.
      //
      // The test is on the fill against the pre-fill displayed size, not on the
      // visible size before and after: replenishment restores the visible size
      // immediately, so comparing the two would never detect the exhaustion.
      st.book.remove_from_queue(maker_idx);
      st.arena[maker_idx].level = maker_level;
      (void)st.book.add_to_queue(maker_idx);
    }
  }

  collect_triggers(st, fill_price);
}

// ---------------------------------------------------------------------------
// Stops
// ---------------------------------------------------------------------------

void Engine::collect_triggers(SymbolState& st, Price trade_price) noexcept {
  if (!st.config.stop_enabled) {
    return;
  }
  // A stop order sits in the stop book at its trigger price, on the side it
  // would trade on, so a buy stop occupies a bid level and a sell stop an ask
  // level. A level therefore holds only one kind of stop.
  //
  // Buy stops fire when the trade price reaches or exceeds the trigger: walk
  // trigger prices upward from the lowest, which also makes a cascade fire its
  // nearest trigger first and so stay reproducible.
  for (LevelIndex lvl = st.stops_buy.lowest_occupied(); lvl != kNullLevel;
       lvl = st.stops_buy.next_occupied_index(lvl)) {
    if (st.stops_buy.price_of(lvl).value > trade_price.value) {
      break;
    }
    dispatch_stop_level(st, Side::Buy, lvl);
  }
  // Sell stops fire when the trade price reaches or falls below the trigger:
  // walk downward from the highest.
  for (LevelIndex lvl = st.stops_sell.highest_occupied(); lvl != kNullLevel;
       lvl = st.stops_sell.prev_occupied_index(lvl)) {
    if (st.stops_sell.price_of(lvl).value < trade_price.value) {
      break;
    }
    dispatch_stop_level(st, Side::Sell, lvl);
  }
}

void Engine::dispatch_stop_level(SymbolState& st, Side side, LevelIndex lvl) noexcept {
  Book& book = side == Side::Buy ? st.stops_buy : st.stops_sell;
  // Written as an explicit condition rather than `while (const OrderIndex i = head)`:
  // an index in a declaration is not a bool, and the implicit conversion is
  // exactly the kind of thing that silently inverts.
  while (book.level_at(lvl).head != kNullOrder) {
    const OrderIndex idx = book.level_at(lvl).head;
    // Copy before releasing: the slot belongs to the arena free list after this.
    const Order o = st.arena[idx];
    book.remove_from_queue(idx);
    st.index.erase(o.order_id);
    st.arena.release(idx);

    Event e = new_event(st.symbol_id, EventType::StopTriggered, ts_of(o.arrival_seq));
    e.order_id = o.order_id;
    e.participant = o.participant;
    e.side = o.side;
    e.price = o.trigger_price;
    e.qty = o.leaves_qty;
    e.order_type = o.type;
    events_.push_back(e);

    // A triggered stop becomes an ordinary order: Stop works at the market,
    // StopLimit rests at its own limit price.
    NewOrderRequest triggered;
    triggered.seq = o.arrival_seq;
    triggered.ts = e.ts;
    triggered.symbol = st.symbol_id;
    triggered.order_id = o.order_id;
    triggered.participant = o.participant;
    triggered.side = o.side;
    triggered.type = o.type == OrderType::StopLimit ? OrderType::Limit : OrderType::Market;
    triggered.tif = o.tif;
    triggered.price = o.type == OrderType::StopLimit ? o.stop_limit_price : Price{0};
    triggered.trigger_price = o.trigger_price;
    triggered.quantity = o.leaves_qty;
    triggered.display_qty = o.display_qty;
    assert(st.trigger_count < st.trigger_queue.size() && "trigger queue overflow");
    if (st.trigger_count < st.trigger_queue.size()) {
      st.trigger_queue[st.trigger_count++] = triggered;
    }
  }
}

void Engine::drain_trigger_queue(SymbolState& st) noexcept {
  trigger_cursor = 0;
  // Working a triggered order can produce more trades, which can trigger more
  // stops. Draining a queue instead of recursing bounds the stack at one frame
  // regardless of how long the cascade runs.
  while (trigger_cursor < st.trigger_count) {
    const NewOrderRequest next = st.trigger_queue[trigger_cursor];
    ++trigger_cursor;
    handle_new_order(st, next, /*counts_as_acceptance=*/false);
  }
}

// ---------------------------------------------------------------------------
// Resting and removal
// ---------------------------------------------------------------------------

void Engine::rest_remainder(SymbolState& st, OrderIndex order_idx) noexcept {
  Order& o = st.arena[order_idx];
  if (o.leaves_qty.value <= 0) {
    // Fully filled: retire it exactly as a fully filled maker is retired. The
    // order is no longer live, so it must leave the id index too -- leaving it
    // registered would make the index disagree with the book, and would let a
    // client cancel an order that no longer exists.
    retire_order(st, order_idx);
    return;
  }
  if (o.type == OrderType::Market || o.tif == TimeInForce::IOC || o.tif == TimeInForce::FOK) {
    // Market, IOC and FOK never rest. FOK landing here means self-trade
    // prevention broke its all-or-nothing guarantee; STP is documented to win
    // over FOK atomicity because it is a hard compliance gate.
    remove_order(st, order_idx,
                 o.tif == TimeInForce::FOK ? CancelReason::SelfTradePrevention
                                           : CancelReason::ImmediateOrCancel,
                 ts_of(o.arrival_seq));
    return;
  }
  o.level = st.book.index_of(o.price);
  // Whether this is a new level or an existing one decides Added vs Changed.
  // Every other path in the engine already reports it that way; hardcoding Added
  // would tell a market data consumer a level had appeared when it was already
  // there, which would make a consumer's L2 book wrong.
  const bool was_empty = st.book.level_at(o.level).empty();
  if (!st.book.add_to_queue(order_idx)) {
    remove_order(st, order_idx, CancelReason::ClientRequest, ts_of(o.arrival_seq));
    return;
  }
  emit_delta(st, o.level, was_empty ? UpdateAction::Added : UpdateAction::Changed);
}

Book& Engine::book_of(SymbolState& st, const Order& order) noexcept {
  if (is_stop_type(order.type)) {
    return order.side == Side::Buy ? st.stops_buy : st.stops_sell;
  }
  return st.book;
}

void Engine::remove_order(SymbolState& st, OrderIndex idx, CancelReason reason,
                          Timestamp ts) noexcept {
  if (idx == kNullOrder) {
    return;
  }
  const Order o = st.arena[idx];  // copy first: the slot dies below
  st.removed_qty += static_cast<std::uint64_t>(o.leaves_qty.value);
  const LevelIndex lvl = o.level;
  const bool resting = o.is_resting();
  if (resting) {
    // Unlink from the book this order actually lives in. Using the liquidity
    // book unconditionally would splice a stop's queue links into a liquidity
    // level and silently destroy that level's contents.
    Book& book = book_of(st, o);
    book.remove_from_queue(idx);
    if (&book == &st.book) {
      // Only liquidity changes are published as L2 deltas; a stop is not
      // displayed liquidity and must not appear in the market data feed.
      emit_delta(st, lvl,
                 book.level_at(lvl).empty() ? UpdateAction::Removed : UpdateAction::Changed);
    }
  }
  st.index.erase(o.order_id);
  st.arena.release(idx);

  Event e = new_event(st.symbol_id, EventType::Cancelled, ts);
  e.order_id = o.order_id;
  e.participant = o.participant;
  e.side = o.side;
  e.price = o.price;
  e.qty = Quantity{0};
  e.leaves_qty = o.leaves_qty;
  e.reason = reason;
  events_.push_back(e);
}

void Engine::retire_order(SymbolState& st, OrderIndex idx) noexcept {
  if (idx == kNullOrder) {
    return;
  }
  // Fully filled orders have nothing left, so this normally adds zero; an order
  // retired with quantity still outstanding (self-trade prevention cancelling
  // the aggressor) must be accounted for or the conservation law breaks.
  st.removed_qty += static_cast<std::uint64_t>(st.arena[idx].leaves_qty.value);
  const OrderId id = st.arena[idx].order_id;
  if (st.arena[idx].is_resting()) {
    book_of(st, st.arena[idx]).remove_from_queue(idx);
  }
  st.index.erase(id);
  st.arena.release(idx);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

void Engine::submit(const CancelRequest& request) noexcept {
  SymbolState* st = state(request.symbol);
  if (st == nullptr) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::UnknownSymbol;
    events_.push_back(e);
    return;
  }
  const OrderIndex idx = st->index.find_any(request.order_id);
  if (idx == kNullOrder || st->arena[idx].participant.value != request.participant.value) {
    // UnknownOrder rather than a permission error: a client must not be able to
    // probe for the existence of another participant's orders.
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::UnknownOrder;
    events_.push_back(e);
    return;
  }
  remove_order(*st, idx, CancelReason::ClientRequest, request.ts);
}

// ---------------------------------------------------------------------------
// Replace
// ---------------------------------------------------------------------------

void Engine::submit(const ReplaceRequest& request) noexcept {
  SymbolState* st = state(request.symbol);
  if (st == nullptr) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::UnknownSymbol;
    events_.push_back(e);
    return;
  }
  const OrderIndex idx = st->index.find_any(request.order_id);
  if (idx == kNullOrder || st->arena[idx].participant.value != request.participant.value) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::UnknownOrder;
    events_.push_back(e);
    return;
  }

  Order& o = st->arena[idx];

  // A pending stop lives in a stop book, not the liquidity book. Replacing it
  // has to move it within that book; running the liquidity path would splice a
  // stop's links into a liquidity level and corrupt it.
  if (is_stop_type(o.type)) {
    replace_stop_order(*st, request);
    return;
  }

  if (request.new_quantity.value < o.filled_qty.value) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceWouldReduceBelowFilled;
    events_.push_back(e);
    return;
  }
  const Price target_price = request.new_price.value != 0 ? request.new_price : o.price;
  if (target_price.value == o.price.value && request.new_quantity.value == o.total_qty.value) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceNoPriceChange;
    events_.push_back(e);
    return;
  }
  if (request.new_quantity.value == 0) {
    // Reduce-to-zero is a cancel, not a replace.
    remove_order(*st, idx, CancelReason::ClientRequest, request.ts);
    return;
  }

  const bool price_changed = target_price.value != o.price.value;
  const bool qty_increased = request.new_quantity.value > o.total_qty.value;
  if (price_changed && !st->book.in_domain(target_price)) {
    Event e = new_event(request.symbol, EventType::Rejected, request.ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::PriceOutOfRange;
    events_.push_back(e);
    return;
  }

  // Ordering matters here. Unlinking subtracts the order's *current* leaves from
  // the level aggregate, so any case that loses priority has to be unlinked
  // before the new quantity is written, then relinked afterwards so the new
  // quantity is what gets added.
  const LevelIndex old_level = o.level;
  const std::int64_t old_leaves = o.leaves_qty.value;
  const bool keeps_priority = !price_changed && !qty_increased;

  if (!keeps_priority && o.is_resting()) {
    st->book.remove_from_queue(idx);
  }

  // A replace changes how much quantity the venue owes the client, so the
  // conservation counters have to follow it: a shrink is a cancellation, a
  // growth is new acceptance.
  const std::int64_t qty_delta =
      request.new_quantity.value - static_cast<std::int64_t>(o.total_qty.value);
  if (qty_delta < 0) {
    st->removed_qty += static_cast<std::uint64_t>(-qty_delta);
  } else if (qty_delta > 0) {
    st->accepted_qty += static_cast<std::uint64_t>(qty_delta);
  }

  o.price = target_price;
  o.total_qty = request.new_quantity;
  o.leaves_qty = Quantity{request.new_quantity.value - o.filled_qty.value};

  if (keeps_priority) {
    // Still in the queue, so its shrinking contribution has to be taken off the
    // aggregate directly.
    st->book.reduce_level(old_level, Quantity{old_leaves - o.leaves_qty.value});
  } else if (price_changed) {
    st->book.arena()[idx].level = st->book.index_of(target_price);
    (void)st->book.add_to_queue(idx);
  } else {
    // Quantity increase at an unchanged price: back of the same level.
    st->book.arena()[idx].level = old_level;
    (void)st->book.add_to_queue(idx);
  }

  // The execution report comes before the book deltas it causes, so a client
  // applying events in order always learns of the change before it sees the
  // market move.
  Event e = new_event(request.symbol, EventType::Replaced, request.ts);
  e.order_id = o.order_id;
  e.participant = o.participant;
  e.side = o.side;
  e.price = o.price;
  e.qty = o.total_qty;
  e.leaves_qty = o.leaves_qty;
  events_.push_back(e);

  if (price_changed) {
    emit_delta(
        *st, old_level,
        st->book.level_at(old_level).empty() ? UpdateAction::Removed : UpdateAction::Changed);
    emit_delta(*st, st->book.index_of(target_price), UpdateAction::Changed);
  } else if (keeps_priority) {
    emit_delta(*st, old_level, UpdateAction::Changed);
  }
}

void Engine::replace_stop_order(SymbolState& st, const ReplaceRequest& request) noexcept {
  const OrderIndex idx = st.index.find_any(request.order_id);
  Order& o = st.arena[idx];
  const Timestamp ts = request.ts;

  if (request.new_quantity.value < o.filled_qty.value) {
    Event e = new_event(request.symbol, EventType::Rejected, ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceWouldReduceBelowFilled;
    events_.push_back(e);
    return;
  }
  if (request.new_quantity.value == o.total_qty.value) {
    Event e = new_event(request.symbol, EventType::Rejected, ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceNoPriceChange;
    events_.push_back(e);
    return;
  }
  if (request.new_quantity.value == 0) {
    remove_order(st, idx, CancelReason::ClientRequest, ts);
    return;
  }
  // A price change cannot apply to a pending stop: its price is its trigger.
  if (request.new_price.value != 0 && request.new_price.value != o.price.value) {
    Event e = new_event(request.symbol, EventType::Rejected, ts);
    e.order_id = request.order_id;
    e.reject_code = RejectCode::ReplaceNoPriceChange;
    events_.push_back(e);
    return;
  }

  Book& book = o.side == Side::Buy ? st.stops_buy : st.stops_sell;
  const LevelIndex lvl = o.level;
  const std::int64_t old_leaves = o.leaves_qty.value;
  const bool increased = request.new_quantity.value > o.total_qty.value;

  const std::int64_t qty_delta = request.new_quantity.value - o.total_qty.value;
  if (qty_delta < 0) {
    st.removed_qty += static_cast<std::uint64_t>(-qty_delta);
  } else if (qty_delta > 0) {
    st.accepted_qty += static_cast<std::uint64_t>(qty_delta);
  }

  if (increased) {
    // Growing a stop loses priority, exactly as for a resting order.
    book.remove_from_queue(idx);
    o.total_qty = request.new_quantity;
    o.leaves_qty = Quantity{request.new_quantity.value - o.filled_qty.value};
    st.arena[idx].level = lvl;
    (void)book.add_to_queue(idx);
  } else {
    o.total_qty = request.new_quantity;
    o.leaves_qty = Quantity{request.new_quantity.value - o.filled_qty.value};
    book.reduce_level(lvl, Quantity{old_leaves - o.leaves_qty.value});
  }

  Event e = new_event(request.symbol, EventType::Replaced, ts);
  e.order_id = o.order_id;
  e.participant = o.participant;
  e.side = o.side;
  e.price = o.price;
  e.qty = o.total_qty;
  e.leaves_qty = o.leaves_qty;
  events_.push_back(e);
}

// ---------------------------------------------------------------------------
// Mass cancel
// ---------------------------------------------------------------------------

void Engine::submit(const MassCancelRequest& request) noexcept {
  // Administrative action, so walking the occupied levels is acceptable and no
  // extra per-participant index is justified. Victims are collected first
  // because removing an order mutates the very links being walked.
  for (const std::unique_ptr<SymbolState>& up : states_) {
    SymbolState& st = *up;
    // All three books: a mass cancel that left a participant's pending stops
    // alive would be a surprising half-measure, and stops are orders too.
    const std::array<Book*, 3> books = {&st.book, &st.stops_buy, &st.stops_sell};
    for (Book* book : books) {
      std::size_t n = 0;
      for (LevelIndex lvl = 0; lvl < book->domain(); ++lvl) {
        if (book->level_at(lvl).empty()) {
          continue;
        }
        for (OrderIndex cur = book->level_at(lvl).head; cur != kNullOrder;
             cur = st.arena[cur].next) {
          if (st.arena[cur].participant.value == request.participant.value) {
            st.scratch[n++] = cur;
          }
        }
      }
      for (std::size_t i = 0; i < n; ++i) {
        remove_order(st, st.scratch[i], CancelReason::ClientRequest, request.ts);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Session end
// ---------------------------------------------------------------------------

void Engine::on_session_end(Timestamp now) noexcept {
  for (const std::unique_ptr<SymbolState>& up : states_) {
    SymbolState& st = *up;
    std::size_t n = 0;
    for (LevelIndex lvl = 0; lvl < st.book.domain(); ++lvl) {
      if (st.book.level_at(lvl).empty()) {
        continue;
      }
      for (OrderIndex cur = st.book.level_at(lvl).head; cur != kNullOrder;
           cur = st.arena[cur].next) {
        if (st.arena[cur].tif == TimeInForce::Day) {
          st.scratch[n++] = cur;
        }
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      remove_order(st, st.scratch[i], CancelReason::TimeInForce, now);
    }

    // Unfilled stops do not survive the session.
    std::size_t m = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
      Book& book = side == Side::Buy ? st.stops_buy : st.stops_sell;
      for (LevelIndex lvl = 0; lvl < book.domain(); ++lvl) {
        if (book.level_at(lvl).empty()) {
          continue;
        }
        for (OrderIndex cur = book.level_at(lvl).head; cur != kNullOrder;
             cur = st.arena[cur].next) {
          st.scratch[m++] = cur;
        }
      }
    }
    for (std::size_t i = 0; i < m; ++i) {
      const Order o = st.arena[st.scratch[i]];
      const bool is_buy = o.side == Side::Buy;
      (is_buy ? st.stops_buy : st.stops_sell).remove_from_queue(st.scratch[i]);
      st.removed_qty += static_cast<std::uint64_t>(o.leaves_qty.value);
      st.index.erase(o.order_id);
      st.arena.release(st.scratch[i]);
      Event e = new_event(st.symbol_id, EventType::Cancelled, now);
      e.order_id = o.order_id;
      e.participant = o.participant;
      e.side = o.side;
      e.price = o.trigger_price;
      e.qty = o.leaves_qty;
      e.reason = CancelReason::StopCancelled;
      events_.push_back(e);
    }
  }
}

// ---------------------------------------------------------------------------
// State and invariants
// ---------------------------------------------------------------------------

std::uint64_t Engine::state_hash() const noexcept {
  std::uint64_t h = 0xCBF29CE484222325ULL;
  const auto mix = [&h](std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
  };
  mix(next_seq_);
  for (const std::unique_ptr<SymbolState>& up : states_) {
    const SymbolState& st = *up;
    mix(st.book.state_hash());
    mix(st.stops_buy.state_hash());
    mix(st.stops_sell.state_hash());
    mix(st.has_last_trade ? 1U : 0U);
    if (st.has_last_trade) {
      mix(static_cast<std::uint64_t>(st.last_trade_price.value));
    }
    mix(st.trade_count);
  }
  return h;
}

std::vector<std::string> Engine::book_digest() const {
  std::vector<std::string> out;
  for (const std::unique_ptr<SymbolState>& up : states_) {
    const SymbolState& st = *up;
    // Two passes so every buy is emitted before every sell, matching the order
    // the reference engine uses.
    for (int side_i = 0; side_i < 2; ++side_i) {
      const Side side = side_i == 0 ? Side::Buy : Side::Sell;
      // Walk only occupied levels. The grid can be tens of thousands of ticks
      // wide and this runs after every operation in the differential test, so
      // scanning it in full would dominate the run.
      for (LevelIndex lvl = st.book.lowest_occupied(); lvl != kNullLevel;
           lvl = st.book.next_occupied_index(lvl)) {
        // Filter by side: a level holds one side, but without this every order
        // would be emitted in both the buy and the sell pass.
        if (st.book.level_at(lvl).side != side) {
          continue;
        }
        for (OrderIndex cur = st.book.level_at(lvl).head; cur != kNullOrder;
             cur = st.arena[cur].next) {
          const Order& o = st.arena[cur];
          std::string line;
          line.reserve(96);
          line += std::to_string(st.symbol_id.value);
          line += ' ';
          line += (side == Side::Buy ? "buy" : "sell");
          line += ' ';
          line += std::to_string(st.book.price_of(lvl).value);
          line += ' ';
          line += std::to_string(o.order_id.value);
          line += ' ';
          line += std::to_string(o.total_qty.value);
          line += ' ';
          line += std::to_string(o.filled_qty.value);
          line += ' ';
          line += std::to_string(o.leaves_qty.value);
          line += ' ';
          line += std::to_string(static_cast<int>(o.type));
          line += ' ';
          line += std::to_string(static_cast<int>(o.tif));
          out.push_back(std::move(line));
        }
      }
    }
  }
  return out;
}

bool Engine::check_invariants(std::string_view* error) const noexcept {
  const auto fail = [&](const char* msg) {
    if (error != nullptr) {
      *error = msg;
    }
    return false;
  };
  for (const std::unique_ptr<SymbolState>& up : states_) {
    const SymbolState& st = *up;
    if (!st.book.check_invariants(error)) {
      return false;
    }
    if (!st.stops_buy.check_invariants(error) || !st.stops_sell.check_invariants(error)) {
      return false;
    }
    // Conservation: every accepted lot is executed, removed or still resting.
    // This is the check that catches a level aggregate drifting out of step
    // with the orders it is supposed to summarise.
    const auto as_count = [](Quantity q) { return static_cast<std::uint64_t>(q.value); };
    const std::uint64_t resting = as_count(st.book.total_resting_qty()) +
                                  as_count(st.stops_buy.total_resting_qty()) +
                                  as_count(st.stops_sell.total_resting_qty());
    if (st.accepted_qty != st.filled_qty + st.removed_qty + resting) {
      return fail("quantity conservation violated: accepted != filled + removed + resting");
    }
    // An order is resting in exactly one of the two books, never both.
    for (LevelIndex lvl = 0; lvl < st.book.domain(); ++lvl) {
      for (OrderIndex cur = st.book.level_at(lvl).head; cur != kNullOrder;
           cur = st.arena[cur].next) {
        if (is_stop_type(st.arena[cur].type)) {
          return fail("untriggered stop order is resting in the liquidity book");
        }
      }
    }
  }
  return true;
}

}  // namespace lob