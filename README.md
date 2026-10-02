# limit-order-book

A deterministic limit order book and matching engine in C++20, built to run as a
real exchange venue: correctness first, then determinism, then performance.

**Status: the matching core is complete and verified. The journal, runtime,
gateway, tooling and fuzzing described in the original specification are not
built yet.** See [docs/FINAL_REPORT.md](docs/FINAL_REPORT.md) for exactly what
exists, what was measured, and what is missing. Nothing in this repository is
aspirational — the test counts and benchmark numbers below were produced by
running the code on the machine described under [Measurements](#measurements).

---

## What is built

`src/core` — the deterministic matching engine. No I/O, no syscalls, no threads,
no floating point, and zero heap allocation on the hot path.

- **Order types:** Limit, Market, Stop, StopLimit
- **Time in force:** Day, GTC, IOC, FOK
- **Post-only:** reject or slide (slide direction derived from the side)
- **Iceberg/reserve:** displayed slice, with priority loss on replenish
- **Cancel/replace:** quantity decrease keeps priority; price change or quantity
  increase loses it
- **Self-trade prevention:** cancel-oldest, cancel-newest, cancel-both,
  decrement-and-cancel
- **Pre-trade risk:** 27 distinct reject codes in a fixed check order, plus
  per-participant rate limiting on an injected clock
- **Stops:** rest outside the book until triggered, with cascades drained from a
  work list rather than recursing
- **Data structures:** flat price grid + occupancy bitmap with O(1) top of book;
  fixed-capacity order arena with index-based intrusive FIFO links; open-addressing
  id index that never rehashes and uses backward-shift deletion
- **Determinism:** identical input produces a byte-identical event stream
- **Verification:** a state hash, nine structural invariants, and a randomised
  invariant probe

Every rule is specified in [docs/MATCHING_RULES.md](docs/MATCHING_RULES.md),
which is the contract the tests verify. Design rationale is in
[docs/DECISIONS.md](docs/DECISIONS.md) and
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Quickstart

Requires CMake 3.22+ and a C++20 compiler (GCC 12+ or Clang 15+). No network
access is needed after the first fetch of GoogleTest and Google Benchmark.

```bash
git clone <repo> && cd limit-order-book

# Installs ninja, clang-format and clang-tidy into ./.venv — repo-local, nothing
# outside this directory is modified.
./scripts/bootstrap.sh
source scripts/env.sh

cmake --preset release
cmake --build --preset release -j
```

### Presets

| Preset | Purpose |
|---|---|
| `debug` | `-O0 -g3`, invariants checked after every operation |
| `release` | `-O3 -march=native` |
| `asan-ubsan` | Address + UndefinedBehaviour sanitizers, `-fno-sanitize-recover` |
| `tsan` | ThreadSanitizer |
| `coverage` | gcov instrumentation, filtered to `src/core` |
| `release-portable` | `-O3` without `-march=native`, for reproducible binaries |

### Test

```bash
./scripts/test.sh debug        # or release / asan-ubsan / tsan / coverage
```

### Long randomised invariant run

```bash
LOB_PROBE_OPS=20000000 ctest --preset release -R invariant
```

### Lint and format

```bash
./scripts/fmt.sh --check        # what CI runs
./scripts/fmt.sh                # apply
./scripts/tidy.sh               # clang-tidy over src/ and tools/
```

---

## Measurements

Taken on the machine below. Numbers are from running the code, not estimated.

**Hardware and toolchain**

| | |
|---|---|
| CPU | Apple M4, 10 cores |
| Memory | 16 GB |
| OS | macOS 27.0.1 (Darwin 24.3.0 kernel, arm64) |
| Compiler | Apple Clang 21.0.0, `-std=c++20 -O3 -DNDEBUG -march=native` |
| Note | This is **not** a Linux host: no GCC, no `perf`, no `epoll`. See below. |

### Test suite

| Preset | Result |
|---|---|
| `release` | **100 / 100 passed** |
| `asan-ubsan` | **100 / 100 passed** |
| `tsan` | **100 / 100 passed** |
| `clang-tidy` | **0 findings** in first-party code |
| `clang-format --check` | clean |

### Randomised invariant probe

20,000,000 operations of a seeded mixed stream (all order types, all TIFs,
icebergs, post-only, cancel, replace, mass cancel, session end) with all nine
invariants re-checked every 1024 operations:

```
probe: 20000000 ops, state_hash=6505e777fe8b289e
[       OK ] InvariantProbe.MixedStreamKeepsEveryInvariant (256801 ms)
```

Cancel and replace name the order's **actual** owner, so the removal paths are
genuinely exercised. An earlier version picked a random participant, which meant
almost every cancel was rejected as `UnknownOrder` — and a bug that spliced a
stop's queue links into a liquidity level survived 20,000,000 operations
unnoticed. See `docs/FINAL_REPORT.md`.

### Throughput

A single-threaded mixed stream — 55 % new orders (of which 20 % are market
orders that sweep), 30 % cancels, 15 % replaces — with invariants checked every
4096 operations. Five million operations, three consecutive runs:

| Run | Ops | Elapsed | Rate |
|---|---|---|---|
| 1 | 5,000,000 | 2.849 s | **1,755,028 ops/s** |
| 2 | 5,000,000 | 2.857 s | **1,750,217 ops/s** |
| 3 | 5,000,000 | 2.863 s | **1,746,581 ops/s** |

The state hash was identical across all three runs, which is the determinism
property holding under load rather than just in a unit test.

**Against the specification's target of ≥ 2 M orders/sec: not met — 1.75 M
ops/s.** This is a single smoke measurement, not a tuned benchmark suite, and it
includes ~20 % market orders that sweep multiple price levels plus periodic full
invariant checking, all of which the specification's "mixed realistic workload"
did not define precisely. See [docs/FINAL_REPORT.md](docs/FINAL_REPORT.md) for
the analysis of where the time goes and what has *not* been done about it.

### Not measured

Latency percentiles (p50/p99/p99.9), allocation counts under the benchmark
harness, flamegraphs and flamegraph-derived optimisation. The Google Benchmark
suite and the HDR-style histogram are not written yet, so no such numbers are
quoted. Zero allocation *is* proven, but by an instrumented test rather than a
benchmark — see below.

### Zero allocation is proven, not asserted

`tests/unit/no_alloc_test.cpp` replaces global `operator new`/`delete` and
asserts zero allocations across arena churn, book maintenance, level moves and a
full id-index load. `NoAlloc.GuardActuallyObservesAllocations` first proves the
replacement is really counting, so the other tests cannot pass vacuously.

---

## Repository layout

```
src/core/       the matching engine — STL only, no I/O
src/util/       endian helpers
tests/unit/     order book, engine semantics, zero-allocation proof
tests/property/ randomised invariant probe
bench/          Google Benchmark target (smoke test only so far)
docs/           PLAN, ARCHITECTURE, MATCHING_RULES, DECISIONS, FINAL_REPORT
scripts/        bootstrap, build, test, fmt, tidy
cmake/          warnings, sanitizers, dependencies, coverage
```

## Known limitations

- **macOS host.** No GCC and no `perf` here; the GCC leg and `perf` profiling
  cannot be validated locally. See `docs/PLAN.md` §7 for the full deviation list.
- **No journal or recovery.** Nothing is persisted; there is no crash recovery,
  snapshot or state-hash replay, though `state_hash()` exists to support one.
- **No gateway or protocol.** No wire format, no sockets, no market data feed.
- **No tools.** No `lobctl`, `loadgen`, `replay` or `bookviz`.
- **No differential test against a reference engine.** Unit tests and the
  randomised invariant probe cover a great deal, but there is no second
  independent implementation to compare event streams against. This is the
  single highest-value gap.
- **No fuzzing, no coverage report, no CI.**
- **No latency percentiles.**

## Licence

MIT. See [LICENSE](LICENSE).