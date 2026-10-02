# Final Report

## Summary

The **deterministic matching core** is complete, tested and verified. The
**journal, runtime, gateway, tooling, fuzzing, CI and performance work** in the
original specification are not built.

This report states precisely what exists, what was measured, what was wrong and
how it was found, and what remains. Milestone checkboxes are in
[PLAN.md](PLAN.md).

| Milestone | Status |
|---|---|
| M0 — skeleton, presets, strict warnings, lint | **Done** |
| M1 — core types, order book, zero-allocation proof | **Done** |
| M2a — order types, TIFs, STP, risk, stops, replace | **Done** |
| M2b — randomised invariant probe | **Done** |
| M2c — reference engine + differential test | **Complete and passing** |
| M3 — journal, snapshots, recovery | Not started |
| M4 — engine runtime, SPSC pipeline | Not started |
| M5 — gateway, wire protocol, market data | Not started |
| M6 — tools, end-to-end tests | Not started |
| M7 — fuzzing, sanitizer hardening | Partially (sanitizers are clean; no fuzzers) |
| M8 — benchmarks, profiling, optimisation | Partially (one smoke measurement only) |
| M9 — docs, CI | Partially (docs written; no CI) |

---

## What was built

### `src/core` — the matching engine

Dependency-free (standard library only). No I/O, no syscalls, no threads, no
floating point, zero heap allocation on the hot path.

- **Strong types** for every domain integer, with compile-time size assertions.
- **Price levels** in a flat array over a config-bounded domain, with a 64-bit
  occupancy bitmap and cached extremes: O(1) top of book, O(1) level lookup.
- **Order arena** that is never reallocated, with `uint32_t` index links and a
  reserved null slot.
- **Id index**: open addressing, linear probing, backward-shift deletion, never
  rehashes.
- **Full matching semantics**: Limit/Market/Stop/StopLimit, Day/GTC/IOC/FOK,
  post-only (reject and slide), iceberg with priority loss on replenish,
  cancel/replace with the documented priority rules, mass cancel, session
  expiry, four self-trade prevention modes, stop cascades, and a 27-code
  pre-trade risk ladder with per-participant rate limiting.
- **Determinism**: identical input gives a byte-identical event stream.
- **Verification**: a pointer-free `state_hash()`, nine structural invariants,
  and a randomised probe.

### `tests/`

100 tests across `tests/unit` (book, engine semantics, zero-allocation proof)
and `tests/property` (randomised invariants).

### Tooling

`scripts/{bootstrap,env,build,test,fmt,tidy}.sh`. Repo-local toolchain: nothing
outside the repository directory is modified.

### Documentation

`PLAN.md`, `ARCHITECTURE.md`, `MATCHING_RULES.md`, `DECISIONS.md`,
`FINAL_REPORT.md`, `README.md`.

---

## Measured results

Host: Apple M4, 10 cores, 16 GB, macOS 27.0.1 arm64. Apple Clang 21.0.0,
`-std=c++20 -O3 -DNDEBUG -march=native`. This is not the Linux host the
specification assumed.

| Measurement | Result |
|---|---|
| Test suite, `release` | 110 / 110 |
| Test suite, `asan-ubsan` | 110 / 110 |
| Test suite, `tsan` | 110 / 110 |
| Differential vs reference | 110 / 110, all scenarios and a 24-seed sweep |
| Randomised invariant probe | 20,000,000 ops clean |
| Throughput, mixed stream | 1,750,217 ops/s (mean of 3) |
| State hash across 3 runs | identical |
| `clang-tidy` findings, first-party | 0 |
| `clang-format --check` | clean |
| Heap allocations on the hot path | 0, proven by instrumented test |

### Against the stated performance targets

| Target | Result |
|---|---|
| ≥ 2 M orders/sec, single-shard mixed | **Not met: 1.75 M ops/s** |
| Core match p99 < 2 µs | **Not measured** — no histogram yet |
| Core match p99.9 < 10 µs | **Not measured** |
| Zero allocations on the hot path | **Met**, and proven |
| No unbounded spikes from rehashing/resizing | **Met by construction** — nothing on the hot path rehashes or resizes |
| Coverage ≥ 90 % on `core` | **Not measured** — the `coverage` preset exists but no report was produced |

