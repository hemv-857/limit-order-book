# Known issues

Open defects and known limitations. Each is something CI can see but the code
does not yet do the right thing about.

## Snapshots are not implemented

**Status:** open. This is the one remaining item from the original gap list.

Recovery works by replaying the journal from the beginning, so a venue that has
run for a long time replays its entire history on every restart. Correct, and
unbounded in time as the log grows.

`JournalDurability::segment_bytes` bounds each *file*, not the total work, and
`replay --verify` on a real soak log (80,000 records, 6.8 MB across two
segments) rebuilt the book in well under a second -- so this is not yet a
practical problem. It becomes one when the log reaches millions of records.

The design is settled enough to implement, and the pieces are verified to exist:

- **Enumeration is possible.** `Book` exposes `next_occupied_index()` and
  `level_at()`; `Level` exposes `head`; `Order` exposes `next` and `level`. So
  every resting order is reachable by walking levels and then each level's
  queue. This was checked against the headers, not assumed.
- **The format can reuse the journal.** A snapshot is a list of resting orders,
  which is exactly a sequence of `NewOrder` records, so `encode_record` and
  `replay` are already the right serialisation and are already tested.
- **The cut-off point must be recorded.** A snapshot has to carry the number of
  journal records it covers (`JournalWriter::records_written()`), and recovery
  has to skip exactly that many. Replaying the whole log on top of a snapshot
  would re-add orders that were cancelled after it was taken.

What is missing is the code: a walker that emits resting orders, the snapshot
header carrying the record count, venue wiring to write one and to prefer it
over a full replay, and tests for the cut-off boundary. That is a feature, not a
fix, and it wants its own pass rather than being bolted on at the end of a
session where it cannot be properly tested.

## Not measured

- **Recovery has not been tested against a machine-level crash.** `kill -9` was
  measured: 2,000 orders sent, 2,000 in the journal, no torn tail. But that is a
  weaker result than it looks, and the distinction matters:

  `kill -9` kills the process, not the kernel, so everything already written
  survives in the OS page cache. Only records still sitting in the writer's
  userspace buffer are lost, which the 64 KiB flush threshold bounds -- at ~85
  bytes per record, under 800 records. What `fsync` actually protects against is
  power loss or a kernel panic, where unwritten page-cache data is gone. That
  cannot be simulated here, so the durability claim rests on the code rather
  than on an observation.

- **The soak is short.** CI runs 15 seconds at 500/s per connection
  (~30,000 orders), which is enough to catch a throughput collapse, a journal
  regression or a book that ends up crossed. It is not a long-running leak or
  latency-drift test; nothing measures how behaviour changes over hours.

## Deliberate limits

- **The venue sends no per-order acknowledgement.** `loadgen` therefore measures
  delivery, not acceptance, and says so in its output. Confirming what the venue
  accepted means replaying the journal and inspecting the book.
- **Replay stops at the first missing segment.** A gap means records are lost, so
  reading past it would rebuild a book that is quietly short orders. A torn
  *tail* is safe and is tolerated; a gap is not.
