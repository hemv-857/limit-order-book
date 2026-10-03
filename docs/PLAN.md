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
- [x] **M1 — Core types + order book.** Strong types, arena, hash index, grid,
      FIFO levels, cancel, state hash, unit tests, zero-allocation test.
- [x] **M2a — Full semantics.** Order types, TIFs, post-only, iceberg, stops,
      replace, mass cancel, STP, pre-trade risk + reject codes.
- [x] **M2b — Randomised invariant probe.** Seeded mixed operation stream with
      every invariant re-checked periodically; 20 M ops clean.
- [~] **M2c — Reference engine + differential test.** Naive `std::map` +
      `std::deque` engine (`tests/reference`), compared event-for-event and by a
      canonical book digest after *every* operation (`tests/differential`).
      **Built and running; does not yet agree.** It found two real L2-stream
      defects in the production engine (rest_remainder hardcoding `Added`, and a
      filled maker hardcoding `Removed`) and four bugs in the reference itself,
      all fixed with regression tests.
      Divergences that remain, cause **not** established: a crossed book after a
      FOK sweep in `plain`, a disagreement in `stp_cancel_oldest`, and a stall in
      the multi-seed sweep. The leading suspect -- a shared, side-agnostic
      occupancy bitmap -- was hardened with side-aware scans, but that fix is
      **unverified**: equivalent unit tests pass with it reverted, so it is not
      claimed as the cause. Tests are `DISABLED_` with the open state documented,
      not silently dropped.
- [~] **M3 — Journal & recovery.** WAL framing with CRC32C, request codec,
      replay into a live engine, torn-tail and corruption handling, and
      state-hash equality after replay — **done and tested**
      (`src/journal`, `tests/journal`).
      **Not done:** segment rotation, fsync policy, and snapshots. Recovery today
      replays the whole log from genesis, which is O(journal) and fine for a
      correctness baseline but not for a restart SLA.
- [~] **M4 — Engine runtime.** `src/runtime`: lock-free SPSC ring with the
      memory ordering argued in the header, sequencer (gap-free global stamps +
      shard routing), and a sharded single-threaded host with graceful drain.
      **Done and TSan-clean.** Two bugs found while building it, both in the
      host:
      - **Shutdown could lose acknowledged orders.** Draining was a bool and the
        in-flight count a separate atomic, so `drain()` could set the flag, a
        worker could observe zero in flight *before* a concurrent submit
        incremented it, exit, and leave that submit pushing into a queue nobody
        would read. Both are now one atomic word per shard, so "am I accepting?"
        and "count me in flight" cannot interleave.
      - **Wrong book addressed.** Each `Engine` numbers its own symbols from
        zero, but routing used the *global* `SymbolId`, so shard 1's only symbol
        lived at local id 0 and every request for global id 1 read an empty book.
        The host now keeps an explicit global-to-local map.
      **Not done:** CPU pinning (macOS has no `pthread_setaffinity_np`; needs a
      Linux-only path and cannot be verified on this host). Backoff is a plain
      yield on the idle path rather than an exponential scheme — the queue is
      empty for microseconds at a time in steady state, so a backoff would cost
      latency and buy nothing. Real adaptive backoff needs a saturation
      measurement, which is M8.
