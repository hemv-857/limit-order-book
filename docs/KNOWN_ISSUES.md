# Known issues

Open defects that are known and not yet fixed. Each one is something CI can see
but the code does not yet do the right thing about.

## ThreadSanitizer data race on the shard request queue

**Status:** open. Fails `linux (gcc tsan)` only.

TSan reports a race at `src/runtime/spsc_ring.hpp:79`, which is the slot read in
`SpscRing::pop()`:

- read of size 8 by the shard worker, `ShardedEngineHost::run_shard`
  (`src/runtime/runtime.cpp:93`)
- previous write of size 8 by the main thread

`push()` publishes the slot with `head_.store(release)` and `pop()` acquires
`head_`, which is the standard SPSC pairing and looks correct on inspection. TSan
does not see the happens-before edge, so either the pairing is wrong somewhere
in practice, or a second thread is reaching into a ring it does not own -- most
likely a `drain()` or teardown path that lets the main thread touch a queue, or
lets a worker outlive the object, while it is still being read.

Not yet narrowed to a specific call site. The reproduction is CI-only so far; the
TSan e2e run is clean on macOS.

## Durability

Journal rotation, an fsync policy, and snapshots are not implemented. The writer
flushes at 64 KiB and `fsync()`s on close, so a recovered venue is correct after
a *graceful* restart and after a torn write, but a process killed between flushes
loses the tail of the log. This is a known limitation, not a crash-safe design.

## Not built

- `replay` tool: no way to inspect or replay a journal offline.
- `loadgen`: no sustained-load driver, so there is no soak result.