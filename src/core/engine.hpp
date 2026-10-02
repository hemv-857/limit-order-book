#pragma once

#include "core/book.hpp"
#include "core/config.hpp"
#include "core/events.hpp"
#include "core/order.hpp"
#include "core/risk.hpp"
#include "core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace lob {

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------
//
// Inputs carry their own sequence number and timestamp. Neither is generated
// inside the core: the sequencer assigns the sequence and the clock is injected.
// That is what makes a replay bit-identical to the original run.

struct NewOrderRequest {
  Sequence seq{};
  Timestamp ts{};
  SymbolId symbol{};
  OrderId order_id{};
  ParticipantId participant{};
  Side side{Side::Buy};
  OrderType type{OrderType::Limit};
  TimeInForce tif{TimeInForce::GTC};
  Price price{};          ///< limit price; ignored for Market
  Price trigger_price{};  ///< stop trigger; only for Stop / StopLimit
  Quantity quantity{};
  Quantity display_qty{};  ///< iceberg slice; 0 means fully displayed
  bool post_only{false};
};

struct CancelRequest {
  Sequence seq{};
  Timestamp ts{};
  SymbolId symbol{};
  OrderId order_id{};
  ParticipantId participant{};
};

/// Cancel/replace.
///
/// A replace restates the order in full, exactly as it would be re-sent on the
/// wire: `new_quantity` is the order's *new total quantity*, not a delta. That
/// is deliberate -- a replace can be rejected on size or notional limits before
/// it is applied, and a delta form would make those checks ambiguous.
///
/// `new_price == 0` keeps the current price. `new_quantity == 0` cancels.
struct ReplaceRequest {
  Sequence seq{};
  Timestamp ts{};
  SymbolId symbol{};
  OrderId order_id{};
  ParticipantId participant{};
  Price new_price{};
  Quantity new_quantity{};
};

struct MassCancelRequest {
  Sequence seq{};
  Timestamp ts{};
  ParticipantId participant{};
};

// ---------------------------------------------------------------------------
// Event buffer
// ---------------------------------------------------------------------------

/// Fixed-capacity event buffer.
///
/// Sized once from configuration and never grown, because emitting must not
/// allocate. The bound is a real one: a single aggressive order can produce one
/// Trade per maker it consumes plus an L2 delta per level touched, so the
/// capacity is derived from the maximum number of resting orders. Exceeding it
/// is a configuration error and trips an assertion rather than silently
/// dropping execution reports.
class EventBuffer {
 public:
  void reset(std::size_t capacity) {
    events_.assign(capacity, Event{});
    count_ = 0;
  }

  void push_back(const Event& e) noexcept {
    assert(count_ < events_.size() && "event buffer capacity exceeded");
    if (count_ < events_.size()) {
      events_[count_++] = e;
    }
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return count_;
  }
  [[nodiscard]] bool empty() const noexcept {
    return count_ == 0;
  }
  void clear() noexcept {
    count_ = 0;
  }

  [[nodiscard]] const Event* data() const noexcept {
    return events_.data();
  }
  [[nodiscard]] const Event& operator[](std::size_t i) const noexcept {
    return events_[i];
  }
  [[nodiscard]] std::size_t capacity() const noexcept {
    return events_.size();
  }

 private:
  std::vector<Event> events_;
  std::size_t count_{0};
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

/// Everything the engine holds for one symbol.
///
/// The resting book and the stop book are two price structures over one arena
/// and one id index. An order is in exactly one of them (or in neither, while an
/// aggressor is matching), which is what keeps a single OrderId unambiguous.
struct SymbolState {
  SymbolState(const SymbolConfig& cfg, std::uint32_t capacity)
      : config(cfg),
        arena(capacity),
        book(cfg, arena, index),
        stops_buy(cfg, arena, index),
        stops_sell(cfg, arena, index) {
    index.reset(capacity);
    // Mass cancel and session expiry collect victims before removing them
    // (removing while walking would disturb the walk), so they need scratch
    // space sized for the worst case. Allocated once, here.
    scratch.resize(capacity);
    trigger_queue.resize(capacity);
  }

  ~SymbolState() = default;
  SymbolState(const SymbolState&) = delete;
  SymbolState& operator=(const SymbolState&) = delete;
  SymbolState(SymbolState&&) = delete;
  SymbolState& operator=(SymbolState&&) = delete;

