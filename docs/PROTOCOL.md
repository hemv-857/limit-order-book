# Wire protocol

Normative. Version 1. The implementation is `src/protocol/codec.{hpp,cpp}`; the
tests in `tests/protocol` and the end-to-end tests in `tests/e2e` are the
executable form of everything below.

## Framing

Every message is one frame. All integers are **little endian**, matching the
journal's on-disk format so one framing shape and one CRC routine serve both.

```
+--------+---------+---------+---------+---------+-----------+---------+
| "LO"   | version | type    | length  | payload | crc32c    |
| 2 B    | 1 B     | 1 B     | 4 B     | length B| 4 B       |
+--------+---------+---------+---------+---------+-----------+---------+
   magic                                      |<-- CRC covers -->|
```

`length` is the payload length in bytes, capped at **65536** (`kMaxPayload`).

`crc32c` is CRC-32C (Castagnoli) over **version, type, length and payload** —
everything after the magic.

### Why each of those is there

- **The magic** lets a stream resynchronise and lets a listener reject a non-venue
  connection immediately.
- **The version** makes an incompatible peer fail loudly at the first frame rather
  than being misread as garbage further in.
- **The length cap** means a hostile or corrupt length field cannot make the
  gateway allocate. An absurd length is rejected as `length_too_large` *before* any
  buffer is sized.
- **The CRC is verified before the payload is parsed**, so a corrupt frame is never
  handed to the field decoder at all.

### Partial reads are normal

A TCP read can land anywhere: mid-header, mid-payload, or spanning several frames.
`FrameReader` buffers a partial tail until the rest arrives. A client must not
assume one `send` is one frame, or that one frame arrives in one read.

A client that pipelines several frames into one write is fine. A server that
concatenates frames must not include its own framing.

## Message types

| Value | Name | Direction |
|---|---|---|
| 1 | `hello` | client → server |
| 2 | `authenticate` | client → server |
| 3 | `new_order` | client → server |
| 4 | `cancel` | client → server |
| 5 | `replace` | client → server |
| 6 | `mass_cancel` | client → server |
| 7 | `subscribe` | client → server |
| 8 | `heartbeat` | both |
| 9 | `goodbye` | client → server |
| 10 | `market_data_snapshot` | server → client |
| 11 | `market_data_increment` | server → client |

Sending a server-only message to the server is a protocol error and closes the
connection.

## Payloads

### `hello` (1)

```
session_id : string
```

A stable identifier for the connection. Reused by `authenticate`.

### `authenticate` (2)

```
session_id          : string   (must equal the hello's)
participant_token   : string
```

The server resolves the token to a participant. See **Participant identity**
below — the client does not get to choose.

### `new_order` (3)

```
order_id        : u64
participant     : u64
symbol          : u32
side            : u8      0 = buy, 1 = sell
order_type      : u8      0 limit, 1 market, 2 stop, 3 stop-limit
time_in_force   : u8      0 GTC, 1 day, 2 IOC, 3 FOK
price           : i64     limit price; ignored for market
trigger_price   : i64     stop trigger; only for stop / stop-limit
quantity        : i64
display_qty     : i64     iceberg slice; 0 = fully displayed
post_only       : u8      0 or 1
```

**`sequence` and `timestamp` are deliberately absent.** They are assigned by the
server's sequencer. A client-supplied sequence would let any participant choose its
ordering relative to every other participant, which is not something a venue hands
out.

### `cancel` (4)

```
order_id    : u64
participant : u64
symbol      : u32
```

### `replace` (5)

```
order_id     : u64
participant  : u64
symbol       : u32
new_price    : i64   0 keeps the current price
new_quantity : i64   the new TOTAL quantity; 0 cancels
```

`new_quantity` is a total, not a delta. See `MATCHING_RULES.md` §8 for why.

### `mass_cancel` (6)

```
participant : u64
```

Cancels every one of that participant's resting orders on the symbol implied by the
connection's subscription set.

### `subscribe` (7)

```
symbol : u32
```

The server replies with a `market_data_snapshot` for that symbol, followed by
`market_data_increment` for every change after it. See **Market data** below.

### `heartbeat` (8) / `goodbye` (9)

Empty. A heartbeat proves liveness; it need not be the only traffic — any message
counts.

### `market_data_snapshot` (10) — server → client

```
sequence       : u64    engine sequence this snapshot reflects
symbol         : u32
has_bid        : u8
best_bid       : i64
best_bid_qty   : i64
best_bid_orders: u32
has_ask        : u8
best_ask       : i64
best_ask_qty   : i64
best_ask_orders: u32
```

### `market_data_increment` (11) — server → client

```
sequence : u64
symbol   : u32
side     : u8
action   : u8   0 added, 1 changed, 2 removed
price    : i64
quantity : i64
```

`action` is the engine's own `UpdateAction`, echoed rather than re-derived, so the
feed is consistent with the engine's L2 stream.

## Session lifecycle

```
  connected --hello--> awaiting_auth --authenticate--> ready --goodbye--> closed
       |                      |                            |
       +----------------------+----------------------------+--> closed
                  (any timeout, or protocol error)
```

Two rules are enforced by the server, not by convention:

- **No trading message is accepted before `ready`.** Every refusal closes the
  connection, so there is no path that reaches the book unauthenticated.
- **The participant id is the session's, never the client's.** A request naming a
  different participant is a protocol error, not something the server quietly
  rewrites — rewriting would hide an impersonation attempt behind a silently
  accepted order.

Timeouts: `hello_timeout` (default 5 s), `auth_timeout` (default 5 s),
`heartbeat_timeout` (default 15 s). A ready session is dropped once it has been
silent past the heartbeat timeout.

## Market data

A subscriber receives a snapshot of one symbol as of one exact sequence, and then
every increment after it. **Nothing before the snapshot, and nothing twice.**

`sequence` in the snapshot is the boundary. The server records it against the
subscription and forwards an increment only when its sequence is strictly greater.
Getting this wrong does not crash and is not visibly wrong on the wire: the client
simply ends up with a subtly incorrect book, which is the worst failure a feed can
have.

Both sequence spaces are the **engine's** event counter. They are not the gateway
sequencer's numbers, and not wall-clock time.

A subscriber that exceeds its outbox cap is **disconnected**, not silently
skipped. Dropping updates would leave the client holding a book it believes is
current and is not; reconnecting gets a fresh snapshot.

## Errors

Decode failures are distinguished, because a protocol error means a buggy or
hostile client and a CRC failure means a network problem, and they should not be
alerted on identically:

| Code | Meaning |
|---|---|
| `bad_magic` | not a venue frame |
| `bad_version` | incompatible protocol version |
| `unknown_type` | undefined message type |
| `length_too_large` | payload exceeds `kMaxPayload` |
| `crc_mismatch` | corrupt frame; not parsed |
| `truncated` | **not an error** — the rest has not arrived yet |
| `malformed_payload` | wrong length for the type, or trailing bytes |

Trailing bytes in a payload are **rejected, not ignored**. If sender and receiver
disagree about a layout, ignoring the tail is how version skew becomes a silent
field-level misread.

## Security notes

- There is **no transport security here.** The protocol is cleartext and does no
  authentication of its own; `authenticate` carries a token that must be
  transported over a channel you trust. Terminate TLS in front of the venue.
- The length cap and CRC-check-before-parse are what keep a hostile peer from
  turning the gateway into an allocator or a parser bug. Both are fuzzed.
- A client that cannot authenticate, or that sends a malformed frame, is closed
  rather than tolerated. There is no penalty path, no partial acceptance, and no
  silent coercion of a bad field.