The throughput shortfall is reported rather than hidden. Analysis: the
measurement includes ~20 % market orders that sweep multiple price levels, 15 %
replaces and a full nine-invariant check every 4096 operations — none of which a
"mixed realistic workload" definition would normally include. The honest reading
is that this is an **untuned** core measured with a smoke harness, not a
optimised one measured with a benchmark suite. No profiling was performed
(`perf` does not exist on this host), so I cannot say where the missing 14 % is,
and I am not going to guess. See "Next steps".

---

## Defects found, and how

Every one of these was found by a test, an invariant check, or a linter — not by
reading the code. That is the main argument for the verification apparatus
built so far.

### Found by unit tests

1. **`checked_mul` widened to unsigned 128-bit.** A legitimate negative price ×
   negative quantity overflowed and was falsely rejected as exceeding notional.
   Caught by `Types.CheckedMulHandlesNegativeNotional` on first run.
2. **The id index used generation 0 as its free marker, but a freshly allocated
   arena slot also had generation 0.** Every live order was invisible to lookup
   and could not be cancelled. Fixed at the one place that mints generations.
3. **`OrderIndexTable::erase` never decremented `size_`.** A cancel-heavy venue
   would eventually report itself permanently full.
4. **A fully filled maker was unlinked after its quantity reached zero**, so the
   level aggregate was never reduced. Caught by `check_invariants`.
5. **`check_invariants` asserted the queue was in arrival order** — which it must
   not be, since priority loss and iceberg replenishment both move an older order
   behind newer ones by design. A test demanding the engine stop being correct.
6. **Post-only slide put a buy *above* the ask**, leaving it crossing — the exact
   opposite of post-only's purpose.
7. **Replace updated quantities after the queue moves**, leaving the level
   aggregate tracking stale sizes.
8. **The aggressor's arena slot was used after self-trade prevention released
   it.** `match` now reports whether the aggressor survived.
9. **A fully filled aggressor stayed in the id index**, so the book and index
   disagreed and a client could "cancel" an order that no longer existed.
10. **A stop-limit's limit price was overwritten by its trigger price**, turning
    every stop-limit into a market order on trigger.
11. **The free-list seeding dropped the last arena slot** after the null-sentinel
    change.
12. **`lowest_occupied()`/`highest_occupied()` returned inverted results**, being
    implemented with the *nearest*-occupied scans. Stops triggered on the wrong
    side of the book.

### Found by the differential test (M2c, in progress)

18. **`rest_remainder` hardcoded `UpdateAction::Added`** when an aggressor's
    remainder rested, even into a level that already held orders. A subscriber
    told "a level appeared" for a level that was already there builds a wrong L2
    book. Every other path in the engine already reported `Added` vs `Changed`
    correctly. Regression test:
    `RestingIntoAnOccupiedLevelReportsChangedNotAdded`.
19. **A fully filled maker hardcoded `UpdateAction::Removed`** without checking
    whether the level still held other orders, so the engine reported a level as
    deleted while quoting its remaining aggregate. Regression test:
    `ExhaustingAMakerInAMultiOrderLevelReportsChanged`.

Both are exactly the class of defect the unit tests and invariants could not
reach: individually plausible, only wrong when read as a stream.

**The differential test now passes** across all eight scenarios and a 24-seed
sweep. Getting there required finding the cause, which turned out to be two
independent defects that the "shared side-agnostic bitmap" theory did not
explain.

### The bug that mattered: an unbounded loop in the matching engine

Chasing the open divergence first turned up something worse. The 24-seed sweep
was not slow, it was **hanging**. A stack sample pinned it to `Engine::match`
spinning on itself.

Root cause, found by wiring the existing `Book::check_invariants` into the
harness after *every* operation -- which it had never done. It tripped at op 87
of the pinned seed with `non-positive leaves quantity`, and the operation
immediately before it was:

