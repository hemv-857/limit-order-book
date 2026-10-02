# Architecture decision records

Short entries: context, decision, consequences. Ordered roughly by how much they
constrain everything else.

---

## ADR-001 — Prices live in a bounded flat grid, not a tree

**Context.** Best bid/ask must be O(1). A `std::map<Price, Level>` gives O(log n)
top-of-book plus a pointer chase per level, and every lookup during matching
walks it.

**Decision.** A flat `Level` array indexed by `price - min_price`, over a
config-bounded domain, plus a 64-bit occupancy bitmap and cached extreme
indices.

**Consequences.**
- Top of book is a load, not a search; level lookup is one array index.
- The price domain becomes a *configuration* bound, enforced at startup. A
  misconfigured venue fails immediately instead of allocating unbounded memory.
- Memory is `24 bytes × domain` per book whether or not the levels are used. A
  venue with many symbols must size the domain per symbol (ideally to the price
  collar). A sparse `std::map` fallback was considered and **rejected**: with
  domain validation it is unreachable, so it would be untested dead code.

## ADR-002 — Order links are indices, and the null sentinel is zero

**Context.** The obvious intrusive-list design uses `Order*` links into a
`std::vector<Order>`.

**Decision.** `uint32_t` indices into a never-reallocated array, with
`kNullOrder == 0` and arena slot 0 reserved.

**Consequences.**
- No owning pointers in the core; no reallocation, so no dangling links and no
  invalidated references.
- Indices keep same-level orders adjacent in memory.
- The sentinel had to be zero. With `UINT32_MAX`, a loop written as
  `while (const OrderIndex i = head)` exits only at slot **0**, which is a valid
  order, and then dereferences a wild index. This happened during development and
  is why the invariant is now structural rather than stylistic.

## ADR-003 — The id index refuses to grow

**Context.** Open-addressed tables normally rehash when they fill.

**Decision.** Size once from the configured order limit; a full table makes
insertion fail and the order is rejected with `BookFull`.

**Consequences.**
- No allocation and no latency spike on the order path. Rehashing is O(table) and
  would fire at peak load, which is the worst possible time.
- A venue that under-provisions `max_open_orders` starts rejecting orders. That
  is a loud, attributable failure rather than a silent stall.
- The limit must be sized for the real order population.

## ADR-004 — Deletion uses backward shift, not tombstones

**Context.** Tombstone accumulation is proportional to cancel traffic, not to
live orders.

**Decision.** On erase, shift later entries in the probe cluster backwards so no
hole remains.

**Consequences.** The table's occupancy tracks its liveness exactly, so
`BookFull` means the book is genuinely full. The algorithm is easy to get subtly
wrong — an early version decremented nothing — which is why
`IdIndexFillsCompletelyWithoutLosingEntries` churns 512 keys and re-checks every
one.

## ADR-005 — Aggressor-side volume is reported, not asserted equal

**Context.** The spec asks for a volume-conservation invariant.

**Decision.** Report `aggressor_buy_volume` / `aggressor_sell_volume`, and assert
a different law: per symbol, `accepted == filled + removed + resting`. Each fill
advances `filled` by **twice** the traded quantity, because it reduces two
orders' leaves.

**Consequences.** Aggressor volumes are deliberately asymmetric — one order can
sweep the passive side many times over — so asserting equality would either fail
correctly-behaving runs or force the check to be meaningless. The conservation
law is stronger and is the check that actually caught two accounting bugs.

## ADR-006 — A triggered stop is booked as a continuation

**Context.** A stop is accepted when submitted, long before it triggers.

**Decision.** When it triggers, the converted order is created with
`counts_as_acceptance == false`; any rejection from that path books the quantity
as removed (`fail_new`).

**Consequences.** Without this, a stop-limit whose limit price has drifted outside
the collar leaks its quantity out of the conservation law. Found by the
randomised probe, not by review.

## ADR-007 — Buy and sell stops use separate books

**Context.** A price level holds exactly one side — that invariant is what makes
the liquidity book structurally incapable of crossing. A buy stop and a sell stop
can share a trigger price.

**Decision.** Each symbol has `stops_buy` and `stops_sell`.

**Consequences.** One extra grid per stop-enabled symbol. Rejected alternatives:
encoding the side into the level index (breaks the invariant that an order's
price equals its level's price, which the book relies on), and allowing a level
to hold both sides (would break the crossing guarantee). A production deployment
should size the stop domain to the price collar rather than the full domain.

## ADR-008 — Level aggregates include iceberg reserve

**Context.** An iceberg hides quantity. Should the published aggregate be the
displayed slice or the total?

**Decision.** `Level::aggregate_qty` sums `leaves_qty`, so it includes reserve,
and the L2 deltas report the same figure.

**Consequences.** One aggregate, one invariant to check
(`aggregate == Σ leaves`), and the conservation law stays simple. The cost is that
published depth reveals more than a display-only venue would. A venue wanting
displayed-only would carry a second aggregate — deliberately not built here
because nothing in the spec requires it and it doubles the bookkeeping that the
conservation check relies on.

## ADR-009 — Wall-clock time is injected, never read

**Context.** Every request carries a `Timestamp` and a `Sequence`.

**Decision.** The core never reads a clock and never assigns a sequence number
itself; both are supplied by the caller.

**Consequences.** A wall-clock-dependent engine is neither replayable nor
testable, and the whole point of the state hash is proving a replay reproduces the
original. It also makes the 20,000,000-operation invariant probe reproducible from
a seed alone.

## ADR-010 — Rate limiting fails closed

**Context.** Tracking participants in an open-addressed table, on the order path.

**Decision.** Size the table once from `max_participants`; a participant the
table cannot track is refused rather than triggering a rehash.

**Consequences.** Preserves the zero-allocation guarantee on the order path. The
failure mode is refusing a new participant when the table is full, which is
visible and attributable — the alternative, growing under load, is the latency
spike the rest of the design exists to avoid.

## ADR-011 — `clang-tidy` findings are resolved, not silenced, unless the design is intentional

**Context.** The tidy config runs `bugprone-*`, `cert-*`, `concurrency-*`,
`cppcoreguidelines-*`, `performance-*`, `portability-*` and `readability-*`, with
`bugprone`, `cert`, `concurrency`, `performance` and `portability` as errors.

**Decision.** Two checks are disabled with a written rationale
(`pro-bounds-avoid-unchecked-container-access` — index-based access into
pre-sized arrays is the performance design, and the range is what
`check_invariants` asserts; `redundant-member-init` — explicit initialisers on a
struct this safety-critical are documentation). Everything else is fixed in the
code.

**Consequences.** The `bugprone-exception-escape` errors it raised were real: they
exposed a function-local static that allocated inside a `noexcept` accessor, and
a `push_back` that could allocate on the stop path. Both are now structurally
impossible rather than suppressed.

## ADR-012 — Build tools are installed repo-locally

**Context.** The host has no ninja, clang-format or clang-tidy, and installing
them system-wide would modify the machine.

**Decision.** `scripts/bootstrap.sh` installs them as wheels into `./.venv`;
`scripts/env.sh` puts them on `PATH`. Nothing outside the repository is touched.

**Consequences.** The build is reproducible from a clean checkout without root.
CI installs its own copies.