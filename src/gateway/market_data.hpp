// Market data publisher: snapshot-then-increments, per subscriber.
//
// The contract a market data feed has to keep, and the reason this class is
// more than a loop over events:
//
//   A subscriber that joins mid-stream must receive a snapshot of the book as of
//   one exact sequence, and then every increment *after* that sequence -- with
//   nothing before it and nothing twice.
//
// Getting that wrong does not crash and does not look obviously wrong on the
// wire: the client simply ends up with a subtly incorrect book. So the boundary
// is made explicit rather than emergent. Each subscriber records, per symbol,
// the highest engine sequence it has been sent, stamped at subscribe time. An
// increment is forwarded only when its sequence is strictly greater, and the
// recorded value advances as it goes. No gaps are possible because the engine's
// sequences are dense; no duplicates because the comparison is strict; no stale
// pre-snapshot increment because the snapshot's sequence seeds the record.
//
// Slow-consumer policy: bounded outboxes, and a subscriber that exceeds its cap
// is disconnected rather than silently skipped. Dropping updates would leave the
// client with a book it believes is current and is not, which is worse than
// telling it to reconnect and start from a fresh snapshot. There is no
// drop-oldest / drop-newest option here on purpose -- for an order book, every
// one of those silently corrupts state.

#pragma once

#include "core/engine.hpp"
#include "protocol/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace lob {

/// Identifies a connected client for subscription purposes.
struct SessionId {
  std::uint64_t value = 0;
  friend bool operator==(const SessionId& a, const SessionId& b) {
    return a.value == b.value;
  }
};

struct MarketDataConfig {
  /// Per-subscriber outbox cap in bytes. Exceeding it disconnects the
  /// subscriber; see the file comment for why there is no silent-drop option.
  std::size_t max_outbox_bytes = 4UL * 1024UL * 1024UL;
  /// Refuse a new subscription once this many symbols are already tracked.
  std::size_t max_symbols_per_session = 4096;
};

/// A snapshot taken from a live book. `sequence` is the engine sequence current
/// at the moment the top of book was read -- read it *after* the book, so it is
/// never stale relative to the snapshot it accompanies.
[[nodiscard]] protocol::SnapshotPayload make_snapshot(const Book& book, SymbolId symbol,
                                                      std::uint64_t sequence) noexcept;

/// Build an increment payload from an engine BookUpdate event. Returns false for
/// any other event type: only level changes belong on the feed, and forwarding
/// everything would be a firehose the client cannot use.
[[nodiscard]] bool as_increment(const Event& event, protocol::IncrementPayload& out) noexcept;

class MarketDataPublisher {
 public:
  explicit MarketDataPublisher(MarketDataConfig config = {}) : config_(config) {}

  /// Start delivering `symbol` to `session`.
  ///
  /// Queues a snapshot built from `book` and stamped with `sequence`, then marks
  /// the session as having seen everything up to and including it. `sequence`
  /// must be read from the engine after reading `book`.
  ///
  /// Returns false if the subscription was refused (already subscribed, at the
  /// symbol cap, or the subscriber was already dropped as slow).
  bool subscribe(SessionId session, SymbolId symbol, const Book& book, std::uint64_t sequence);

  /// Stop delivering `symbol`. Returns false if it was not subscribed.
  bool unsubscribe(SessionId session, SymbolId symbol);

  /// Forget a session entirely. Called when a connection closes.
  void remove(SessionId session);

  /// Fan a BookUpdate out to every subscriber of its symbol.
  void publish(const Event& event);

  /// Bytes waiting for `session`, to be written to its socket.
  [[nodiscard]] const std::vector<std::uint8_t>& outbox(SessionId session) const;

  /// True once `session` exceeded its outbox cap and must be disconnected. The
  /// publisher refuses to keep feeding it.
  [[nodiscard]] bool is_dropped(SessionId session) const;

  [[nodiscard]] bool is_subscribed(SessionId session, SymbolId symbol) const;

  /// Clear and return the outbox, as the reactor does after a successful write.
  void take(SessionId session, std::vector<std::uint8_t>& out);

  [[nodiscard]] std::size_t subscriber_count() const noexcept {
    return subscribers_.size();
  }

  /// Sub-scribers whose book state should be reconstructed from `book` plus the
  /// increments already delivered. Exposed for tests that verify the no-gap
  /// property end to end.
  [[nodiscard]] std::uint64_t delivered_through(SessionId session, SymbolId symbol) const;

 private:
  struct Subscriber {
    /// Highest engine sequence delivered for each subscribed symbol. Seeded at
    /// subscribe time -- this is the snapshot/increment boundary.
    std::map<std::uint32_t, std::uint64_t> delivered_through;
    std::vector<std::uint8_t> outbox;
    bool dropped = false;
  };

  Subscriber* find(SessionId session) noexcept;
  const Subscriber* find(SessionId session) const noexcept;

  /// Queue bytes, applying the slow-consumer policy. Returns false if the
  /// subscriber is now dropped.
  bool enqueue(Subscriber& sub, protocol::MessageType type, const protocol::Inbound& message);

  MarketDataConfig config_;
  std::map<std::uint64_t, Subscriber> subscribers_;
};

}  // namespace lob