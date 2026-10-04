// The venue: everything wired together.
//
// This is where the pieces meet -- codec, session state machine, sharded engine,
// market data publisher, reactor -- and where the threading is settled:
//
//   acceptor thread          one, owns the reactor and every socket
//   shard worker threads     one per shard, own their Engine exclusively
//
// Requests flow acceptor -> shard through the SPSC request rings. Events flow
// back shard -> acceptor through the SPSC event rings. There is no other shared
// mutable state and no lock in the path: each engine has one writer, and the
// rings carry the ordering. That is the whole reason the runtime is shaped the
// way it is.
//
// Two rules the shape of this class is built around, both learned the hard way:
//
//   - **A gateway never reads a shard's book.** The worker owns it and is
//     mutating it. A subscription is answered by the shard itself and comes back
//     as a payload over a ring.
//   - **Nothing mutates the connection map while iterating it.** Closing a
//     connection erases it, and doing that inside a range-for over the same map
//     is undefined behaviour that presents as a spin, not as a crash.
//
// Shutdown is explicit and is the venue's most-tested property: run() owns its
// exit condition, stop() sets it, and the destructor joins.

#pragma once

#include "gateway/market_data.hpp"
#include "gateway/reactor.hpp"
#include "gateway/session.hpp"
#include "gateway/write_queue.hpp"
#include "journal/journal.hpp"
#include "runtime/runtime.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lob {

struct VenueConfig {
  /// Symbols, in global order, so SymbolId is the index into this list.
  std::vector<SymbolConfig> symbols;
  std::size_t shards = 2;
  SessionConfig session{};
  MarketDataConfig market_data{};
  /// Where to journal accepted requests. Empty means no journal, which is only
  /// sensible for a throwaway process: without it a restart loses everything.
  std::string journal_path;
  /// Rebuild the book by replaying `journal_path` before serving. The replay reads
  /// the file *before* the writer truncates it, so recovery and appending compose.
  bool recover_from_journal = false;
};

/// One live connection: its socket, its session state, and its market data state.
///
/// Inherits Connection rather than holding one, so the reactor can hand back a
/// `Connection&` with no cast. A reinterpret_cast from a base to a derived would
/// work, and would be a latent disaster the moment the two were reordered.
struct VenueConnection : Connection {
  protocol::FrameReader reader;
  Session session;
  WriteQueue outbox;
  /// Subscriptions awaiting a snapshot answer from their shard.
  std::vector<SymbolId> pending_subscriptions;
};

/// Counters an operator would actually watch.
///
/// Atomic because they are written by the acceptor thread and read by anything
/// watching the venue -- a metrics endpoint, a test, an operator at a terminal.
/// Plain uint64_t there is a data race that TSan reports and that produces
/// arbitrarily stale or torn counts in production.
struct VenueStats {
  std::atomic<std::uint64_t> connections_accepted{0};
  std::atomic<std::uint64_t> frames_decoded{0};
  std::atomic<std::uint64_t> requests_submitted{0};
  std::atomic<std::uint64_t> requests_refused{0};
  std::atomic<std::uint64_t> protocol_errors{0};
  std::atomic<std::uint64_t> subscriptions{0};
  std::atomic<std::uint64_t> slow_consumers_dropped{0};
};

class Venue : public ReactorHandler {
 public:
  explicit Venue(VenueConfig config);
  ~Venue() override;

  Venue(const Venue&) = delete;
  Venue& operator=(const Venue&) = delete;
  Venue(Venue&&) = delete;
  Venue& operator=(Venue&&) = delete;

  // --- ReactorHandler -----------------------------------------------------
  Connection* connection(std::uint64_t id) override;
  void on_accept(int fd) override;
  void on_readable(Connection& raw) override;
  void on_writable(Connection& connection) override;
  void on_closed(Connection& connection) override;

  /// Listen on `port`; 0 picks an ephemeral one. Returns the bound port, or 0 on
  /// failure. Tests use 0 so they never collide.
  std::uint16_t listen_on(std::uint16_t port);

