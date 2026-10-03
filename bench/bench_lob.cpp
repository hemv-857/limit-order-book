// Microbenchmarks for the matching path.
//
// Two rules, because a benchmark that breaks them measures the harness instead of
// the code:
//
//   - **No allocation in the measured region.** Every fixture is built outside the
//     timed loop. An engine that grows a vector during matching will look like it
//     allocates, and a reader cannot tell that from the allocator.
//
//   - **The input is deterministic.** Same sequence, same cache behaviour, every
//     run. A benchmark driven by a clock or a random seed measures luck.
//
// Numbers here are per-operation and are only meaningful relative to each other
// and to a recorded before/after for the same benchmark. Absolute nanoseconds on
// any single machine say very little.

#include "core/engine.hpp"
#include "gateway/session.hpp"
#include "gateway/write_queue.hpp"
#include "journal/journal.hpp"
#include "protocol/codec.hpp"
#include "runtime/spsc_ring.hpp"
#include "util/crc32c.hpp"

#include <benchmark/benchmark.h>

#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace lob;  // NOLINT(build/namespaces) -- benchmark scope only

SymbolConfig symbol_config(std::string_view name, std::int64_t max_price = 10'000) {
  SymbolConfig c;
  c.name = name;
  c.min_price = 0;
  c.max_price = max_price;
  c.tick_size = 1;
  c.lot_size = 1;
  c.max_order_qty = 1'000'000;
  c.max_notional = 1'000'000'000;
  c.max_open_orders = 65'536;
  return c;
}

NewOrderRequest make_order(OrderId id, Side side, Price price, Quantity qty,
                           ParticipantId pid = ParticipantId{1}) {
  NewOrderRequest r;
  r.seq = Sequence{id.value};
  r.ts = Timestamp{static_cast<std::int64_t>(id.value)};
  r.symbol = SymbolId{0};
  r.order_id = id;
  r.participant = pid;
  r.side = side;
  r.type = OrderType::Limit;
  r.tif = TimeInForce::GTC;
  r.price = price;
  r.quantity = qty;
  return r;
}

Engine make_engine() {
  return Engine({symbol_config("BM")}, EngineConfig{});
}

// ---------------------------------------------------------------------------
// Engine: the paths a venue actually spends its time in
// ---------------------------------------------------------------------------