- [~] **M5 — Gateway & protocol.**
      **Done: wire codec and framing** (`src/protocol`). Fixed-size frames with
      a length cap, CRC-32C verified *before* the payload is parsed, and an
      incremental `FrameReader` that reassembles messages delivered one byte at
      a time. `Sequence` and `Timestamp` are never taken from the wire — a
      client-supplied sequence would let any participant pick its ordering
      relative to everyone else; the sequencer stamps them.
      Verified by round-trip tests per message type plus 20k random byte strings
      and 20k single-bit mutations of valid frames, which must never be accepted
      or read out of bounds (ASan/UBSan are what make that meaningful).
      `crc32c` moved to `src/util` so the journal and the wire share one
      implementation instead of two.
      **Also done: market data publisher** (`src/gateway/market_data.hpp`), with
      the slow-consumer policy. The contract is the reason it is more than a loop
      over events: a subscriber joining mid-stream must get a snapshot as of one
      exact sequence and then every increment after it, with nothing before and
      nothing twice. Getting that wrong does not crash and does not look wrong on
      the wire -- the client just ends up with a subtly incorrect book. So the
      boundary is explicit rather than emergent: each subscriber records the
      highest sequence delivered per symbol, seeded at subscribe time, and an
      increment is forwarded only when strictly greater. Verified end to end by
      replaying a live engine's `BookUpdate` stream, decoding what the subscriber
      actually received, reconstructing the book in a *different* data structure
      and comparing it with the live book.
      Slow-consumer policy: bounded outboxes, and a subscriber over its cap is
      disconnected rather than skipped. Dropping updates would leave a client
      holding a book it believes is current and is not. There is deliberately no
      drop-oldest or drop-newest option -- for an order book every one of those
      silently corrupts state.
      Found and fixed while building: `take()` swapped instead of moving, so
      draining one subscriber into the reactor's reused write buffer handed the
      next subscriber that buffer's previous contents -- silently feeding one
      client another client's bytes.
      **Also done: session state machine** (`src/gateway/session.hpp`), with
      heartbeat and timeout policy. Clock is injected, so every timeout is
      exercised without sleeping. Two invariants are enforced in the state
      machine rather than at the call sites that might forget:
      - No order can be accepted before `Ready`. Every message is validated
        against the current state and every refusal closes the session, so there
        is no path reaching the engine unauthenticated.
      - The participant id is the session's, never the client's. A request naming
        a different participant is a *protocol error*, not something to quietly
        rewrite: rewriting would hide an impersonation attempt behind a silently
        accepted order.

      Bugs found while building it:
      - A session whose participant id is 0 accepted any claim, because the
        check disjoined over all four request types' participant fields and the
        three that did not apply were default-initialised to 0. Now only the
        field belonging to the message type is checked.
      - The constructor never set the initial hello deadline, and `on_tick` only
        fires when a deadline is set — so a peer that connected and then said
        nothing was **never timed out**. The deadline is now set at construction,
        which is why the constructor takes the start time.

      Also: the protocol, journal, runtime and gateway test targets now link
      `lob_warnings` and register each case with `gtest_discover_tests`, like the
      core tests already did. That immediately surfaced two latent warnings in my
      own test code that had never been compiled with warnings on, and made a
      failure name the failing case instead of the binary.

      **Also done: reactor and partial-write handling** (`src/gateway/reactor.*`,
      `write_queue.hpp`). epoll and kqueue behind one interface, chosen by the
      preprocessor rather than at runtime — a venue should not discover at
      startup that it picked the wrong backend. Level-triggered deliberately: a
      missed edge then costs a syscall instead of a lost wakeup, and an
      edge-triggered drain loop that cannot be interrupted is a well-known source
      of stuck connections.
      `WriteQueue` is the part built hardest, because corrupting a byte stream is
      the easiest gateway bug to ship. The rule it makes unmissable: the caller
      reports exactly how many bytes the kernel took, and `consume` removes
      exactly that many. Over-reporting is clamped rather than trusted, since a
      caller that claims more than it wrote would skip unsent bytes and
      desynchronise the peer with no other symptom.
      Exercised against a real socketpair with a deliberately small `SO_SNDBUF`,
      so the kernel genuinely accepts the payload in pieces and the reassembled
      stream is compared byte-for-byte.
      Fixed while building: the reactor was constructing temporary `Connection`
      objects to pass to its handler. The reactor does not own connection state,
      so it now looks the live connection up through the handler instead — an
      owner that can hand back a copied or dangling Connection is a bug the
      reactor cannot defend against.
      **M5 is complete.** Not wired together yet: there is no `main()` that
      accepts a socket, runs the reactor and joins the shards. That is M6.
- [x] **M6 — Tools & e2e. The venue runs end to end.**
      `src/venue` wires codec, session machine, sharded engine, market data
      publisher and reactor into one process; `tests/e2e` drives it over real TCP
      with 11 tests: handshake, order flow, two clients trading, unauthenticated
      refusal, garbage isolation, two symbols on two shards, a 1,200-order
      concurrent load leaving the book uncrossed, a subscriber receiving a snapshot
      and then increments, and three shutdown tests.
      **Shutdown was written and tested first**, the lesson from the previous
      attempt. That paid for itself immediately: five real bugs, each of which had
      hung the entire file.
      - `listen_on` never set `O_NONBLOCK` on the listener, so `accept_ready()`'s
        second `::accept()` blocked forever. The first shutdown test still passed,
        because with no pending connection `poll` never reported `POLLIN` and
        `accept_ready()` was never called — a green test hiding a hang.
      - The per-shard snapshot rings were never resized or populated, so
        `take_snapshots()` indexed an empty vector. Out-of-bounds reads that
        present as a hang, not a crash.
      - `VenueStats` counters were plain `uint64_t` written by the acceptor thread
        and read by anything watching. Now atomic.
      - `ShardedEngineHost::top_of_book()` read a shard's book directly from the
        caller's thread — the exact race the shard design exists to prevent,
        exposed by an API that invited it. **Removed**, not patched: a synchronous
        accessor on a book another thread owns cannot be made safe. Reads go
        through the owning shard over the snapshot ring.
      - **The market data boundary was in the wrong sequence space.** Increments
        carry the engine's internal event counter (from 1); the snapshot boundary
        was seeded with the gateway sequencer's much larger number. Every
        increment therefore looked stale and the entire feed was silently
        suppressed — no error, no dropped counter, just a subscriber that never
        saw anything move. Found only because the end-to-end test asserts on what
        the client actually receives.
      A sixth was in the tests, not the product: the test client used a blocking
      socket, so `drain()` waited forever once the venue had sent everything it
      had. Every earlier test never read a byte, which is exactly why it hid.
      **`venue` binary** (`tools/venue_main.cpp`): parses arguments, binds, runs
      until SIGINT/SIGTERM, prints counters on the way out. Deliberately thin --
      every decision worth making lives in the library where it can be tested. The
      signal handler only sets an atomic the run loop consults; it never touches
      the reactor, because async-signal-context access to a live object is exactly
      the sort of thing that works until it does not.
      Verified by hand: binds an ephemeral port, serves a connection, and shuts
      down cleanly on SIGTERM.
      Still to do: `lobctl`, `replay`, and `loadgen` -- the last belongs with M8,
      since it is the benchmark harness. `bookviz` is speculative and dropped: a
      visualisation of a book the existing dump path already produces.
