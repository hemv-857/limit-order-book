# Operations

How to build, run, test and reason about this venue day to day.

## Prerequisites

CMake 3.22+ and Ninja. Everything else is vendored or installed into the repo:

```bash
./scripts/bootstrap.sh      # creates .venv with ninja, clang-format, clang-tidy
source scripts/env.sh       # put .venv/bin on PATH for this shell
```

`scripts/env.sh` must be sourced in every shell you build from. Nothing is
installed system-wide.

## Build presets

| Preset | Purpose |
|---|---|
| `debug` | assertions, no optimisation — the default for development |
| `release` | optimised — **the only preset to benchmark in** |
| `asan-ubsan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |
| `coverage` | gcov instrumentation |
| `release-portable` | no `-march=native`, for comparing machines |

```bash
cmake --preset debug && cmake --build --preset debug -j
cmake --preset release && cmake --build --preset release -j
```

## Test

```bash
ctest --preset debug --output-on-failure
```

Warnings are errors everywhere (`-Werror`). `clang-tidy` and `clang-format` are
clean and are enforced:

```bash
./scripts/fmt.sh     # format in place
./scripts/tidy.sh    # report
```

A failing test should name the failing case, not the binary. Every test target
registers its cases individually via `gtest_discover_tests`, so a failure in a
600-case run points at one name.

### What each preset is for

**Run `asan-ubsan` and `tsan` before believing anything is done.** They find
different classes of bug and neither substitutes for the other:

- `asan-ubsan` finds out-of-bounds, use-after-free, leaks and UB. Most of the
  protocol's fuzz value only materialises here — a bounds bug in a decoder is
  invisible in a plain build.
- `tsan` finds data races. Several of the worst bugs found in this project were
  races: a shutdown flag read by one thread and written by another, counters read
  cross-thread, and a synchronous book accessor that raced the shard worker that
  owned the book.

The full suite takes roughly 7 minutes under TSan. Budget for it.

## Run the venue

```bash
./build/release/bin/venue --port 0 --shards 2 --symbols 3
```

`--port 0` binds an ephemeral port, which is what the tests use so they never
collide. On startup it prints the bound port; on `SIGINT`/`SIGTERM` it drains and
prints counters:

```
venue: listening on 127.0.0.1:52805, 2 shard(s), 3 symbol(s)
venue: stopped. connections=1 frames=42 submitted=40 refused=0 protocol_errors=1 subscriptions=0
```

**Shutdown is graceful and must stay that way.** A request accepted before the
stop is applied before the process exits. The in-flight count and the draining flag
live in the same atomic word per shard for exactly this reason: with them
separate there is a window where a worker sees zero in flight, exits, and a
concurrent submit pushes into a queue nobody will ever read.

If you add a run loop, give it an exit condition and a test that joins it.

## Benchmarks

```bash
cmake --preset release && ./build/release/bin/bench_lob
```

Release only — a debug build measures the wrong thing. Results and the
before/after for the one measured optimisation are in `PLAN.md` §M8.

Two rules keep these numbers meaningful, and both were learned the hard way:

- **Fixtures go outside the timed region.** An engine that allocates during matching
  otherwise looks like it allocates, and a reader cannot tell that from the code.
- **A benchmark must assert it did the work.** `BM_JournalReplayRecords` once
  reported 560 GiB/s — four orders of magnitude faster than the machine's memory
  bandwidth — because replay was returning after the first record. If you write a
  benchmark, make it fail when it stops exercising anything.

## Fuzzing

```bash
./scripts/fuzz.sh                # ASan+UBSan, portable driver
./scripts/fuzz.sh release        # release preset
```

Picks libFuzzer where its runtime links, and the portable corpus + mutation driver
everywhere else — which is every default toolchain on macOS. The portable driver
runs under sanitizers, so it is a real safety net rather than a consolation prize.

The engine target asserts the **full** invariant set after **every** operation.
The bugs worth finding are self-consistent-but-wrong states — a zero-quantity order
resting in the book, a crossed book — and a harness that only looks for crashes
cannot see them.

**Verify a fuzz change has teeth.** Reintroduce a known bug and confirm the target
fails; restore and confirm it passes. Four separate times during M7 the target was
silently exploring nothing — generating only invalid orders, running one operation
per input, never building crossing liquidity — and it passed the whole time.

## Recovery

The engine is deterministic, so the journal records **requests**, not book state.
Recovery replays the log into a fresh engine.

```
LOBJ | kind u8 | length u32 | payload | crc32c
```

`crc32c` covers kind, length and payload. A crash mid-append leaves a torn tail,
which is expected: replay stops at the first record failing its magic, length or
CRC check, reports `truncated`, and keeps every intact record before it.

The load-bearing property is that replaying a log into a fresh engine yields an
identical `state_hash`. If that ever fails, recovery is silently producing a
different venue.

Not implemented: segment rotation, fsync policy, and snapshots. Recovery currently
replays the whole log from genesis — fine as a correctness baseline, not a restart
SLA.

## Troubleshooting

**A test hangs.** Almost always a shutdown path. A run loop whose stop flag nothing
reads cannot be joined. Sample the process:

```bash
./build/debug/bin/test_foo &   # then, once wedged
/usr/bin/sample <pid> 1
```

This found three separate hangs during M6, each with a different cause.

**High CPU with no load.** Check for a spin loop. An idle shard worker once burned
100% of a core in a `yield` loop; two shards saturated the machine and starved the
threads they were waiting for. Shard workers now spin briefly then sleep, with the
spin counter reset on every successful pop.

**Tidy or the build complains about a `reinterpret_cast` / vararg.** Some are
genuinely unavoidable — the socket API takes `struct sockaddr*`, and `htons` has no
C++ overload. Those carry a file-scoped `NOLINTBEGIN` with the reason. Line-local
suppressions are fragile: `clang-format` reflows code and detaches them.

**A test passes but proves nothing.** Check whether it asserts on what it claims.
Two tests in this project were green for a long time while hiding a journal bug,
because a convenient helper cleared the buffer between records. If a test wraps a
helper, check what the helper assumes.

## Repository layout

```
src/core       the matching engine: book, engine, validation, events, risk, STP
src/journal    write-ahead journal, CRC, replay
src/runtime    SPSC ring, sequencer, sharded engine host
src/protocol   wire codec and framing
src/gateway    sessions, market data, write queue, reactor
src/venue      all of the above wired into a process
src/util       shared CRC
bench          Google Benchmark suite
fuzz           fuzz targets and libFuzzer entry points
tests          unit, property, differential, journal, runtime, protocol, gateway, e2e, fuzz
tools          the venue binary
docs           this, plus PLAN, ARCHITECTURE, PROTOCOL, MATCHING_RULES, DECISIONS, FINAL_REPORT
```