  SymbolId symbol_id{};
  SymbolConfig config{};
  OrderArena arena;
  OrderIndexTable index;
  Book book;  ///< resting liquidity
  /// Untriggered stop orders, keyed by trigger price.
  ///
  /// Two books rather than one: a buy stop and a sell stop can share a trigger
  /// price, and a price level holds exactly one side by design (that invariant
  /// is what makes the liquidity book incapable of crossing). One stop book
  /// would therefore reject the second stop to arrive. Costs one extra grid per
  /// symbol with stops enabled; symbols with stop_enabled == false allocate
  /// neither, and a venue should size the stop domain to the price collar
  /// rather than the full domain.
  Book stops_buy;
  Book stops_sell;

  bool has_last_trade{false};
  Price last_trade_price{};
  std::uint64_t trade_count{0};

  /// Aggressor-side fill volume: quantity bought by incoming buys and sold by
  /// incoming sells. These are *not* expected to be equal -- one order can sweep
  /// the other side many times over -- so they are reported, not asserted.
  std::uint64_t buy_volume{0};
  std::uint64_t sell_volume{0};

  /// Conservation accounting. Every lot a venue accepts is either executed,
  /// removed, or still resting, so these three plus the resting total must sum
  /// back to the accepted total. This is the invariant that catches a level
  /// aggregate drifting out of step with its orders.
  std::uint64_t accepted_qty{0};
  std::uint64_t filled_qty{0};
  std::uint64_t removed_qty{0};

  /// Pre-sized scratch space so mass cancel and session expiry never allocate.
  std::vector<OrderIndex> scratch;
  /// Orders whose stop has fired and are waiting to be worked. A work list
  /// rather than recursion, so a long stop cascade cannot exhaust the stack.
  ///
  /// Pre-sized and written through an explicit count rather than push_back:
  /// push_back on a full vector would allocate, which is not allowed here.
  std::vector<NewOrderRequest> trigger_queue;
  std::size_t trigger_count{0};
};

/// The deterministic matching engine.
///
/// No threads, no clock, no syscalls, no exceptions. Feed it requests that
/// already carry sequence numbers and timestamps, and read events back out. The
/// same request sequence must always produce the same event sequence.
///
/// Symbols are independent. This class holds them all, but the runtime in
/// src/engine shards whole symbols onto separate threads, so a symbol is never
/// touched by two threads at once.
class Engine {
 public:
  Engine(std::vector<SymbolConfig> configs, EngineConfig engine_config);

  ~Engine() = default;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;

  // ---- request entry points ----

  void submit(const NewOrderRequest& request) noexcept;
  void submit(const CancelRequest& request) noexcept;
  void submit(const ReplaceRequest& request) noexcept;
  void submit(const MassCancelRequest& request) noexcept;

  /// End of trading session: expire every Day order and drop every unfilled stop.
  void on_session_end(Timestamp now) noexcept;

  /// Mark the engine as draining. New orders are rejected with
  /// RejectCode::ShuttingDown while cancels, replaces and mass cancels keep
  /// working, so participants can still flatten their positions.
  void begin_drain() noexcept {
    draining_ = true;
  }
  [[nodiscard]] bool draining() const noexcept {
    return draining_;
  }

  // ---- output ----

  [[nodiscard]] const EventBuffer& events() const noexcept {
    return events_;
  }

  /// Discard accumulated events. The caller drains events() after each request
  /// and calls this to reset the buffer, which keeps the fixed capacity valid
  /// for arbitrarily long runs instead of growing with the session.
  void clear_events() noexcept {
    events_.clear();
  }
  [[nodiscard]] Sequence next_sequence() const noexcept {
    return Sequence{next_seq_};
  }

  // ---- introspection ----

  [[nodiscard]] std::uint64_t state_hash() const noexcept;
  [[nodiscard]] bool check_invariants(std::string_view* error) const noexcept;

  [[nodiscard]] const Book& book(SymbolId symbol) const noexcept;
  [[nodiscard]] const Book& stop_buy_book(SymbolId symbol) const noexcept;
  [[nodiscard]] const Book& stop_sell_book(SymbolId symbol) const noexcept;
  [[nodiscard]] const SymbolState* symbol(SymbolId symbol) const noexcept;

  /// Mutable access for tests and for the snapshot writer.
  [[nodiscard]] Book& mutable_book(SymbolId symbol) noexcept;
  [[nodiscard]] const SymbolConfig* config(SymbolId symbol) const noexcept;

  [[nodiscard]] std::optional<Price> last_trade_price(SymbolId symbol) const noexcept;
  /// Aggressor-side fill volume. Not expected to be symmetric: one aggressive
  /// order can sweep the passive side many times over.
  [[nodiscard]] std::uint64_t aggressor_buy_volume(SymbolId symbol) const noexcept;
  [[nodiscard]] std::uint64_t aggressor_sell_volume(SymbolId symbol) const noexcept;

