# Matching Rules

The normative specification. The unit tests in `tests/unit/engine_test.cpp` and
the invariant checks in `Book::check_invariants` / `Engine::check_invariants`
verify every statement here; where this document and the code disagree, that is
a bug in one of them and both should be treated as suspect.

Throughout: **taker** is an incoming aggressive order, **maker** is a resting
order it trades against.

---

## 1. Value domains

| Concept | Type | Rules |
|---|---|---|
| Price | `int64` ticks | Must be `> 0` for Limit/StopLimit, within `[min_price, max_price]`, and an exact multiple of `tick_size` for both the limit price and a stop trigger. |
| Quantity | `int64` lots | Must be `> 0` and an exact multiple of `lot_size`. |
| Notional | `int64` | `price × quantity`, computed in signed 128-bit. Overflow is a rejection, never a wrap. |
| OrderId | `uint64` | Opaque. Unique per symbol while live. The core never assumes these are dense or ordered. |

There is **no floating point anywhere in `src/core`**.

A symbol's *price domain* is `[min_price, max_price]` inclusive. It is a
configurable hard bound, not a suggestion: a price outside it cannot be
represented in the flat level grid and is rejected with `PriceOutOfRange`
rather than clamped.

---

## 2. Order types

| Type | Meaning |
|---|---|
| `Limit` | Rests at `price`. Matches when the book crosses it. |
| `Market` | Aggressive. No price; works against whatever liquidity exists. Never rests. |
| `Stop` | Stop-market. Rests **outside** the book until triggered, then becomes a market order. |
| `StopLimit` | Rests outside the book until triggered, then becomes a Limit order at its own `stop_limit_price`. |

Only `Limit` and `StopLimit` carry a limit price. `Market` and `Stop` do not,
and their `price` field is ignored rather than validated — otherwise a correctly
formed stop order would be rejected for a field it does not have.

## 3. Time in force

| TIF | Meaning |
|---|---|
| `Day` | Rests until the session ends; expired then. |
| `GTC` | Rests until cancelled. |
| `IOC` | Fills what is available now; the remainder is cancelled immediately with reason `immediate_or_cancel`. |
| `FOK` | Fills entirely or not at all. Rejected outright with `FokInsufficientLiquidity` if the book cannot fill it. Never rests. |

`Market`, `IOC` and `FOK` orders never rest, so they have no queue position and
can never be cancelled as a resting order.

`FOK` availability is measured **ignoring self-trade prevention**. If STP then
removes liquidity the FOK could have used, STP wins: the remainder is cancelled
with reason `self_trade_prevention` rather than the fill being rolled back. STP
is a compliance gate, so it is treated as the stronger constraint. This is a
deliberate, documented deviation from strict FOK atomicity.

## 4. Post-only

A post-only order that would cross on arrival is handled per
`SymbolConfig::post_only_action`:

- **`Reject`** (default) — the order is rejected with `WouldCrossPostOnly`. This
  is the only lossless option, so it is the default.
- **`Slide`** — the limit price is moved one tick to the *passive* side of the
  touch: a buy slides to `best_ask − tick_size`, a sell to `best_bid +
  tick_size`. The `Accepted` event carries the slid price. If the slid price
  leaves the domain, the order is rejected with `PriceOutOfRange`.

Direction matters: sliding a buy *above* the ask would leave it crossing, which
is the opposite of post-only's purpose.

`Market` + `PostOnly` is `InvalidOrderType` (a market order cannot be made
passive), and `PostOnly` + `FOK` is `InvalidTimeInForce` (it could only ever
resolve to a cancel).

## 5. Iceberg (reserve) orders

An order with `display_qty > 0` is an iceberg.

- **Displayed size** is `min(display_qty, leaves_qty)`.
- A fill can never consume more than the currently displayed size. Reserve
  quantity is unreachable until the order regains priority.
- **When the displayed slice is consumed and reserve remains, the order
  replenishes and moves to the back of its level — it loses priority.** That is
  the entire point of the order type.

`display_qty` must be `> 0` and strictly less than the total, and on the lot
grid, or the order is rejected (`InvalidQuantity` / `QuantityNotOnLot`).

## 6. Priority

Strict **price-time priority**: better price first, then arrival order within a
price level.

- New orders join the **back** of their level.
- A fully filled maker is removed, exposing the next in the queue.
- The cached best bid/ask are maintained on every insert and removal, so reading
  top of book is a single load, not a search.

Priority is deliberately **not** equal to arrival order. The following all move
an order behind newer ones:

