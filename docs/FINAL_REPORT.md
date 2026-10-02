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
| M2c — reference engine + differential test | **Not done** |
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
| Test suite, `release` | 100 / 100 |
| Test suite, `asan-ubsan` | 100 / 100 |
| Test suite, `tsan` | 100 / 100 |
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

1. **No differential test.** The highest-value missing piece. There is no second
   independent implementation, so the invariants prove *self-consistency* rather
   than *correctness against the specification*. A subtle systematic error — wrong
   fill ordering, wrong reject ordering — would survive every test currently in
   the suite. `validate_new_order` was already refactored to take a `SymbolRules`
   POD specifically to make the reference engine easy to write.
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