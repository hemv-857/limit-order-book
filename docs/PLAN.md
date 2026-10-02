# Implementation Plan — Limit Order Book & Matching Engine

Target: a venue-grade deterministic LOB core plus the runtime, gateway, tooling and
verification harness needed to run and validate it.

Status legend: `[ ]` todo · `[~]` in progress · `[x]` done · `[!]` blocked/deviation

---

## 0. Environment reality check (done before any code)

The build host is **macOS 27.0.1, arm64 (Apple M4), 10 cores / 16 GB**, not Linux.
Everything below was verified by running it, not assumed.

| Requirement | Local status | Resolution |
|---|---|---|
| CMake 3.22+ | 4.4.3 ✅ | Use presets. |
| Ninja | missing from PATH | Installed **inside repo** via `.venv` wheel. |
| GCC 12+ / Clang 15+ | Clang 21.0.0 ✅ · **no GCC** | `g++` is Apple Clang. GCC validated in CI only. |
| `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` | ✅ verified | Applied to all first-party targets. |
| ASan + UBSan | ✅ compile + run verified | `asan-ubsan` preset. |
| TSan | ✅ compile + run verified | `tsan` preset. |
| GoogleTest / Google Benchmark via FetchContent | ✅ verified (v1.15.2 / v1.9.1) | Pinned to exact tags. |
| clang-format / clang-tidy | ✅ installed into `.venv` (23.1.2 / 22.1.8) | Local-only; no system Homebrew changes. |
| libFuzzer | ❌ **runtime absent** in Apple CLT | See deviation D-2. |
| `epoll` | ❌ not on macOS | See deviation D-1. |
| `perf` / flamegraphs | ❌ no Linux perf | See deviation D-3. |

Pinned dependency versions: googletest `v1.15.2`, benchmark `v1.9.1`.

---

## 1. Guiding constraints

1. **Correctness before speed.** Every performance number must be backed by a
   correctness test that exists first.
2. **Determinism is a testable property**, not an aspiration: same input bytes ⇒
   identical output bytes, proven by a differential harness.
3. **Zero allocation on the hot path** is verified by an instrumented test that
   traps `operator new` and `malloc`, not by inspection.
4. **The core knows nothing about I/O.** No sockets, no clock reads, no threads,
   no filesystem, no exceptions. Everything it needs is injected.
5. **No floating point in `src/core`.** All prices/quantities are integers.

---

## 2. Module breakdown

```
include/lob/            public headers (thin, documented)
  core/    types.hpp  order.hpp  book.hpp  engine.hpp  config.hpp  events.hpp
           hash_index.hpp  arena.hpp  grid.hpp  risk.hpp  stp.hpp  validate.hpp
  journal/ journal.hpp  snapshot.hpp  crc32c.hpp  framing.hpp
  engine/  spsc_ring.hpp  shard.hpp  sequencer.hpp  runtime.hpp
  gateway/ codec.hpp  frame.hpp  session.hpp  reactor.hpp  publisher.hpp
  util/    histogram.hpp  metrics.hpp  logger.hpp  config_file.hpp  endian.hpp

src/core/       the deterministic engine. STL only. No syscalls.
src/journal/    WAL + snapshot + recovery.
src/engine/     threads, SPSC rings, sequencing, shutdown.
src/gateway/    sockets, protocol codec, market data fan-out.
src/util/       histogram, metrics, async logger, endian, arg parsing.
tools/          venue (server), lobctl, loadgen, replay, bookviz.
tests/          unit, property, differential, golden, recovery, integration.
fuzz/           libFuzzer-style targets + committed corpus.
bench/          Google Benchmark microbenchmarks.
```

Dependency direction is strictly one-way: `core ← journal ← engine ← gateway`.
`core` links nothing but the standard library.

---

## 3. Key design decisions (details and rationale in `docs/DECISIONS.md`)

### 3.1 Price storage: bounded flat array + occupancy bitmap
The spec allows "flat array/bitmap for dense tick ranges with a sorted fallback".

**Decision:** per-symbol, a flat `Level` array indexed by `price - min_price`,
valid only over a **config-bounded** price domain, plus a 64-bit occupancy
bitmap over that domain and cached `best_bid_idx_` / `best_ask_idx_` hints.

- `best_bid()` / `best_ask()` read a member ⇒ **O(1)**, no search.
- Insert/remove touching a non-extreme level ⇒ **O(1)** (bitmap set/clear).
- Removing an extreme level resynchronises the hint by scanning bitmap words with
  `__builtin_clzll`: amortized **O(1)**, worst case O(domain/64) which is bounded
  by config (default domain 2^20 ticks ⇒ 16 384 words ⇒ a few µs, and only when
  the entire top of book is swept).