- replace with a price change (§8),
- replace with a quantity increase (§8),
- iceberg replenishment (§5).

`Book::check_invariants` therefore checks queue *structure*, not queue order;
price-time priority is a property of the engine's decisions.

## 7. Self-trade prevention

Evaluated before every fill, comparing the maker's and taker's participant ids.

| Mode | Behaviour |
|---|---|
| `None` | Self trades are permitted. |
| `CancelOldest` | The resting maker is cancelled; matching continues with the next maker. |
| `CancelNewest` | The aggressor is cancelled and matching stops. |
| `CancelBoth` | Both are cancelled and matching stops. |
| `DecrementAndCancel` | The aggressor is filled in full; the maker is reduced by the fill quantity and cancelled if it reaches zero. |

When STP cancels the aggressor, its arena slot is released and
`Engine::match` reports that it no longer exists, so no code path can touch the
freed slot.

## 8. Cancel and replace

**Cancel** removes a resting order. A cancel naming an order that is not live,
or one belonging to a different participant, is rejected with `UnknownOrder` —
not with a permission error, so a client cannot probe for the existence of other
participants' orders.

**Replace restates the order in full**: `new_quantity` is the new *total*
quantity, not a delta. `new_quantity == 0` is a cancel. `new_price == 0` keeps
the current price. (A delta form would make it impossible to apply size and
notional limits to the result *before* applying it.)

| Change | Priority |
|---|---|
| Quantity decrease only | **Keeps** priority |
| Quantity increase | **Loses** priority (moves to back) |
| Price change | **Loses** priority (moves to back of the new level) |

Rejections: `ReplaceWouldReduceBelowFilled` if the new total is below what has
already executed; `ReplaceNoPriceChange` if nothing would change;
`PriceOutOfRange` if the new price is outside the domain; and
`ReplaceWouldCross` if a price change would land the order at or through the
opposite touch.

Two rules here exist because leaving them out produces a broken book rather than
a wrong answer:

- A new total **at or below** the already-executed quantity leaves zero
  remaining, so it is a **cancel**, not a replace. Resting a zero-quantity order
  is structurally invisible — its level aggregate equals the sum over its
  orders, because both are zero — but it cannot trade, and a matching loop that
  reaches it computes a zero fill, makes no progress and spins forever.
- A replace never re-runs matching, so a price change that crosses would rest
  the order on the far side of the book. It is rejected rather than matched: that
  keeps the book uncrossed by construction and keeps replace deterministic,
  without inventing matching semantics for a path that had none.

A quantity decrease changes the order's contribution to its level, so the level
aggregate is reduced to match while the queue position is left alone.

## 9. Stop orders

Stops rest in a **second book keyed by trigger price**, on the side they would
trade on. An untriggered stop therefore occupies no liquidity and cannot trigger
against itself. An order is in exactly one of the two books, or in neither while
it is matching.

Trigger rules, evaluated against the **last trade price** after every fill:

- A **buy** stop triggers when `last_trade_price >= trigger_price`.
- A **sell** stop triggers when `last_trade_price <= trigger_price`.

Ties trigger: a trade printing exactly at the trigger price fires it. Buy stops
are walked from the lowest trigger upward and sell stops from the highest
downward, so a cascade fires its nearest trigger first and is reproducible.

A stop whose trigger is already through the market is rejected with
`InvalidStopDirection` (buy trigger `<=` last, sell trigger `>= last`). The
check is skipped when the symbol has never traded, since there is no reference
price to be wrong about.

On trigger the engine emits `StopTriggered`, then converts the order:

- `Stop` → a market order (`Market`, TIF unchanged).
- `StopLimit` → a limit order at the stored `stop_limit_price`.

A pending stop's `price` field holds its *trigger*, so an order's price always
matches the price of the level it is linked to; the limit a `StopLimit` will work
at is held separately.

Cascades are driven by a work list, not recursion, so a long chain of triggered
stops cannot exhaust the stack.

## 10. Rejection order

Checks run in a fixed order so an order with several problems always reports the
same one:

1. `UnknownSymbol`
2. `DuplicateOrderId`
3. `InvalidQuantity` — zero or negative
4. `QuantityNotOnLot`
5. `OrderSizeExceeded`
6. `InvalidOrderType` — unknown type, Market+PostOnly, stop on a stop-disabled symbol
7. `InvalidTimeInForce` — unknown TIF, PostOnly+FOK
8. `InvalidQuantity` / `QuantityNotOnLot` — iceberg display slice
9. `InvalidPrice` / `PriceNotOnTick` / `PriceOutOfRange` / `PriceBelowCollar` / `PriceAboveCollar`
10. `InvalidStopPrice` / `PriceNotOnTick` / `InvalidStopDirection`
11. `NotionalExceeded`
12. `ShuttingDown` — engine draining
13. `RateLimitExceeded` — participant over its allowance
14. `BookFull` — arena or id index at capacity

