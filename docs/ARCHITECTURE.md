# Architecture

## Layering

The strict rule is a one-way dependency: `core ← journal ← engine ← gateway`.
`src/core` links nothing but the standard library. It has no sockets, no clock,
no filesystem, no threads and no exceptions on the hot path. Everything it
needs from the outside world is injected.

```
clients ──TCP──> [Gateway] ──SPSC──> [Sequencer/Journal] ──> [Matching Engine]
                                                                    │
                                fills/acks/rejects/market data <───┘ ──> [Publisher]
```

Milestone status is in `docs/FINAL_REPORT.md`; only `src/core` exists today.

---

## The core

### Strong types

Every domain integer has its own type (`Price`, `Quantity`, `OrderId`,
`SymbolId`, `ParticipantId`, `Sequence`, `Timestamp`, `TradeId`). A `Price`
cannot be passed where a `Quantity` belongs. That class of bug is invisible in
review and catastrophic in a venue, and `-Wconversion` does not catch it.

The wrappers are thin — trivially copyable, `noexcept`, no virtuals — and
`types.cpp` asserts their sizes at compile time, because those sizes are part of
the contract with anything that serialises them.

There is **no floating point in `src/core`**. Tick conversion is the protocol
layer's job.

### Price levels: flat array + occupancy bitmap

A per-symbol `Book` owns a flat array of `Level` indexed by `price - min_price`,
valid over a config-bounded domain, plus a 64-bit occupancy bitmap and cached
`best_bid_` / `best_ask_` indices.

| Operation | Cost |
|---|---|
| `best_bid()` / `best_ask()` | **O(1)** — a load of a cached index |
| level lookup | **O(1)** — `price - min_price` |
| insert/remove at a non-extreme level | **O(1)** — bitmap set/clear |
| insert/remove at an extreme level | amortised **O(1)**; worst case O(domain/64) |
| matching N levels | **O(N)**, no per-level search |

The extremes are the only part that needs care. When an extreme level empties,
the new extreme is the nearest set bit found with `__builtin_clzll`, which
stops at the first non-empty word. Its worst case is bounded by the configured
domain.

A level holds exactly **one side**. That is not an optimisation; it is what makes
the book structurally incapable of crossing, because `add_to_queue` refuses to
put the other side into an occupied level.

The domain is bounded by config and validated at startup. A `std::map` fallback
was considered and rejected as unreachable: with domain validation there is no
case it would handle. Exchange price bands are bounded by listing rules, so a
bounded domain is the realistic assumption, and enforcing it turns an unbounded
memory hazard into a startup error.

### Orders: fixed arena, index-based links

`OrderNode`s live in a pre-sized array that is **never reallocated**. The
intrusive FIFO links are `uint32_t` indices, not pointers.

- No owning pointers anywhere in the core.
- No reallocation, so no reference or iterator can be invalidated.
- No allocator churn: `Level`s are never created or destroyed either.
- Indices keep neighbouring orders of the same level close together in memory.

`kNullOrder` is **0**, and arena slot 0 is reserved. This is not cosmetic: with
a `UINT32_MAX` sentinel, `while (const OrderIndex i = head)` exits only at slot
0 — a *valid* order — and runs off the end of the arena. Making the sentinel
zero means `if (idx)` means "has order" and the bug class cannot recur.

### The id index: open addressing, no tombstones, never rehashes

`OrderId → {slot, generation}`, linear probing, load factor capped at 1/2.

- **It never rehashes.** Sized once from the per-symbol order limit. A full
  table makes insertion fail, and the caller rejects with `BookFull`. Rehashing
  would allocate and produce a latency spike proportional to the table size at
  exactly the moment the venue is busiest.
- **Deletion uses backward shift**, not tombstones. Cancel-heavy venues would
  otherwise accumulate dead slots until the table looked full while being mostly
  empty.
- **Generations** reject stale handles: a slot recycled between a client's
  cancel and its arrival cannot remove an unrelated order.

Occupancy is encoded in the generation word rather than a sentinel `OrderId`,
because `OrderId` 0 is a legal wire value.

### Events

One flat `Event` struct (~120 bytes) with a type tag, emitted through a
caller-owned `EventBuffer` sized once at startup. Not `std::variant`: a flat
struct is a single cache-friendly copy, directly memcmp-able for differential
testing, and trivially serialisable into a journal.

`EventBuffer` capacity is derived from the maximum number of resting orders,
because one aggressive order can produce one `Trade` per maker it consumes plus
an L2 delta per level touched. Exceeding it is a configuration error and trips
an assertion rather than dropping execution reports.

### One arena per symbol, three books over it

A symbol's resting book and its two stop books are three price structures over
**one** arena and **one** id index. An order is in exactly one of them, or in
none while it is matching — which is what keeps a single `OrderId` unambiguous.

Buy and sell stops get separate books because a price level holds one side, and
a buy stop and a sell stop can legitimately share a trigger price.

`Book` therefore takes `OrderArena&` and `OrderIndexTable&` rather than owning
them, and `SymbolState` is immovable and held behind `unique_ptr`, because the
books hold pointers into its own arena.

### Matching

The aggressor walks the opposite side's best level first, applying:

1. self-trade prevention, evaluated before every fill,
2. a fill capped by the taker's remainder **and** what the maker is currently
   displaying (so an iceberg's reserve is unreachable until it regains priority),
3. priority loss for an iceberg whose displayed slice was consumed,
4. stop-trigger collection against the new last trade price.

Everything a fill touches is accounted explicitly: the level aggregate, both
orders' quantities, and the per-symbol conservation counters. The aggregate
adjustment is the subtle part — a partially filled maker stays in its queue, so
its contribution has to be reduced directly rather than by unlinking it.

### Determinism

- Same request sequence ⇒ byte-identical event stream. No output depends on
  pointer values, hash iteration order, threads or wall clock.
- Timestamps and sequence numbers are injected; the core never reads a clock.
- The hash index is never iterated to produce output; state walks the grid in
  price order and each queue in priority order.
- `state_hash()` is a pointer-free digest over observable state *including queue
  position*, so a replay can be proven identical rather than merely similar.

---

## Memory layout

| Structure | Per-unit size | Allocation |
|---|---|---|
| `Order` | ≤ 104 bytes, hot fields first | arena array, once |
| `Level` | 24 bytes (head, tail, aggregate, count, side + padding) | flat array, once |
| occupancy bitmap | 1 bit per price tick | once |
| id-index slot | 16 bytes | once, power-of-two |
| `Event` | ~120 bytes | fixed buffer, once |

Per symbol, with a default 65,536-tick domain and 65,536 order capacity, a
symbol costs roughly 1.6 MB of levels and bitmap, 1 MB of id index, 6.8 MB of
arena and 3 MB of stop grids. **Domains and capacities are the memory knobs**,
and both are validated at startup rather than discovered at runtime.

---

## What is not here yet

`src/journal`, `src/engine` (runtime), `src/gateway` and `tools/` are empty. The
architecture for each is in `docs/PLAN.md` §2 and the interfaces they will need
are already visible in the core: `state_hash()` for a snapshot digest, a flat
`Event` for the journal and the feed, and injected `Timestamp`/`Sequence` for a
replay to be bit-identical.