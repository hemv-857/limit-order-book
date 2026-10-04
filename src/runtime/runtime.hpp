// Sequencing and shard routing.
//
// One sequencer sits between the gateway and the shards. It does two jobs and
// deliberately no more:
//
//   1. stamp every request with a gap-free, globally increasing Sequence and a
//      matching Timestamp, so a total order over all accepted requests exists
//      even though the shards match independently and concurrently;
//   2. route each request to a shard by symbol, so all activity for one symbol
//      is handled by exactly one thread and needs no locking.
//
// It does not order anything itself beyond the stamp. Because shards run
// concurrently, a lower Sequence can be *applied* after a higher one in a
// different shard; the Sequence exists so that a reader can detect that, not so
// that application order is globally serialised.

#pragma once

#include "core/engine.hpp"
#include "runtime/spsc_ring.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <thread>
#include <variant>
#include <vector>

namespace lob {

/// Ask the shard that owns `symbol` to report its current touch.
///
/// A gateway cannot read the book itself: the shard worker owns it and is
/// mutating it. So a subscription is answered by the owning thread, which is the
/// only place the read is safe.
struct SubscribeRequest {
  Sequence seq{};
  Timestamp ts{};
  SymbolId symbol{};
  std::uint64_t session_id{};
};

/// A shard's answer to a SubscribeRequest, taken on the owning thread at a
/// definite sequence.
struct ShardSnapshot {
  SymbolId symbol{};
  TopOfBook book{};
  std::uint64_t sequence{};
  /// Echoes SubscribeRequest::session_id so the caller can tell a subscription from
  /// a one-off observation.
  std::uint64_t session_id{};
};

/// A request of any kind, as it crosses from a gateway thread to a shard thread.
using Envelope = std::variant<NewOrderRequest, CancelRequest, ReplaceRequest, MassCancelRequest,
                              SubscribeRequest>;

/// Stamps requests and routes them to a shard.
///
/// Thread-safe: gateways on several threads may stamp concurrently. Assigning
/// the sequence and the timestamp must happen under one atomic operation each so
/// that a reader can trust that Sequence N's timestamp is the Nth timestamp
/// observed, not merely that each counter increments.
class Sequencer {
 public:
  explicit Sequencer(std::size_t shard_count, std::int64_t start_ts = 0)
      : shard_count_(shard_count == 0 ? 1 : shard_count), ts_(start_ts) {}

  /// Gap-free, monotonically increasing. Never reused, never skips.
  [[nodiscard]] Sequence next_sequence() noexcept {
    return Sequence{seq_.fetch_add(1, std::memory_order_relaxed)};
  }

  /// Monotonically increasing, one per assigned sequence.
  [[nodiscard]] Timestamp next_timestamp() noexcept {
    return Timestamp{ts_.fetch_add(1, std::memory_order_relaxed)};
  }

  /// Which shard owns this symbol. All activity for one symbol lands on one
  /// shard, so no shard ever needs to see another's book.
  [[nodiscard]] std::size_t shard_for(SymbolId symbol) const noexcept {
    return static_cast<std::size_t>(symbol.value) % shard_count_;
  }

  [[nodiscard]] std::size_t shard_count() const noexcept {
    return shard_count_;
  }

 private:
  std::size_t shard_count_;
  std::atomic<std::uint64_t> seq_{1};
  std::atomic<std::int64_t> ts_;
};

/// One symbol's engine plus the thread that owns it.
struct Shard {
  std::size_t index = 0;
  std::unique_ptr<Engine> engine;
  std::thread worker;
};

/// Sharded, single-threaded-per-shard engine host.
///
/// Each shard owns its Engine exclusively and is driven by one thread, so the
/// engine itself needs no synchronisation and stays allocation-free and
/// deterministic. Shutdown is a graceful drain: `stop_accepting()` first closes
/// the door to new work, and only once every queue is empty does the worker
/// exit. Draining rather than aborting is what makes a restart not lose
/// acknowledged orders.
class ShardedEngineHost {
 public:
  /// Queue depth per shard; a power of two so the ring's index wrap is a mask.
  static constexpr std::size_t kQueueDepth = 1UL << 16;
  /// Event ring per shard. Sized so that a normal burst never comes close; an
  /// overflow is treated as a fault, not silently tolerated.
  static constexpr std::size_t kEventDepth = 1UL << 20;
  static constexpr std::size_t kSnapshotDepth = 1UL << 12;

  /// `symbols` is the venue's symbol list in *global* order, so SymbolId is the
  /// index into it. Each shard's Engine numbers its own symbols from zero, so
  /// the host keeps the global-to-local translation; routing on a global id
  /// without it silently addresses the wrong book.
  ShardedEngineHost(std::vector<SymbolConfig> symbols, std::size_t shard_count);
  ~ShardedEngineHost();

  ShardedEngineHost(const ShardedEngineHost&) = delete;
  ShardedEngineHost& operator=(const ShardedEngineHost&) = delete;
  ShardedEngineHost(ShardedEngineHost&&) = delete;
  ShardedEngineHost& operator=(ShardedEngineHost&&) = delete;