`BookFull` is reported rather than growing either container. Growing under load
is exactly the unbounded latency spike the design exists to eliminate. Rate
limiting is applied last so a structurally invalid order does not burn quota.

## 11. Rate limiting

A fixed one-second window per participant, keyed on the **injected** timestamp.
A wall-clock limiter would make the engine non-deterministic and therefore
unreplayable. A fixed window has a known burst property of up to 2× the limit
across a window boundary. The table is sized once at startup; a participant it
cannot track is refused rather than triggering a rehash on the order path.

## 12. Session end

`Engine::on_session_end` expires every resting `Day` order (reason
`time_in_force`) and drops every unfilled stop (reason `stop_cancelled`).
`GTC` orders survive.

## 13. Event contract

Every event carries a venue-wide, strictly increasing `Sequence`. For one
request the order is:

**New order**

1. `Rejected` alone, if rejected.
2. Otherwise `Accepted`, emitted **before** any fill, carrying the full requested
   quantity — so a client sees its order come alive first.
3. Then zero or more `Trade` and `BookUpdate` events in match order.
4. Then, if a remainder expires, `Cancelled`.

**Replace** emits `Replaced` **before** the `BookUpdate` deltas it causes, so a
client applying events in order always learns of the change before it sees the
market move. Cancel behaves the same way: `Cancelled` follows its delta.

**Fully filled orders emit no `Cancelled`.** The fills already told the client
everything, and a cancel after a full fill would be actively misleading. Such
an order is removed from the book *and* the id index, so a later cancel for it
is rejected with `UnknownOrder`.

`Trade` identifies **both** sides: `order_id` is the taker, `maker_order_id` is
the resting order consumed. Reporting only the maker's participant and price
would not let a client reconcile a fill against its own book.

`BookUpdate` is an L2 delta: the level price, the level's **new aggregate** and
an `UpdateAction` of `Added` / `Changed` / `Removed`. Aggregates cover an
iceberg's reserve as well as its displayed slice; a venue that publishes
displayed-only quantities would carry a second aggregate.

## 14. Determinism

- The same request sequence always produces the same event sequence, byte for
  byte. Verified by `SameInputProducesIdenticalEventStreams`.
- No output depends on pointer values, hash iteration order, threads or wall
  clock. State walks the level grid in price order and each queue in priority
  order.
- `Book::state_hash` / `Engine::state_hash` are pointer-free digests over
  observable state, including queue position, so a replay can be proven identical
  to the original.

## 15. Invariants

Checked after every operation in debug builds:

1. The book is never crossed.
2. Each level's aggregate equals the sum of its orders' `leaves_qty`.
3. The id index agrees with the book, with no orphans in either direction.
4. No order has non-positive `leaves_qty`; `filled + leaves == total` always.
5. Cached extremes match the occupancy bitmap.
6. Queue back/forward links are consistent.
7. **Conservation:** per symbol,
   `accepted == filled + removed + resting`. Every lot a venue accepts is either
   executed, removed, or still resting. Each fill advances the counter by
   **twice** the traded quantity, because it reduces two orders' `leaves`.
8. An untriggered stop is never resting liquidity.
9. Buy and sell aggressor volumes are reported but deliberately **not** asserted
   equal: one aggressive order can sweep the passive side many times over.

## 16. Reject codes

`none`, `unknown_symbol`, `invalid_quantity`, `invalid_price`,
`price_not_on_tick`, `quantity_not_on_lot`, `order_size_exceeded`,
`notional_exceeded`, `price_below_collar`, `price_above_collar`, `book_full`,
`rate_limit_exceeded`, `duplicate_order_id`, `unknown_order`,
`would_cross_post_only`, `fok_insufficient_liquidity`, `invalid_time_in_force`,
`invalid_stop_price`, `invalid_stop_direction`, `invalid_order_type`,
`participant_not_found`, `session_closed`, `price_out_of_range`,
`replace_below_filled`, `replace_no_price_change`, `shutting_down`,
`invalid_field`, `cancel_exceeds_leaves`.

The numeric values are part of the wire protocol.