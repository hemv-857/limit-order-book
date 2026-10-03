#include "runtime/runtime.hpp"

#include <chrono>
#include <type_traits>
#include <utility>

namespace lob {
namespace {

/// Idle backoff for a shard worker: spin-yield this many times before sleeping.
constexpr std::uint32_t kSpinLimit = 64;
constexpr int kIdleSleepUs = 200;

constexpr std::uint64_t kDraining = 1UL;
constexpr std::uint64_t kOneInflight = 2UL;  ///< so the count cannot reach bit 0

/// Claim a slot, or fail if the shard is draining. One fetch_add decides both,
/// so there is no window between "is it draining?" and "count me in".
[[nodiscard]] bool claim(std::atomic<std::uint64_t>& gate) noexcept {
  const std::uint64_t prev = gate.fetch_add(kOneInflight, std::memory_order_acq_rel);
  if ((prev & kDraining) != 0U) {
    gate.fetch_sub(kOneInflight, std::memory_order_release);
    return false;
  }
  return true;
}

void release_slot(std::atomic<std::uint64_t>& gate) noexcept {
  gate.fetch_sub(kOneInflight, std::memory_order_release);
}

// Not named apply: ADL on std::variant would find std::apply and the two collide.
void submit_envelope(Engine& engine, const Envelope& envelope) {
  std::visit(
      [&engine](const auto& request) {
        using T = std::decay_t<decltype(request)>;
        // A Subscribe is answered by the shard from its own book, never handed to
        // the engine, so it has no Engine::submit overload.
        if constexpr (!std::is_same_v<T, SubscribeRequest>) {
          engine.submit(request);
        }
      },
      envelope);
}

}  // namespace

ShardedEngineHost::ShardedEngineHost(std::vector<SymbolConfig> symbols, std::size_t shard_count)
    : sequencer_(shard_count) {
  const std::size_t n = shard_count == 0 ? 1 : shard_count;
  shards_.resize(n);
  queues_.resize(n);
  events_.resize(n);
  snapshots_.resize(n);
  gate_.clear();
  for (std::size_t i = 0; i < n; ++i) {
    gate_.emplace_back(0);
  }
  local_.resize(symbols.size());

  std::vector<std::vector<SymbolConfig>> per_shard(n);
  for (std::size_t g = 0; g < symbols.size(); ++g) {
    const std::size_t shard = sequencer_.shard_for(SymbolId{static_cast<std::uint32_t>(g)});
    local_[g] = SymbolId{static_cast<std::uint32_t>(per_shard[shard].size())};
    per_shard[shard].push_back(symbols[g]);
  }

  for (std::size_t i = 0; i < n; ++i) {
    queues_[i] = std::make_unique<SpscRing<Envelope, kQueueDepth>>();
    events_[i] = std::make_unique<SpscRing<Event, kEventDepth>>();
    // Populated here, not lazily: take_snapshots() indexes this by shard, and an
    // unpopulated vector there is an out-of-bounds read that presents as a hang
    // rather than as a crash.
    snapshots_[i] = std::make_unique<SpscRing<ShardSnapshot, kSnapshotDepth>>();
    // Each shard is constructed and owned by exactly one thread from here on.
    shards_[i].index = i;
    shards_[i].engine = std::make_unique<Engine>(std::move(per_shard[i]), EngineConfig{});
  }
  for (std::size_t i = 0; i < shards_.size(); ++i) {
    shards_[i].worker = std::thread([this, i] { run_shard(i); });
  }
}

ShardedEngineHost::~ShardedEngineHost() {
  drain();
}

void ShardedEngineHost::run_shard(std::size_t i) {
  Envelope envelope;
  // Reset on every successful pop, so a busy shard never sleeps.
  std::uint32_t spins = 0;
  while (true) {
    if (!queues_[i]->pop(envelope)) {
      // Nothing queued. Exit only once draining *and* genuinely idle. Because
      // claim() sets the in-flight count with the same atomic that reads the
      // draining bit, "idle" here cannot be stale.
      const std::uint64_t gate = gate_[i].load(std::memory_order_acquire);
      if ((gate & kDraining) != 0U && (gate >> 1U) == 0U) {
        return;
      }
      // Spin briefly, then sleep. A bare yield was measurably wrong: an idle
      // shard burned 100% of a core, and two of them saturated the machine and
      // starved the very threads they were waiting for. Yield for a short while
      // because the queue really is empty for microseconds at a time under load,
      // then fall back to a short sleep so an idle shard costs nothing.
      if (spins < kSpinLimit) {
        ++spins;
        std::this_thread::yield();
        continue;
      }
      std::this_thread::sleep_for(std::chrono::microseconds(kIdleSleepUs));
      continue;
    }
    spins = 0;
    // A subscribe is answered by the owning thread, from its own book. No other
    // thread may read that book.
    if (const auto* req = std::get_if<SubscribeRequest>(&envelope); req != nullptr) {
      const SymbolId local = local_symbol(req->symbol.value);
      // The boundary must come from the *engine's* sequence space, not the
      // request's. Events are stamped from the engine's own counter starting at 1,
      // while the request carries a gateway sequencer number far larger. Seeding
      // the snapshot with that made every later increment look stale and silently
      // suppressed the whole feed.
      //
      // next_sequence() is the value the *next* event will carry, so one less is
      // the last event already reflected in this snapshot.
      const std::uint64_t boundary = shards_[i].engine->next_sequence().value - 1U;
      (void)snapshots_[i]->push(ShardSnapshot{
          req->symbol, shards_[i].engine->book(local).top_of_book(), boundary, req->session_id});
      release_slot(gate_[i]);
      continue;
    }
    submit_envelope(*shards_[i].engine, envelope);
    // Publish this request's events to the owner before releasing the slot, so
    // drain() cannot return while events are still in flight to the reader.
    Engine& engine = *shards_[i].engine;
    const EventBuffer& events = engine.events();
    for (std::size_t k = 0; k < events.size(); ++k) {
      if (!events_[i]->push(events[k])) {
        event_overflow_.store(true, std::memory_order_release);
      }
    }
    release_slot(gate_[i]);
  }
}

bool ShardedEngineHost::submit(const NewOrderRequest& request) noexcept {
  if (request.symbol.value >= local_.size()) {
    return false;
  }
  const std::size_t s = sequencer_.shard_for(request.symbol);
  if (!claim(gate_[s])) {
    return false;
  }
  NewOrderRequest local_request = request;
  local_request.symbol = local_symbol(request.symbol.value);
  // A full queue must not silently lose an order: undo the count and report.
  if (!queues_[s]->push(Envelope{local_request})) {
    release_slot(gate_[s]);
    return false;
  }
  return true;
}

bool ShardedEngineHost::submit(const CancelRequest& request) noexcept {
  if (request.symbol.value >= local_.size()) {
    return false;
  }
  const std::size_t s = sequencer_.shard_for(request.symbol);
  if (!claim(gate_[s])) {
    return false;
  }
  CancelRequest local_request = request;
  local_request.symbol = local_symbol(request.symbol.value);
  if (!queues_[s]->push(Envelope{local_request})) {
    release_slot(gate_[s]);
    return false;
  }
  return true;
}

bool ShardedEngineHost::submit(const ReplaceRequest& request) noexcept {
  if (request.symbol.value >= local_.size()) {
    return false;
  }
  const std::size_t s = sequencer_.shard_for(request.symbol);
  if (!claim(gate_[s])) {
    return false;
  }
  ReplaceRequest local_request = request;
  local_request.symbol = local_symbol(request.symbol.value);
  if (!queues_[s]->push(Envelope{local_request})) {
    release_slot(gate_[s]);
    return false;
  }
  return true;
}

bool ShardedEngineHost::submit(const MassCancelRequest& request) noexcept {
  // A mass cancel is symbol-wide; the shard is derived from its only symbol.
  const std::size_t s = sequencer_.shard_for(SymbolId{0});
  if (!claim(gate_[s])) {
    return false;
  }
  if (!queues_[s]->push(Envelope{request})) {
    release_slot(gate_[s]);
    return false;
  }
  return true;
}

void ShardedEngineHost::drain() {
  if (draining_.exchange(true, std::memory_order_acq_rel)) {
    return;  // already draining
  }
  // Flag every shard before joining any of them, so a worker never sees itself
  // as the last one draining while another is still accepting.
  for (std::size_t i = 0; i < shards_.size(); ++i) {
    gate_[i].fetch_or(kDraining, std::memory_order_acq_rel);
  }
  for (auto& shard : shards_) {
    if (shard.worker.joinable()) {
      shard.worker.join();
    }
  }
}

bool ShardedEngineHost::submit(const SubscribeRequest& request) noexcept {
  if (request.symbol.value >= local_.size()) {
    return false;
  }
  const std::size_t s = sequencer_.shard_for(request.symbol);
  if (!claim(gate_[s])) {
    return false;
  }
  // The symbol stays global here on purpose. The worker localises it when it
  // looks the book up, and the reply echoes the global id so the caller can match
  // the answer to what it asked for. Localising twice, or echoing the local id,
  // makes the reply unmatchable.
  if (!queues_[s]->push(Envelope{request})) {
    release_slot(gate_[s]);
    return false;
  }
  return true;
}

std::vector<ShardSnapshot> ShardedEngineHost::take_snapshots() {
  std::vector<ShardSnapshot> out;
  ShardSnapshot s{};
  for (std::size_t i = 0; i < shards_.size(); ++i) {
    while (snapshots_[i]->pop(s)) {
      out.push_back(s);
    }
  }
  return out;
}

std::vector<Event> ShardedEngineHost::take_events() {
  std::vector<Event> out;
  Event e{};
  for (std::size_t i = 0; i < shards_.size(); ++i) {
    while (events_[i]->pop(e)) {
      out.push_back(e);
    }
  }
  return out;
}

std::uint64_t ShardedEngineHost::state_hash() const noexcept {
  // FNV-1a folded over each shard's hash in shard order. Not a hash of hashes:
  // order matters and must be fixed, or two different books could collide.
  std::uint64_t h = 1469598103934665603ULL;
  for (const auto& shard : shards_) {
    const std::uint64_t shard_hash = shard.engine->state_hash();
    for (int b = 0; b < 8; ++b) {
      h ^= (shard_hash >> (8 * b)) & 0xFFU;
      h *= 1099511628211ULL;
    }
  }
  return h;
}

}  // namespace lob