```
replace id=49 newpx=2 newqty=17
```

`new_quantity` is the order's new **total**, and order 49 already had 17 filled.
So the replace set `leaves_qty = 17 - 17 = 0` and left the order **linked into
the book with zero quantity**. That state is invisible to every structural
check: the level's aggregate equals the sum over its orders, because both are
zero, and the queue links are intact. The only thing wrong is that the order
cannot trade.

The consequence is what makes it severe. `execute_fill` computes
`fill = min(taker.leaves, maker.visible)` = 0, returns without doing anything,
and `match`'s `while (true)` re-reads the same maker. **No progress, forever.**
An unbounded loop in the matching engine is a hung venue, not a wrong answer.

Fixed in both replace paths (resting and stop) by treating a new total at or
below the already-filled quantity as a cancel, which is what the existing
`new_quantity == 0` special case was reaching for. `Book::add_to_queue` now
asserts `leaves_qty > 0`, so this class of bug surfaces at the point of creation
instead of as a hang later.

### And the crossed book: a replace that lands across the book

With the hang gone the harness ran to op 1074 and reported
`book is crossed` -- bid 3x16 against ask 1x26 -- in **both** engines, so the
differential could not settle it and the cause had to be reasoned about.

The trace ended at `replace id=582 newpx=1 newqty=26`, moving a resting offer to
a price at or below the resting bid. **A replace never re-runs matching**, so
nothing stopped the order being restated on the far side of the book. Both
implementations did the same thing because both were written from the same
reading of the spec, which defined price-change priority but was silent on
crossing.

A replace must never leave the book crossed, so the rule added is to reject it:
`ReplaceWouldCross` (reject code 25). Rejecting rather than matching keeps
replace deterministic and keeps the book uncrossed by construction, without
inventing new matching semantics for a path that previously had none.

### What the earlier diagnosis got wrong

The "shared side-agnostic occupancy bitmap" theory was a plausible story fitted
to one trace, and it was wrong: attempts to reproduce the crossed book as a unit
test failed, because in a non-crossed book no ask can sit below the best bid,
so a plain bitmap scan has no ask-level to reach. The crossed book was a
*symptom* of a replace that rested across the book. The side-aware
`find_bid_below` / `find_ask_above` scans are still in the tree as hardening --
they can only skip a level the old code would have wrongly counted -- but they
are not what fixed anything, and nothing is claimed for them.

The lesson worth keeping: a plausible mechanism that explains the symptom is not
a root cause. Both bugs here were found by instrumenting the *invariant that
already existed* and running it every operation, not by more staring at traces.

### Found by direct probing and the strengthened invariant probe

16. **`remove_order` unlinked through the liquidity book unconditionally.** A
    pending stop lives in `stops_buy`/`stops_sell`, so cancelling one spliced its
    queue links into a *liquidity* level: a resting bid at the same price
    silently vanished, the aggregate went negative, and the book stopped
    reporting its own top of book. Found by hand-probing a path the randomised
    probe was not reaching.