- Config validation **rejects** domains larger than a hard ceiling rather than
  silently falling back to a tree. This is the "well-justified alternative" the
  spec permits: exchange price bands are bounded by listing rules, so a bounded
  domain is the realistic assumption, and enforcing it at config time turns an
  unbounded-memory hazard into a startup error.

A `std::map` fallback was considered and rejected as dead code: with domain
validation in place it is unreachable. (YAGNI.)

### 3.2 Orders: fixed arena + intrusive index-based linked list
`OrderNode`s live in a pre-sized array that is **never reallocated**; links are
`uint32_t` indices, not pointers.

- No raw owning pointers anywhere.
- No reallocation ⇒ no iterator/reference invalidation ⇒ no UB, and stale
  references are impossible because indices stay valid for the node's lifetime.
- Free list of indices for O(1) alloc/free.
- `kNullOrder` is a sentinel index, so no null checks in the hot loop.

### 3.3 Order lookup: open-addressing hash index, never rehashes
Pre-sized power-of-two table, linear probing, load factor capped at a configured
maximum. If a New would exceed the cap, the order is **rejected with
`RejectCode::BookFull`** — it never triggers a rehash.

This is what makes the zero-allocation guarantee achievable: the alternative
(rehashing under load) would produce exactly the unbounded latency spike the spec
forbids.

`OrderId → {node index, generation}`; generation checked on lookup so a recycled
index cannot alias a stale cancel.

### 3.4 Events: flat struct into a caller-provided ring
One flat POD-ish `Event` (~64 B) with a `EventType` tag rather than
`std::variant`, emitted through an `EventSink` interface backed by a pre-sized
ring owned by the caller. Rationale: no heap, no allocation on emit, trivial
`memcpy` for journaling and market data, and a single predictable cache footprint.

### 3.5 Priority, replace, iceberg
- Price-time priority: FIFO per level, strictly insertion order.
- Replace with **price change or qty increase ⇒ loses priority** (moved to back).
  Replace with **qty decrease only ⇒ keeps priority**. Reduce-to-zero treated as
  cancel. Cannot reduce below executed quantity.
- Iceberg: order carries `display_qty` and `total_qty`. Displayed size is
  `min(display_qty, remaining)`. When the displayed portion is exhausted and
  reserve remains, the order **re-queues at the back** of its level (priority
  loss) and replenishes.
- Self-trade prevention applies to the *maker* side against the aggressor's
  participant id, and is evaluated before each fill.

### 3.6 Stops
Triggered by **last trade price**. Stop orders live outside the LOB in a separate
stop book keyed by trigger price, so they never occupy liquidity before triggering.
Trigger evaluation order on a trade: buy-stop triggers on `last >= stop_price`,
sell-stop on `last <= stop_price`. Ties (last exactly equal) trigger. Triggered
orders are dispatched as new orders through the normal path, so cascades
(stop → fill → another stop triggers) are just repeated application.

### 3.7 Determinism
- No `unordered_map`/pointer-ordered iteration in any state or output path.
  Hash index iteration is never used to produce output; output walks the grid.
- Sequence numbers are a single monotonically increasing counter.
- The wall clock is never read inside `core`; `Timestamp` is injected.
- `-fno-rtti` is *not* used (GoogleTest needs it), but `core` is `noexcept`-clean.

---

## 4. Milestones

Each milestone ends with: clean build (zero warnings), full test run, a commit,
and its box ticked **here in this file**.

- [x] **M0 — Skeleton & toolchain.** CMake presets (`debug`, `release`,
      `asan-ubsan`, `tsan`, `coverage`), warnings-as-errors, clang-format/tidy
      configs, local tool bootstrap script, `.gitignore`, license.
- [ ] **M1 — Core types + order book.** Strong types, arena, hash index, grid,
      FIFO levels, cancel, state hash, unit tests, zero-allocation test.
- [ ] **M2 — Full semantics.** Order types, TIFs, post-only, iceberg, stops,
      replace, mass cancel, STP, pre-trade risk + reject codes; naive reference
      engine; differential test (≥10 M ops) and property/invariant tests.
- [ ] **M3 — Journal & recovery.** WAL framing with CRC32C, segment rotation,
      fsync policy, snapshots, replay, torn-tail handling, state-hash equality.
- [ ] **M4 — Engine runtime.** SPSC rings (documented memory ordering), sequencer,
      sharded single-threaded engines, pinning, backoff, graceful drain, TSan.
- [ ] **M5 — Gateway & protocol.** Wire codec, framing, session state machine,
      reactor interface (epoll + kqueue), partial I/O, slow-consumer policy,
      heartbeat/timeout, market data publisher with snapshot+increments.
- [ ] **M6 — Tools & e2e.** `venue`, `lobctl`, `loadgen`, `replay`, `bookviz`;
      end-to-end integration tests over real TCP.