void BM_EngineRestLimit(benchmark::State& state) {
  Engine engine = make_engine();
  std::uint64_t next_id = 1;
  for (auto _ : state) {
    const OrderId id{next_id++};
    // A resting bid at a price nothing offers against, so it never matches.
    engine.submit(make_order(id, Side::Buy, Price{50}, Quantity{10}));
    engine.clear_events();

    state.PauseTiming();
    // Reclaim it, so the book does not grow without bound and skew later
    // iterations toward worse cache behaviour.
    CancelRequest c;
    c.seq = Sequence{0};
    c.ts = Timestamp{0};
    c.symbol = SymbolId{0};
    c.order_id = id;
    c.participant = ParticipantId{1};
    engine.submit(c);
    engine.clear_events();
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EngineRestLimit);

/// The hot path in a real venue: an aggressive order sweeping resting liquidity.
void BM_EngineMarketSweep(benchmark::State& state) {
  constexpr int kDepth = 20;
  Engine engine = make_engine();
  std::uint64_t id = 1;

  // Rest offers to sweep, then rebuild them for the next iteration.
  auto seed_book = [&] {
    for (int i = 0; i < kDepth; ++i) {
      engine.submit(make_order(OrderId{static_cast<std::uint64_t>(id++)}, Side::Sell,
                               Price{100 + i}, Quantity{10}));
    }
  };
  seed_book();

  for (auto _ : state) {
    NewOrderRequest r;
    r.seq = Sequence{static_cast<std::uint64_t>(id)};
    r.ts = Timestamp{0};
    r.symbol = SymbolId{0};
    r.order_id = OrderId{static_cast<std::uint64_t>(id++)};
    r.participant = ParticipantId{2};
    r.side = Side::Buy;
    r.type = OrderType::Market;
    r.tif = TimeInForce::IOC;
    r.quantity = Quantity{kDepth * 10};
    engine.submit(r);
    engine.clear_events();
    seed_book();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EngineMarketSweep);

void BM_EngineCancelResting(benchmark::State& state) {
  Engine engine = make_engine();
  constexpr std::size_t kOrders = 1024;
  std::vector<OrderId> ids;
  ids.reserve(kOrders);

  for (auto _ : state) {
    ids.clear();
    for (std::size_t i = 0; i < kOrders; ++i) {
      const OrderId id{static_cast<std::uint64_t>(i) + 1 + ids.size()};
      ids.push_back(id);
      engine.submit(make_order(id, Side::Buy, Price{50}, Quantity{1}));
    }
    engine.clear_events();

    state.PauseTiming();
    for (const OrderId id : ids) {
      engine.submit(make_order(id, Side::Buy, Price{50}, Quantity{1}));
    }
    engine.clear_events();
    state.ResumeTiming();

    for (const OrderId id : ids) {
      CancelRequest c;
      c.seq = Sequence{0};
      c.ts = Timestamp{0};
      c.symbol = SymbolId{0};
      c.order_id = id;
      c.participant = ParticipantId{1};
      engine.submit(c);
    }
    engine.clear_events();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kOrders));
}
BENCHMARK(BM_EngineCancelResting);

void BM_EngineReplaceKeepingPriority(benchmark::State& state) {
  Engine engine = make_engine();
  constexpr std::size_t kOrders = 256;
  std::vector<OrderId> ids;
  for (std::size_t i = 0; i < kOrders; ++i) {
    const OrderId id{i + 1};
    ids.push_back(id);
    engine.submit(make_order(id, Side::Buy, Price{50}, Quantity{10}));
  }
  engine.clear_events();

  std::size_t next = kOrders + 1;
  for (auto _ : state) {
    for (const OrderId id : ids) {
      ReplaceRequest r;
      r.seq = Sequence{static_cast<std::uint64_t>(next++)};
      r.ts = Timestamp{0};
      r.symbol = SymbolId{0};
      r.order_id = id;
      r.participant = ParticipantId{1};
      r.new_price = Price{0};
      // Alternate between shrinking and growing, so both the reduce and the
      // increase path are timed.
      r.new_quantity = Quantity{(next % 2 == 0) ? 9 : 11};
      engine.submit(r);
    }
    engine.clear_events();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(kOrders));
}
BENCHMARK(BM_EngineReplaceKeepingPriority);

/// Book depth as it grows: this is what tells you whether the flat grid still
/// behaves like a flat grid, or whether it has degraded into a linear scan.
void BM_EngineRestAtDepth(benchmark::State& state) {
  for (auto _ : state) {
    state.PauseTiming();
    Engine engine = make_engine();
    for (int i = 0; i < 20'000; ++i) {
      engine.submit(make_order(OrderId{static_cast<std::uint64_t>(i) + 1}, Side::Buy,
                               Price{static_cast<std::int64_t>(i % 500) + 1}, Quantity{1}));
    }
    engine.clear_events();
    state.ResumeTiming();

    CancelRequest c;
    c.seq = Sequence{0};
    c.ts = Timestamp{0};
    c.symbol = SymbolId{0};
    // Cancel from the middle of the book, which is neither the head nor the tail
    // and so exercises the index rather than a boundary shortcut.
    c.order_id = OrderId{10'000};
    c.participant = ParticipantId{1};
    engine.submit(c);
    engine.clear_events();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_EngineRestAtDepth)->Iterations(200);

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

void BM_CodecEncodeFrame(benchmark::State& state) {
  protocol::Inbound m;
  m.type = protocol::MessageType::NewOrder;
  m.new_order.order_id = OrderId{42};
  m.new_order.participant = ParticipantId{1};
  m.new_order.symbol = SymbolId{0};
  m.new_order.side = Side::Buy;
  m.new_order.type = OrderType::Limit;
  m.new_order.tif = TimeInForce::GTC;
  m.new_order.price = Price{100};
  m.new_order.quantity = Quantity{10};

  const std::vector<std::uint8_t> payload = protocol::encode_payload(m);
  std::vector<std::uint8_t> out;
  out.reserve(payload.size() + 16);

  const std::string payload_view(reinterpret_cast<const char*>(payload.data()), payload.size());
  for (auto _ : state) {
    out.clear();
    (void)protocol::encode(protocol::MessageType::NewOrder, payload_view, out);
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(payload.size() + 12));
}
BENCHMARK(BM_CodecEncodeFrame);

void BM_CodecDecodeFrame(benchmark::State& state) {
  protocol::Inbound m;
  m.type = protocol::MessageType::NewOrder;
  m.new_order.order_id = OrderId{42};
  m.new_order.participant = ParticipantId{1};
  m.new_order.symbol = SymbolId{0};
  m.new_order.side = Side::Buy;
  m.new_order.type = OrderType::Limit;
  m.new_order.tif = TimeInForce::GTC;
  m.new_order.price = Price{100};
  m.new_order.quantity = Quantity{10};

  std::vector<std::uint8_t> frame;
  (void)protocol::encode(m, frame);
  const std::string bytes(reinterpret_cast<const char*>(frame.data()), frame.size());

  for (auto _ : state) {
    std::size_t consumed = 0;
    protocol::DecodeError error = protocol::DecodeError::None;
    const auto decoded = protocol::decode(bytes, consumed, error);
    benchmark::DoNotOptimize(decoded.has_value());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(bytes.size()));
}
BENCHMARK(BM_CodecDecodeFrame);

/// CRC over a typical frame payload. Recomputed per frame on both the write and
/// the read path, so it is not incidental work.
void BM_Crc32cFrameSized(benchmark::State& state) {
  std::vector<std::uint8_t> payload(72, 0x5A);
  for (auto _ : state) {
    benchmark::DoNotOptimize(crc32c(payload.data(), payload.size(), 0));
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * 72);
}
BENCHMARK(BM_Crc32cFrameSized);

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

void BM_JournalEncodeRecord(benchmark::State& state) {
  JournalRecord r;
  r.kind = RecordKind::NewOrder;
  r.new_order.seq = Sequence{1};
  r.new_order.ts = Timestamp{1};
  r.new_order.symbol = SymbolId{0};
  r.new_order.order_id = OrderId{42};
  r.new_order.participant = ParticipantId{1};
  r.new_order.side = Side::Buy;
  r.new_order.price = Price{100};
  r.new_order.quantity = Quantity{10};

  std::vector<std::uint8_t> out;
  out.reserve(128);
  for (auto _ : state) {
    out.clear();
    encode_record(r, out);
    benchmark::DoNotOptimize(out.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_JournalEncodeRecord);

void BM_JournalReplayRecords(benchmark::State& state) {
  JournalRecord r;
  r.kind = RecordKind::NewOrder;
  r.new_order.symbol = SymbolId{0};
  r.new_order.order_id = OrderId{1};
  r.new_order.participant = ParticipantId{1};
  r.new_order.side = Side::Buy;
  r.new_order.price = Price{100};
  r.new_order.quantity = Quantity{10};

  std::vector<std::uint8_t> bytes;
  bytes.reserve(85 * 4096);
  for (int i = 0; i < 4096; ++i) {
    encode_record(r, bytes);
  }
  const std::string journal(reinterpret_cast<const char*>(bytes.data()), bytes.size());

  std::size_t seen = 0;
  for (auto _ : state) {
    seen = 0;
    const ReplayReport report = replay(journal, [&seen](const JournalRecord&) { ++seen; });
    benchmark::DoNotOptimize(seen);
    benchmark::ClobberMemory();
    // Assert the benchmark is doing the work it claims. Without this the
    // measurement is worthless: an early return from the first record made this
    // report 560 GiB/s, which is four orders of magnitude faster than the memory
    // bandwidth of the machine it ran on.
    if (report.records_applied != 4096U || seen != 4096U) {
      state.SkipWithError("replay applied " + std::to_string(report.records_applied) +
                          " of 4096, seen=" + std::to_string(seen) +
                          " bytes=" + std::to_string(journal.size()));
      return;
    }
  }
  state.SetItemsProcessed(state.iterations() * 4096);
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(journal.size()));
}
BENCHMARK(BM_JournalReplayRecords);

// ---------------------------------------------------------------------------
// Write queue: the partial-write path
// ---------------------------------------------------------------------------

void BM_WriteQueueAppendConsume(benchmark::State& state) {
  WriteQueue queue;
  const std::string payload(64, 'x');
  for (auto _ : state) {
    queue.append(payload);
    queue.consume(payload.size());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteQueueAppendConsume);

/// The realistic shape: a large outbox drained in kernel-sized pieces, which is
/// what a busy subscriber's socket actually does.
void BM_WriteQueuePartialDrain(benchmark::State& state) {
  WriteQueue queue;
  queue.append(std::string(4096, 'y'));
  for (auto _ : state) {
    while (!queue.empty()) {
      const std::size_t n = queue.readable().size() < 1400 ? queue.readable().size() : 1400;
      queue.consume(n);
    }
    queue.append(std::string(4096, 'y'));
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_WriteQueuePartialDrain);

// ---------------------------------------------------------------------------
// SPSC ring
// ---------------------------------------------------------------------------

void BM_SpscRingRoundTrip(benchmark::State& state) {
  // Measured single-threaded on purpose: this is the memory ordering and the
  // per-operation cost. Thread handoff and cache-line transfer belong to a
  // separate benchmark, not mixed in here.
  SpscRing<int, 1024> ring;
  int counter = 0;
  for (auto _ : state) {
    while (!ring.push(counter)) {
      benchmark::DoNotOptimize(counter);
    }
    int received = 0;
    while (!ring.pop(received)) {
      benchmark::DoNotOptimize(received);
    }
    benchmark::DoNotOptimize(received);
    ++counter;
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscRingRoundTrip);

// ---------------------------------------------------------------------------
// Session state machine
// ---------------------------------------------------------------------------

void BM_SessionOnMessage(benchmark::State& state) {
  protocol::Inbound hello;
  hello.type = protocol::MessageType::Hello;
  hello.session_id = "session-key";

  for (auto _ : state) {
    Session session(1, SessionConfig{}, 0);
    (void)session.on_message(hello, 0);
    benchmark::DoNotOptimize(session.state());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SessionOnMessage);

}  // namespace

BENCHMARK_MAIN();