 private:
  [[nodiscard]] SymbolState* state(SymbolId symbol) noexcept {
    return symbol.value < states_.size() ? states_[symbol.value].get() : nullptr;
  }
  [[nodiscard]] const SymbolState* state(SymbolId symbol) const noexcept {
    return symbol.value < states_.size() ? states_[symbol.value].get() : nullptr;
  }

  void reject(SymbolState& st, const NewOrderRequest& r, RejectCode code) noexcept;

  /// Reject a new order, booking its quantity when it arrived from the
  /// stop-trigger queue.
  ///
  /// Such an order was already counted as accepted when the stop was submitted,
  /// so a rejection here must also book the quantity as removed or the
  /// conservation law leaks. A stop that triggers into an order which no longer
  /// validates -- a stop-limit whose limit price is outside the collar, say -- is
  /// rejected with a code rather than silently dropped.
  void fail_new(SymbolState& st, const NewOrderRequest& r, RejectCode code,
                bool counts_as_acceptance) noexcept {
    if (!counts_as_acceptance) {
      st.removed_qty += static_cast<std::uint64_t>(r.quantity.value);
    }
    reject(st, r, code);
  }

  /// Reject a new order that arrived from the stop-trigger queue.
  ///
  /// Such an order was already counted as accepted when the stop was submitted,
  /// so a rejection here must also book its quantity as removed or the
  /// conservation law leaks. A stop that triggers into an invalid order (a
  /// stop-limit whose limit price is outside the collar, say) is rejected
  /// rather than silently dropped, and the client sees the reject code.
  void reject_triggered(SymbolState& st, const NewOrderRequest& r, RejectCode code) noexcept {
    st.removed_qty += static_cast<std::uint64_t>(r.quantity.value);
    reject(st, r, code);
  }

  /// Full new-order path: post-only handling, FOK proof, accept, match, rest.
  /// Takes its argument by value because a slid post-only order has its price
  /// rewritten, and mutating the caller's request would be a nasty surprise.
  ///
  /// `counts_as_acceptance` is false for orders arriving from the stop-trigger
  /// queue: a triggered stop was already accepted when it was submitted, and
  /// counting it twice would break the conservation invariant.
  void handle_new_order(SymbolState& st, NewOrderRequest request,
                        bool counts_as_acceptance) noexcept;

  /// Returns false when the aggressor no longer exists (self-trade prevention
  /// cancelled it), in which case the caller must not touch its arena slot again.
  [[nodiscard]] bool match(SymbolState& st, OrderIndex aggressor_idx, Timestamp ts) noexcept;
  void execute_fill(SymbolState& st, OrderIndex aggressor_idx, OrderIndex maker_idx,
                    Timestamp ts) noexcept;
  void rest_remainder(SymbolState& st, OrderIndex order_idx) noexcept;

  /// Unlink, unindex and free a resting order, emitting Cancelled.
  void remove_order(SymbolState& st, OrderIndex idx, CancelReason reason, Timestamp ts) noexcept;
  /// Unlink, unindex and free without emitting anything. Used when an order has
  /// been fully filled: the fills already told the client everything it needs,
  /// and a Cancelled after a full fill would be actively misleading.
  static void retire_order(SymbolState& st, OrderIndex idx) noexcept;

  void emit_delta(SymbolState& st, LevelIndex level, UpdateAction action) noexcept;

  /// Queue any stops whose trigger the given trade price reached.
  void collect_triggers(SymbolState& st, Price trade_price) noexcept;
  void dispatch_stop_level(SymbolState& st, Side side, LevelIndex lvl) noexcept;

  void drain_trigger_queue(SymbolState& st) noexcept;

  /// Cursor into SymbolState::trigger_queue, so draining can enqueue more work
  /// while walking it.
  std::size_t trigger_cursor{0};

  [[nodiscard]] Event new_event(SymbolId symbol, EventType type, Timestamp ts) noexcept;

  /// Held indirectly because SymbolState is immovable: the books inside it hold
  /// pointers to its own arena and index, so any move would dangle them. There
  /// is one heap block per symbol, all created at startup, never resized.
  std::vector<std::unique_ptr<SymbolState>> states_;
  /// Returned for out-of-range symbols so callers never null-check a SymbolId.
  /// One tick wide with one order slot, so reading it costs nothing and no
  /// allocation can happen on the access path. Owns its own arena rather than
  /// borrowing one from a real symbol, which would make it depend on the
  /// construction order of members declared before it.
  SymbolState invalid_;
  EngineConfig engine_config_;
  EventBuffer events_;
  RiskManager risk_;
  std::uint64_t next_seq_{1};
  bool draining_{false};
};

}  // namespace lob