  /// The acceptor loop. Exits when stop() is called, so the thread running it can
  /// always be joined.
  void run();

  /// Ask run() to return. Does not join; the destructor does that.
  void stop() noexcept {
    reactor_.stop();
  }

  /// Stop and join. Idempotent, and safe to call from a thread other than the one
  /// running run().
  void shutdown();

  [[nodiscard]] bool stopped() const noexcept {
    return reactor_.stopped();
  }
  [[nodiscard]] std::uint16_t port() const noexcept {
    return port_;
  }
  [[nodiscard]] const VenueStats& stats() const noexcept {
    return stats_;
  }
  /// Last known top of book for `symbol`; asks the owning shard for a fresh one as
  /// a side effect.
  ///
  /// Reads go through the shard rather than touching a book directly: the shard
  /// worker owns it and is mutating it, so a direct read is a data race however
  /// convenient it looks. The value returned is the most recent answer to have
  /// arrived, so it is always a real snapshot taken on the owning thread -- just
  /// not instantaneous. Tests poll it; so would a metrics endpoint.
  [[nodiscard]] TopOfBook top_of_book(SymbolId symbol);

  /// Ask the owning shard to report `symbol`'s touch. Non-blocking.
  void request_top_of_book(SymbolId symbol);

  /// Open the journal, optionally replaying it first. Called by listen_on().
  bool open_journal();

  /// Records appended to the journal. Zero when journalling is off.
  [[nodiscard]] std::uint64_t journal_records() const noexcept {
    return journal_.records_written();
  }

  /// False once a journal write has failed. The venue keeps serving, but recovery
  /// from this process is no longer trustworthy and the operator must know.
  [[nodiscard]] bool journal_healthy() const noexcept {
    return journal_.healthy();
  }

  [[nodiscard]] ShardedEngineHost& engine() noexcept {
    return engine_;
  }

  /// One accept-and-pump cycle, for tests that want to drive the loop themselves.
  void poll_once(int timeout_ms);

 private:
  void handle_message(VenueConnection& conn, const protocol::Inbound& message);
  bool submit_to_engine(const protocol::Inbound& message);
  /// Stamp and submit one request, leaving the result in scratch_record_.
  bool routed_body(const protocol::Inbound& message);
  void flush(VenueConnection& conn);
  void close_connection(VenueConnection& conn, std::string_view why);
  /// Submit any observations queued by request_top_of_book(). Owner thread only.
  void submit_pending_tops();

  void pump_snapshots();
  void pump_market_data();
  void accept_ready();

  VenueConfig config_;
  Sequencer sequencer_;
  ShardedEngineHost engine_;
  MarketDataPublisher publisher_;
  Reactor reactor_{*this};
  int listen_fd_ = -1;
  std::uint16_t port_ = 0;
  std::uint64_t next_id_ = 1;
  VenueStats stats_{};
  std::vector<Event> event_scratch_;
  std::vector<std::uint8_t> market_data_scratch_;
  /// Last snapshot answer per symbol, published by the acceptor thread and read by
  /// anyone watching, so it needs a lock rather than the single-writer assumption
  /// that covers the rest of the venue.
  mutable std::mutex tops_mutex_;
  std::unordered_map<std::uint32_t, TopOfBook> last_tops_;
  // Observations requested from outside the loop. The shard queues are SPSC with
  // a single producer -- the acceptor loop -- so top_of_book() must not submit
  // from whichever thread happened to call it. It queues here instead.
  std::mutex pending_tops_mutex_;
  std::vector<SymbolId> pending_tops_;
  /// Connections closed during a pump, removed after the iteration rather than
  /// during it.
  std::vector<VenueConnection*> to_close_;
  JournalWriter journal_;
  /// Reused so journalling an accepted request does not allocate per request.
  JournalRecord scratch_record_;
  std::unordered_map<std::uint64_t, VenueConnection> connections_;
};

}  // namespace lob