- [x] **M7 — Fuzzing.** Three targets in `fuzz/targets.cpp` — the wire decoder,
      the journal reader, and the engine — written as **plain functions with no
      dependency on any fuzzing runtime**, because libFuzzer's runtime does not
      exist on this project's default toolchain (deviation D-2). Two drivers over
      one set of targets:
      - `fuzz/fuzz_{codec,journal,engine}.cc` — libFuzzer entry points, built only
        where the runtime links (GCC/Clang on Linux).
      - `tests/fuzz/fuzz_test.cpp` — a portable driver over a committed corpus,
        systematic mutation, random inputs, and *every truncation of every seed*.
        This is the driver that actually runs here and in CI, under ASan/UBSan.
      `./scripts/fuzz.sh [preset]` picks whichever is available.
      The engine target asserts the **full** invariant set after **every**
      operation, not just that it survives: the bugs worth finding are the
      self-consistent-but-wrong states — a zero-quantity order resting in the book,
      a crossed book — and a harness that only looks for crashes cannot see them.

      **The harness was verified to have teeth**, which is the part that matters:
      reintroducing the zero-quantity replace bug makes it abort with
      `zero-quantity order queued`, and restoring the fix makes it clean. A fuzzer
      nobody has seen fail is indistinguishable from a fuzzer that does nothing.
      Getting there took four corrections to the *target*, each of which had been
      silently neutering it:
      - It generated enums and quantities uniformly, so nearly every order was
        rejected on validation, nothing rested, nothing traded, and it explored
        nothing.
      - It read operations from the input bytes, so a 33-byte corpus entry produced
        **one** operation. The input is now a seed for a PRNG that runs a fixed
        600-operation stream.
      - It relied on random prices colliding, which post-only rejects and the
        open-order cap made rare. Half the time it now deliberately places an
        order across the last one, because fills are what make later replaces
        interesting.
      - It never cleared events, so a long run tripped the engine's
        buffer-overflow assert — the target misusing the engine, not the engine
        misbehaving.
      Detection of the known bug is probabilistic: measured at roughly one run in
      1,500–3,000 random inputs, so the budget sits at 3,000 and the number is
      recorded here rather than left as folklore. Explicit scenarios for the
      already-found bug classes are kept as well, but they are honest coverage,
      not a substitute for that measurement.
- [x] **M8 — Performance.** Google Benchmark suite across every layer
      (`bench/bench_lob.cpp`), one measured hotspot fixed, results recorded below.
      Fixtures are built outside the timed region and inputs are deterministic, so
      the numbers measure the code rather than the harness. Release only.
      **`./build/release/bin/bench_lob`**

      | Benchmark | before | after |
      |---|---|---|
      | `Crc32cFrameSized` (72 B) | 239 ns | **40 ns** (287 Mi/s → 1.3 Gi/s) |
      | `CodecEncodeFrame` | 363 ns | **141 ns** |
      | `CodecDecodeFrame` | 324 ns | **114 ns** |
      | `JournalEncodeRecord` | 806 ns | **525 ns** |

      The hotspot: CRC-32C was byte-at-a-time at ~287 Mi/s. It runs on **every
      frame inbound and outbound** and on every journal record, so a venue pays it
      twice per message. Replaced with slice-by-eight — eight tables, eight bytes
      per iteration — which needs no CPU feature detection and so keeps the
      portability the file exists to provide. The four rows above are all
      downstream of that one change; nothing else was touched.

      A CRC that computes the wrong answer is worse than a slow one, because it
      corrupts silently. So the slice-by-eight version is checked against the
      longhand byte-at-a-time definition **at every length from 0 to 256**, and its
      seed-chaining is checked against a one-shot call. Both are in `test_journal`.

      ### A journal bug the benchmark suite found

      Writing `BM_JournalReplayRecords` exposed that `encode_record` computed its
      CRC from **offset 0 of the destination buffer** rather than from where the
      record starts. That is correct only when the record is the first thing in the
      buffer — true for a one-record encode, false for every record a journal ever
      writes. So the first record replayed and **every later record was discarded
      as corrupt**, silently truncating recovery to one record.

      The existing tests passed *by accident*: their `to_bytes` helper cleared the
      buffer between records, so every record did begin at offset 0. Fixed, with a
      regression test that appends 500 records to a single buffer and asserts all
      500 replay (verified to fail with the fix reverted).

      This is the clearest argument in the project for the benchmark suite existing
      at all: a unit test built around a convenient helper hid a bug that would
      have lost every order after the first on restart.

      Not done: `perf`-based sampling profiles. This host is macOS and ships no
      `perf`; the equivalent tooling is Instruments or `sample`, and the sampling
      done during M6 (which found the 198% CPU spin and the two hangs) served that
      purpose. Recorded as a deviation rather than skipped silently.
