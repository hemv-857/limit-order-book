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

- **No long-running soak.** `loadgen` exists and one 20-second run is recorded in
  the git history, but nothing runs it in CI, so a throughput or latency
  regression would not be caught. A CI job that runs loadgen against the venue
  and asserts on orders actually resting in the book would close this.
- **Recovery has only been exercised from graceful shutdown and from a
  deliberately torn tail.** A real `kill -9` mid-write has not been tested
  against the fsync cadence, so the "at most `fsync_every_records` lost" claim
  is reasoned from the code rather than measured.

## Deliberate limits

- **The venue sends no per-order acknowledgement.** `loadgen` therefore measures
  delivery, not acceptance, and says so in its output. Confirming what the venue
  accepted means replaying the journal and inspecting the book.
- **Replay stops at the first missing segment.** A gap means records are lost, so
  reading past it would rebuild a book that is quietly short orders. A torn
  *tail* is safe and is tolerated; a gap is not.