- [ ] **M7 — Fuzzing & sanitizers.** Fuzz targets for protocol decoder, journal
      reader, engine input; committed corpus; ASan/UBSan/TSan clean on the suite.
- [ ] **M8 — Performance.** Google Benchmark suite, profiling, hotspot fixes,
      measured before/after, results recorded.
- [ ] **M9 — Docs & CI.** README, ARCHITECTURE, PROTOCOL, MATCHING_RULES,
      OPERATIONS, DECISIONS, FINAL_REPORT, GitHub Actions matrix.

---

## 5. Testing strategy

| Layer | Technique | Gate |
|---|---|---|
| Unit | GoogleTest, one behaviour per test, every order type / TIF / STP / risk code | all pass |
| Property | Invariant checks after **every** op in debug builds | invariants hold |
| Differential | Naive `std::map`+`std::deque` reference vs. core, randomized streams, byte-identical event streams | **≥ 10 000 000 ops** |
| Fuzz | libFuzzer-style targets for decoder / journal reader / engine input | no findings, corpus committed |
| Golden | Recorded scenarios with expected event streams checked in | byte-identical |
| Recovery | Kill mid-write, truncate, flip bytes in tail | recovery lands on identical state hash |
| Concurrency | TSan on rings and full pipeline | race-free |
| Integration | Real TCP clients against real server | scripted expectations |
| Coverage | gcov/lcov on `src/core`, `src/journal` | **≥ 90 % line** |

Invariants asserted in debug builds:
1. Book is never crossed (`best_bid < best_ask`, or one side empty).
2. Each level's aggregate quantity equals the sum over its orders.
3. ID index agrees with the book (and contains no orphans).
4. No negative quantities; executed qty ≤ order qty.
5. Volume conservation: Σ fill qty on buy side == Σ fill qty on sell side.
6. Per-level queue order matches arrival sequence (price-time priority).

---

## 6. Assumptions (documented, revisit if wrong)

- A **bounded price domain per symbol** is a valid venue model (§3.1).
- `Quantity` is a signed 64-bit integer with a **positive-only** validation
  rule; zero/negative is a pre-trade reject, never a wrap.
- Notional (`price × qty`) is computed in 128-bit and range-checked to 64-bit.
- `OrderId`s are venue-assigned and monotonically increasing per session, but the
  core treats them as opaque and never assumes ordering.
- Session expiry (Day orders, stop cleanup) is driven by an **injected** session
  clock/event, not by wall time.
- One order may be *one* of: Limit, Market, Stop, Stop-Limit (not combined with
  Reduce-Only, which is out of scope per spec).

---

## 7. Known deviations from the original spec (kept honest)

- **D-1 — `epoll` unavailable on the build host.** The reactor is an interface with
  an epoll backend (Linux, exercised in CI) and a kqueue backend (macOS, exercised
  locally). Neither backend is a stub.
- **D-2 — libFuzzer runtime absent from Apple CommandLineTools.** Fuzz targets are
  written against the standard `LLVMFuzzerTestOneInput` ABI. CMake links them with
  `-fsanitize=fuzzer` wherever available (Linux CI ⇒ coverage-guided libFuzzer).
  Where that runtime is missing, a **portable in-tree driver** links the *same*
  target object and drives it with a seeded mutation engine under ASan+UBSan
  against the *same* corpus directory. Same target code, two drivers.
- **D-3 — no `perf` on the build host.** Hotspots are found with Google Benchmark
  A/B comparisons and in-process HDR-style histograms instead of sampled stacks.
  Documented per optimization with before/after numbers.
- **D-4 — GCC not installed locally.** The GCC leg of the CI matrix is real, but
  its results are CI-produced, not quoted as local measurements.
- **D-5 — UDP multicast market data** was a stretch goal in the spec and is not
  implemented; TCP market data with sequence numbers and snapshot recovery is.

---

## 8. Milestone log

### M0 — Skeleton & toolchain (complete)

Toolchain proved by running it, not by reading a version string:

- `release`, `asan-ubsan`, `tsan` configure, build warning-free, and run 8/8 tests.
- `clang-format --check` clean; `clang-tidy` reports zero findings in first-party code.
- Google Benchmark links and reports sane timings.

Two bugs were caught by the tooling rather than by inspection, which is the
point of wiring it up first:

1. `checked_mul` widened its operands to **unsigned** 128-bit, so a legitimate
   negative price times a negative quantity overflowed and was falsely rejected.
   The unit test `Types.CheckedMulHandlesNegativeNotional` failed on first run.
   Fixed by widening to signed `__int128` and checking against both `int64`
   bounds (`src/core/types.hpp`).
2. GoogleTest 1.15.2 ships `-Werror` in its own flags and does not compile under
   Apple Clang 21's `-Wcharacter-conversion`. Third-party targets are now given
   `-w` so upstream code can never fail our build; first-party targets keep the
   full `-Werror` treatment.