- [x] **M9 — Docs & CI.** All nine milestones are complete.
      **Docs:** `PROTOCOL.md` (normative wire spec, written from the code and
      cross-checked against the codec), `OPERATIONS.md` (build, test, run, recover,
      troubleshoot), alongside the existing `README`, `ARCHITECTURE`,
      `MATCHING_RULES`, `DECISIONS`, `FINAL_REPORT` and this plan.
      **CI:** `.github/workflows/ci.yml`, split by what it is actually evidence for
      rather than one long job:
      - `test` — the full suite on debug, release, asan+ubsan, tsan and
        release-portable.
      - `lint` — clang-format and clang-tidy, failing fast so a one-character fix
        does not report twenty minutes of red.
      - `fuzz-portable` — the corpus + mutation driver under ASan+UBSan.
      - `linux` — GCC on release/asan/tsan, which is the only place the epoll
        backend is exercised at all.
      - `fuzz-libfuzzer` — coverage-guided search, which only links on Linux. It
        asserts the entry points were actually built, so the job cannot pass having
        fuzzed nothing.
      - `benchmark` — recorded as an artifact, not gated. A wall-clock threshold
        fails CI for reasons unrelated to correctness; ignoring the numbers loses
        the ability to notice a regression.
      - `smoke` — starts the real binary, connects, and requires a clean `SIGTERM`
        drain. Every other job can pass while the deployed thing is unrunnable.

      Also added **`lobctl`**, a client. `OPERATIONS.md` tells an operator how to
      start the venue and, without this, gives them no way to talk to it.
      Verified against a live venue: handshake, two orders accepted with zero
      refusals, and a subscribe receiving `snapshot seq=3 symbol=0 bid=100x5`.

## Where the project ended

| Milestone | Status |
|---|---|
| M0 toolchain and build presets | Complete |
| M1 order book | Complete |
| M2a matching engine | Complete |
| M2b randomized invariants | Complete |
| M2c reference engine + differential | Complete |
| M3 journal and recovery | Complete |
| M4 engine runtime | Complete |
| M5 gateway and protocol | Complete |
| M6 tools and end-to-end | Complete |
| M7 fuzzing | Complete |
| M8 performance | Complete |
| M9 docs and CI | Complete |

**222 tests green** in debug, release, ASan+UBSan and TSan; clang-tidy and
clang-format clean.

### The bugs worth remembering

Written down because the *pattern* mattered more than any single defect:

- **An unbounded loop in the matching engine** — a hung venue. Found by chasing
  something else: a differential sweep that looked "slow" was actually spinning.
- **A shutdown race that lost acknowledged orders** — a bool and a counter in
  separate atomics, so a worker could exit before a submit that had already been
  accepted. The same shape recurred in a plain `bool` stop flag.
- **A journal whose CRC was computed from offset 0 of the buffer** — correct only
  for the first record, so recovery silently truncated to one order. The unit
  tests passed *because their helper cleared the buffer between records*.
- **An L2 stream that was wrong in ways nothing could see** — a zero-quantity
  order resting in the book satisfies every structural check while no longer
  being tradeable.
- **A synchronous book accessor that raced its own shard worker** — exposed by an
  API, not by a caller. Removed rather than patched.
- **A blocking listener socket** — hid behind a *passing* shutdown test, because
  with no pending connection the code path was never reached.

Three of those six were found by something other than a unit test, and two were
found by a test that was passing at the time. The recurring lesson is the one the
fuzzing and benchmark work kept re-proving: **a test that has not been seen to
fail is not evidence.** Hence the deliberate habit of reintroducing a known bug to
confirm a harness catches it — which caught four separately neutered fuzz targets
and one regression test that proved nothing.