  /// Submit to the shard owning the symbol. Returns false once draining has
  /// started -- a rejected submit is never silently dropped.
  bool submit(const NewOrderRequest& request) noexcept;
  bool submit(const CancelRequest& request) noexcept;
  bool submit(const ReplaceRequest& request) noexcept;
  bool submit(const MassCancelRequest& request) noexcept;
  bool submit(const SubscribeRequest& request) noexcept;

  /// Stop accepting new work, then join every shard once its queue is empty.
  void drain();

  /// True once every shard has consumed everything submitted to it.
  ///
  /// Unlike drain() this leaves the workers running, which is what startup recovery
  /// needs: the book has to be rebuilt from a journal *without* taking the venue
  /// down with it. drain() joins the workers for good, so a venue that recovered
  /// by draining would come up with a correct book and no engine behind it.
  [[nodiscard]] bool idle() const noexcept {
    for (std::size_t i = 0; i < shards_.size(); ++i) {
      // Only the gate, deliberately: it is the one word designed to be read from
      // another thread. Checking the queue's empty() here looked equivalent but
      // broke the SPSC contract -- empty() reads the consumer's tail_, and the
      // shard worker is the consumer -- which TSan reported as a data race.
      //
      // The in-flight count covers queued *and* executing requests, since claim()
      // increments before the push and the worker decrements after processing, so
      // zero everywhere means everything submitted has been fully applied.
      //
      // Bit 0 is the draining flag; everything above it counts in-flight requests.
      if ((gate_[i].load(std::memory_order_acquire) & ~std::uint64_t{1}) != 0U) {
        return false;
      }
    }
    return true;
  }

  /// Move out snapshot answers since the last call. Paired with submit(Subscribe).
  [[nodiscard]] std::vector<ShardSnapshot> take_snapshots();

  /// Move out the events produced since the last call, for every shard, in shard
  /// order. Owner thread only, and only safe to call between polls -- the shard
  /// workers are mutating their engines concurrently, so the events cannot simply
  /// be read out of the engine.
  ///
  /// Overflow of a shard's event ring is reported rather than papered over: a
  /// dropped market data event silently desynchronises every subscriber's book,
  /// so it is surfaced as a fault the owner can refuse to continue on.
  [[nodiscard]] std::vector<Event> take_events();

  /// True once a shard's event ring overflowed and an event was lost.
  [[nodiscard]] bool event_overflow() const noexcept {
    return event_overflow_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t shard_count() const noexcept {
    return shards_.size();
  }
  [[nodiscard]] bool draining() const noexcept {
    return draining_.load(std::memory_order_acquire);
  }

  /// Combined state hash over every shard, in shard order.
  ///
  /// Deliberately order-dependent, so it is only comparable between two hosts
  /// with the *same* shard layout. To compare books across different layouts,
  /// compare per-symbol state via top_of_book().
  [[nodiscard]] std::uint64_t state_hash() const noexcept;

  /// Top of book for one symbol.
  ///
  /// Removed deliberately. It read the shard's book directly, so calling it from
  /// any thread but the owning worker was a data race -- which is precisely the
  /// mistake this class exists to prevent elsewhere, and which the accessor
  /// invited. Use submit(SubscribeRequest) and take_snapshots() instead: the
  /// owning thread answers, and the caller reads a value it received over a ring.
  ///
  /// After drain() has joined every worker no further requests are accepted, so
  /// there is deliberately no post-shutdown way to read the book.

 private:
  void run_shard(std::size_t i);

  /// Local SymbolId inside the shard that owns the global one.
  [[nodiscard]] SymbolId local_symbol(std::size_t global) const noexcept {
    return local_[global];
  }

  std::vector<Shard> shards_;
  std::vector<SymbolId> local_;
  std::vector<std::unique_ptr<SpscRing<Envelope, kQueueDepth>>> queues_;
  std::vector<std::unique_ptr<SpscRing<Event, kEventDepth>>> events_;
  std::vector<std::unique_ptr<SpscRing<ShardSnapshot, kSnapshotDepth>>> snapshots_;
  // One atomic word per shard: bit 0 is "draining", the rest counts in-flight
  // requests. Deliberately a single word rather than a bool plus a counter --
  // with two words, drain() could set the flag and a worker could observe an
  // in-flight count of zero *before* a concurrent submit incremented it, exit,
  // and leave that submit pushing into a queue nobody will ever read. That loses
  // orders the gateway has already acknowledged, which is the one thing a
  // graceful drain exists to prevent. With one word the check and the increment
  // cannot interleave.
  //
  // A deque, not a vector: std::atomic is neither copyable nor move-insertable,
  // so a vector of them cannot be resized. deque never relocates existing
  // elements on emplace_back, which is exactly what is needed here.
  std::deque<std::atomic<std::uint64_t>> gate_;
  Sequencer sequencer_;
  std::atomic<bool> draining_{false};
  std::atomic<bool> event_overflow_{false};
};

}  // namespace lob