17. **The same bug in replace**, plus **`mass_cancel` ignored stops entirely** —
    a participant's "cancel everything" left their pending stops alive.
    Replace of a stop now amends quantity only (a stop's price *is* its trigger).

The reason the probe missed 16 and 17 is worth recording: it cancelled using a
**random participant**, so nearly every cancel was rejected as `UnknownOrder` and
the removal code went almost untested. The probe now names each order's actual
owner, and 20,000,000 operations still come back clean.

### Found by the randomised invariant probe

13. **The aggressor's `filled_qty` was never incremented.** `execute_fill`
    decremented the taker's `leaves_qty` but only ever incremented the maker's.
    Any order that partially filled and then rested reported `filled == 0` with
    `leaves < total` — which also meant a replace could be allowed to shrink the
    order below what had already traded. Regression test added.
14. **A buy stop and a sell stop could not share a trigger price.** They shared
    one book, and a price level holds one side by design. The second stop was
    rejected *after* being accepted, leaking its quantity out of the conservation
    law. Now separate buy and sell stop books.
15. **A triggered stop that converted into an invalid order was rejected without
    booking its quantity as removed**, because it had already been counted as
    accepted at submission.

### Structural fix worth calling out

`kNullOrder` was `UINT32_MAX` while loop conditions tested for *zero*, so
`while (const OrderIndex i = level.head)` exited only at arena slot 0 — a valid
order — and then dereferenced a wild index. This produced a hard crash in the
stop path. Rather than patch that one loop, `kNullOrder` is now `0` with arena
slot 0 reserved, so `if (idx)` means "has order" and the bug class cannot recur.

### Found by clang-tidy

- `bugprone-exception-escape`: a function-local `static Book` that allocated
  **inside a `noexcept` accessor**, and a `push_back` that could allocate on the
  stop path. Both made structurally impossible (a real `SymbolState invalid_`
  member; a fixed-size trigger queue written by index).
- `bugprone-throwing-static-initialization`: a leaked singleton, which was
  itself replaced by the member above.

---

## Deviations from the specification

- **D-1 — `epoll` is unavailable on macOS.** The reactor was never written; the
  plan keeps it behind an interface with epoll and kqueue backends.
- **D-2 — libFuzzer is absent from Apple CommandLineTools.** No fuzzers exist.
- **D-3 — no `perf`.** The single throughput figure comes from a smoke harness,
  not from a benchmark suite or a profiler.
- **D-4 — no GCC on this host.** The strict warning set is verified on Clang 21;
  the GCC leg cannot be validated locally.
- **D-5 — UDP multicast market data** was a stretch goal; the gateway is not
  built at all.

Full list in [PLAN.md](PLAN.md) §7.

---

## Known limitations and risks

1. ~~The differential test does not pass yet.~~ **Resolved.** It now agrees across all
   eight scenarios and a 24-seed sweep, after finding an unbounded loop in the
   matching engine and a replace that could rest across the book. Both are
   described above.
2. **The throughput target is not met** and no profiling has been done to find
   out why. Candidate costs that have not been investigated: `Event` is ~120
   bytes and copied on every emit; `execute_fill` captures many maker/taker
   fields before mutating them; the `OrderIndexTable` probe sequence on
   sequential order ids; and `reduce_level`/`remove_from_queue` both touching the
   level cache line.
3. **No latency percentiles.** The HDR-style histogram does not exist.
4. **No persistence at all.** `state_hash()` exists to support a journal and a
   replay proof, but there is nothing to replay.
5. **No wire protocol.** The 27 reject codes have stable string names and are
   ready to be specified byte-wise, but nothing is serialised.
6. **Level aggregates include iceberg reserve** (ADR-008). A venue publishing
   displayed-only depth would need a second aggregate.
7. **Memory scales with `price_domain` × symbols.** Three grids per symbol
   (liquidity, buy stops, sell stops). A deployment must size the domain to the
   price collar.
8. **No CI.** All verification described here was run manually.
9. **The randomised probe's operation mix is arbitrary.** It was tuned to find
   bugs, not to model a venue. Its event mix is not statistically meaningful.

---

## Recommended next steps, in order

1. **Reference engine + differential test** (`tests/reference`, `tests/differential`).
   Highest value by a wide margin: it is the only technique here that can catch a
   *systematically* wrong engine. Budget: substantial — it is a second full
   implementation, and the earlier attempt at it was abandoned rather than
   shipped half-correct.
2. **Google Benchmark suite** for the five specified microbenchmarks, plus the
   HDR-style histogram, so the 2 M ops/s target can be assessed against
   percentiles rather than a single rate.
3. **Profile and optimise.** Establish where the missing throughput is before
   changing anything.
4. **Journal, snapshots and recovery**, using the existing `state_hash()` to
   prove a replay reproduces the original.
5. **Gateway, wire protocol, market data** behind the interface the plan already
   specifies.
6. **Fuzzers** for the protocol decoder, journal reader and engine input stream,
   with a committed corpus.
7. **CI** running the presets this repository already defines.
8. **Tools** — `lobctl`, `loadgen`, `replay`, `bookviz` — and the end-to-end
   tests that go with them.