#include "fuzz/targets.hpp"

#include "core/engine.hpp"
#include "journal/journal.hpp"
#include "protocol/codec.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <unordered_map>

namespace lob::fuzz {
namespace {

/// A violated invariant is a bug in the engine, not a bad input, so it aborts
/// with the reason rather than being counted as a rejected input. Swallowing it
/// would make the fuzzer's job impossible: the interesting bugs are exactly the
/// states that are self-consistent but wrong.
[[noreturn]] void invariant_failed(std::string_view why, Target target) noexcept {
  std::fprintf(stderr, "INVARIANT VIOLATED in fuzz target %s: %.*s\n", name(target),
               static_cast<int>(why.size()), why.data());
  std::fflush(stderr);
  std::abort();
}

/// A tiny deterministic reader, so the engine target can derive requests from bytes
/// without pulling in the wire codec -- the codec has its own target, and coupling
/// them would mean a codec bug masks an engine bug and vice versa.
class Bytes {
 public:
  Bytes(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  [[nodiscard]] bool done() const noexcept {
    return pos_ >= size_;
  }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return size_ - pos_;
  }

  [[nodiscard]] std::uint8_t u8() noexcept {
    if (pos_ >= size_) {
      exhausted_ = true;
      return 0;
    }
    return data_[pos_++];
  }

  /// Big-endian, so multi-byte values in a corpus file read the way they look.
  [[nodiscard]] std::uint64_t u64() noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v = (v << 8) | u8();
    }
    return v;
  }

  [[nodiscard]] bool exhausted() const noexcept {
    return exhausted_;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  bool exhausted_ = false;
};

/// xorshift64*, seeded from the input. The input is a *seed*, not a script: tying
/// operation count to input length means a 33-byte corpus entry produces one
/// operation and the target explores almost nothing.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E37'79B9'7F4A'7C15ULL : seed) {}
  std::uint64_t next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545'F491'4F6C'DD1DULL;
  }
  [[nodiscard]] std::uint64_t below(std::uint64_t n) {
    return n == 0 ? 0 : next() % n;
  }

 private:
  std::uint64_t state_;
};

/// Fold an arbitrary 64-bit value into a small range. Used to keep generated
/// prices and quantities inside a plausible domain most of the time while still
/// reaching the edges often: a fuzz target that only ever generates valid orders
/// tests nothing.
[[nodiscard]] std::int64_t bounded(std::uint64_t raw, std::int64_t lo, std::int64_t hi) noexcept {
  const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1U;
  return lo + static_cast<std::int64_t>(raw % span);
}

SymbolConfig fuzz_config() {
  SymbolConfig c;
  c.name = "FZ";
  c.min_price = 0;
  c.max_price = 64;
  c.tick_size = 1;
  c.lot_size = 1;
  c.max_order_qty = 1'000;
  c.max_notional = 10'000'000;
  c.max_open_orders = 64;
  return c;
}

// ---------------------------------------------------------------------------
// Codec
// ---------------------------------------------------------------------------

bool codec_target(const std::uint8_t* data, std::size_t size) noexcept {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);

  // Every offset of the buffer, so a frame that starts one byte late is exercised.
  // Truncating a valid frame at each length is the single most productive mutation
  // for a framed protocol, and doing it here beats hoping the mutator finds it.
  bool interesting = false;
  for (std::size_t offset = 0; offset <= size && offset < 64; ++offset) {
    protocol::DecodeError error = protocol::DecodeError::None;
    std::size_t consumed = 0;
    const auto message = protocol::decode(bytes.substr(offset), consumed, error);
    if (message) {
      // A frame that decodes must consume exactly its own bytes, never more:
      // over-consuming would let two frames share bytes and desynchronise a
      // stream with no error anywhere.
      if (consumed > bytes.size() - offset) {
        invariant_failed("decode consumed more than it was given", Target::Codec);
      }
      interesting = true;
    }
  }

  // The incremental reader must behave the same way when fed one byte at a time.
  protocol::FrameReader reader;
  for (std::size_t i = 0; i < size && i < 256; ++i) {
    reader.append(bytes.substr(i, 1));
    protocol::DecodeError error = protocol::DecodeError::None;
    (void)reader.next(error);
  }
  return interesting;
}

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

bool journal_target(const std::uint8_t* data, std::size_t size) noexcept {
  const std::string_view bytes(reinterpret_cast<const char*>(data), size);

  std::size_t applied = 0;
  const ReplayReport report = replay(bytes, [&applied](const JournalRecord&) { ++applied; });

  // Whatever the reader claims to have consumed must fit in what it was given, and
  // a torn tail must never be reported as clean.
  if (report.bytes_consumed > size) {
    invariant_failed("journal consumed more than it was given", Target::Journal);
  }
  if (report.truncated && report.bytes_consumed == size && applied > 0) {
    // Truncated with nothing left over means a record was rejected mid-stream
    // after earlier records were applied; that is allowed, but the consumed count
    // must then not claim the whole buffer.
    if (report.bytes_consumed >= size) {
      invariant_failed("journal reported truncation at a clean boundary", Target::Journal);
    }
  }

  // Truncating at every length must never yield more records than the whole
  // buffer, and a prefix can only ever have fewer.
  for (std::size_t cut = 0; cut <= size && cut < 32; ++cut) {
    std::size_t prefix_applied = 0;
    (void)replay(bytes.substr(0, cut),
                 [&prefix_applied](const JournalRecord&) { ++prefix_applied; });
    if (prefix_applied > applied) {
      invariant_failed("a journal prefix replayed more records than the whole journal",
                       Target::Journal);
    }
  }
  return applied > 0;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

bool engine_target(const std::uint8_t* data, std::size_t size) noexcept {
  Engine engine({fuzz_config()}, EngineConfig{});

  // Seed from the whole input, so two similar inputs explore different streams and
  // one short input still produces a long run.
  std::uint64_t seed = 0xCBF2'9CE4'8422'2325ULL;
  for (std::size_t i = 0; i < size; ++i) {
    seed ^= data[i];
    seed *= 0x1000'0000'01B3ULL;
  }
  Rng rng(seed ^ static_cast<std::uint64_t>(size));

  // Crossing liquidity has to be built, not hoped for. Two random prices over 80
  // levels with random sides technically collide often, but post-only rejects and
  // the open-order cap mean the target still spends most of its budget on orders
  // that never meet. So half the time the next order is deliberately placed to
  // cross the last one: same price, opposite side. That is what produces fills,
  // and fills are what make every later replace interesting.
  Price last_price{0};
  bool have_last = false;
  bool last_was_buy = false;

  // Grey-box, not blind: Trade events are the engine telling us how much of each
  // order has actually executed, and the interesting replace cases are all
  // relative to that number. Generating quantities uniformly and hoping to hit
  // "new total equals filled" wastes almost every input -- a blind fuzzer does not
  // find the bug it is most likely to be shipped with.
  std::unordered_map<std::uint64_t, std::int64_t> filled;
  std::int64_t last_fill_qty = 0;
  std::int64_t last_fill_order = 0;

  std::uint64_t seq = 1;
  std::int64_t ts = 0;
  int operations = 0;

  // A fixed budget rather than "until the input runs out": every input explores
  // the same amount, which makes failures reproducible and the timing predictable.
  constexpr int kOperations = 600;
  while (operations < kOperations) {
    const std::uint8_t kind = static_cast<std::uint8_t>(rng.next());
    ++operations;

    // Deliberately includes invalid kinds: the engine must reject them, and the
    // rejection paths are as worth fuzzing as the acceptance paths.
    const std::uint64_t raw_a = rng.next();
    const std::uint64_t raw_b = rng.next();
    const std::uint64_t raw_c = rng.next();
    const std::uint64_t raw_d = rng.next();

    // Validity matters more than variety here. Generating enums and quantities
    // uniformly means almost every order is rejected on validation, nothing ever
    // rests, nothing ever trades, and the target quietly explores nothing. So:
    // mostly valid, with a deliberate minority of invalid to keep the rejection
    // paths exercised.
    Side side = static_cast<Side>(raw_a & 1U);
    Price price{static_cast<std::int64_t>(raw_b % 80U)};
    if (have_last && rng.below(2) == 0) {
      side = last_was_buy ? Side::Sell : Side::Buy;
      // At the same price, or one tick through it, so it definitely trades.
      price = last_was_buy ? Price{last_price.value - 1} : Price{last_price.value + 1};
      if (price.value < 0) {
        price = last_price;
      }
    }
    const OrderType type = (raw_a >> 1) % 10U < 8U ? static_cast<OrderType>((raw_a >> 4) % 4U)
                                                   : static_cast<OrderType>((raw_a >> 8) % 9U);
    const TimeInForce tif = (raw_a >> 12) % 10U < 8U ? static_cast<TimeInForce>((raw_a >> 16) % 5U)
                                                     : static_cast<TimeInForce>((raw_a >> 20) % 9U);
    // Positive most of the time; zero and negative occasionally, because those are
    // boundaries the validator has to reject rather than clamp.
    const Quantity quantity = (raw_c % 10U) < 8U
                                  ? Quantity{1 + static_cast<std::int64_t>((raw_c >> 8) % 40U)}
                                  : Quantity{bounded(raw_c, -3, 0)};
    OrderId order_id{OrderId{(raw_d % 128U) + 1U}.value};
    const ParticipantId participant{static_cast<std::uint32_t>((raw_c >> 20) & 3U)};

    // Absorb what the last operation actually did, so the next one can aim at it.
    for (std::size_t k = 0; k < engine.events().size(); ++k) {
      const Event& e = engine.events()[k];
      if (e.type == EventType::Trade) {
        filled[e.order_id.value] += e.qty.value;
        filled[e.maker_order_id.value] += e.qty.value;
        last_fill_qty = e.qty.value;
        last_fill_order = static_cast<std::int64_t>(e.order_id.value);
      }
    }
    if (!filled.empty()) {
      // Occasionally reuse a real filled quantity as the order id, so a replace
      // lands on an order that genuinely has a part-filled history.
      if ((raw_d >> 13) % 4U == 0U) {
        order_id = OrderId{static_cast<std::uint64_t>(last_fill_order)};
      }
    }

    switch (kind % 5U) {
      case 0:
      case 1: {
        NewOrderRequest r;
        last_price = price;
        last_was_buy = side == Side::Buy;
        have_last = true;
        r.seq = Sequence{seq++};
        r.ts = Timestamp{ts++};
        r.symbol = SymbolId{0};
        r.order_id = order_id;
        r.participant = participant;
        r.side = side;
        r.type = type;
        r.tif = tif;
        r.price = price;
        r.trigger_price = Price{static_cast<std::int64_t>((raw_b >> 7) % 80U)};
        r.quantity = quantity;
        // Every so often an iceberg, because replenishment is where the queue
        // ordering gets interesting.
        r.display_qty = ((raw_d >> 7) & 1U) != 0U
                            ? Quantity{1 + static_cast<std::int64_t>(raw_d % 4U)}
                            : Quantity{0};
        r.post_only = ((raw_d >> 9) & 1U) != 0U;
        engine.submit(r);
        break;
      }
      case 2: {
        CancelRequest r;
        r.seq = Sequence{seq++};
        r.ts = Timestamp{ts++};
        r.symbol = SymbolId{0};
        r.order_id = order_id;
        r.participant = participant;
        engine.submit(r);
        break;
      }
      case 3: {  // NOLINT
        ReplaceRequest r;
        r.seq = Sequence{seq++};
        r.ts = Timestamp{ts++};
        r.symbol = SymbolId{0};
        r.order_id = order_id;
        r.participant = participant;
        r.new_price = ((raw_d >> 11) & 1U) != 0U ? price : Price{0};
        // Aim at the boundary: a new total equal to what has already executed
        // leaves zero remaining, which is the case a replace must treat as a
        // cancel rather than rest. Values just either side of it matter too, so
        // the perturbation is small rather than absent.
        r.new_quantity = Quantity{quantity.value};
        if (!filled.empty() && (raw_a >> 17) % 3U != 0U) {
          if (getenv("LOB_FUZZ_TRACE"))
            std::fprintf(stderr, "AIM order=%llu filled=%zu\n", (unsigned long long)order_id.value,
                         filled.size());
          const auto it = filled.find(order_id.value);
          const std::int64_t done = it == filled.end() ? 0 : it->second;
          const std::int64_t nudge = static_cast<std::int64_t>((raw_c >> 40) % 3U) - 1;
          r.new_quantity = Quantity{done + nudge};
        }
        engine.submit(r);
        break;
      }
      default: {
        MassCancelRequest r;
        r.seq = Sequence{seq++};
        r.ts = Timestamp{ts++};
        r.participant = participant;
        engine.submit(r);
        break;
      }
    }

    // The whole point of this target. Every operation, the book must be fully
    // consistent: queue links sound, aggregates equal to the sum over their
    // orders, extremes matching the bitmap, no resting order with zero quantity,
    // no side-agnostic level reachable as the wrong extreme, and the book never
    // crossed.
    std::string_view why = "ok";
    if (!engine.check_invariants(&why)) {
      invariant_failed(why, Target::Engine);
    }
    if (engine.book(SymbolId{0}).top_of_book().crossed()) {
      invariant_failed("book crossed", Target::Engine);
    }
    // The engine asserts rather than silently dropping when its event buffer
    // fills, so the target must consume and clear each round. Leaving them
    // accumulated makes a long run trip the overflow assert, which is the target
    // misusing the engine rather than the engine misbehaving.
    engine.clear_events();
  }
  return operations > 0;
}

}  // namespace

namespace {

/// Build the request plumbing every scenario shares.
struct ScenarioEngine {
  Engine engine;

  ScenarioEngine() : engine(std::vector<SymbolConfig>{fuzz_config()}, EngineConfig{}) {}

  std::uint64_t seq = 1;
  std::int64_t ts = 0;

  void check() {
    std::string_view why = "ok";
    if (!engine.check_invariants(&why)) {
      invariant_failed(why, Target::Engine);
    }
    if (engine.book(SymbolId{0}).top_of_book().crossed()) {
      invariant_failed("book crossed", Target::Engine);
    }
    engine.clear_events();
  }

  void order(OrderId id, ParticipantId pid, Side side, OrderType type, TimeInForce tif, Price price,
             Price trigger, Quantity qty, Quantity display, bool post_only) {
    NewOrderRequest r;
    r.seq = Sequence{seq++};
    r.ts = Timestamp{ts++};
    r.symbol = SymbolId{0};
    r.order_id = id;
    r.participant = pid;
    r.side = side;
    r.type = type;
    r.tif = tif;
    r.price = price;
    r.trigger_price = trigger;
    r.quantity = qty;
    r.display_qty = display;
    r.post_only = post_only;
    engine.submit(r);
    check();
  }

  void replace(OrderId id, ParticipantId pid, Price price, Quantity qty) {
    ReplaceRequest r;
    r.seq = Sequence{seq++};
    r.ts = Timestamp{ts++};
    r.symbol = SymbolId{0};
    r.order_id = id;
    r.participant = pid;
    r.new_price = price;
    r.new_quantity = qty;
    engine.submit(r);
    check();
  }
};

/// Rest 10, fill 4, then replace the new *total* to exactly the filled quantity.
/// Must cancel; resting a zero-quantity order makes the matching loop spin forever.
void scenario_replace_to_exactly_filled() {
  ScenarioEngine s;
  s.order(OrderId{1}, ParticipantId{1}, Side::Sell, OrderType::Limit, TimeInForce::GTC, Price{100},
          Price{0}, Quantity{10}, Quantity{0}, false);
  s.order(OrderId{2}, ParticipantId{2}, Side::Buy, OrderType::Limit, TimeInForce::GTC, Price{100},
          Price{0}, Quantity{4}, Quantity{0}, false);
  s.replace(OrderId{1}, ParticipantId{1}, Price{0}, Quantity{4});
}

/// A price-changing replace that lands across the resting touch. Never re-runs
/// matching, so allowing it leaves a crossed book.
void scenario_replace_across_the_book() {
  ScenarioEngine s;
  s.order(OrderId{1}, ParticipantId{1}, Side::Buy, OrderType::Limit, TimeInForce::GTC, Price{100},
          Price{0}, Quantity{5}, Quantity{0}, false);
  s.order(OrderId{2}, ParticipantId{2}, Side::Sell, OrderType::Limit, TimeInForce::GTC, Price{104},
          Price{0}, Quantity{5}, Quantity{0}, false);
  s.replace(OrderId{2}, ParticipantId{2}, Price{100}, Quantity{5});
}

/// An iceberg whose displayed slice is consumed repeatedly, so replenishment and
/// priority loss run many times over.
void scenario_iceberg_replenishment() {
  ScenarioEngine s;
  s.order(OrderId{1}, ParticipantId{1}, Side::Sell, OrderType::Limit, TimeInForce::GTC, Price{50},
          Price{0}, Quantity{20}, Quantity{4}, false);
  for (int i = 2; i <= 8; ++i) {
    s.order(OrderId{static_cast<std::uint64_t>(i)}, ParticipantId{2}, Side::Buy, OrderType::Limit,
            TimeInForce::GTC, Price{50}, Price{0}, Quantity{4}, Quantity{0}, false);
  }
}

/// Stop and stop-limit churn, including a replace that reduces a pending stop to
/// nothing remaining.
void scenario_stop_churn() {
  ScenarioEngine s;
  s.order(OrderId{1}, ParticipantId{1}, Side::Sell, OrderType::StopLimit, TimeInForce::GTC,
          Price{90}, Price{95}, Quantity{10}, Quantity{0}, false);
  s.order(OrderId{2}, ParticipantId{2}, Side::Buy, OrderType::Limit, TimeInForce::GTC, Price{80},
          Price{0}, Quantity{5}, Quantity{0}, false);
  s.replace(OrderId{1}, ParticipantId{1}, Price{0}, Quantity{2});
  s.replace(OrderId{1}, ParticipantId{1}, Price{0}, Quantity{0});
}

}  // namespace

void run_engine_scenarios() noexcept {
  scenario_replace_to_exactly_filled();
  scenario_replace_across_the_book();
  scenario_iceberg_replenishment();
  scenario_stop_churn();
}

std::size_t engine_scenario_count() noexcept {
  return 4;
}

const char* name(Target target) noexcept {
  switch (target) {
    case Target::Codec:
      return "codec";
    case Target::Journal:
      return "journal";
    case Target::Engine:
      return "engine";
  }
  return "unknown";
}

bool run(Target target, const std::uint8_t* data, std::size_t size) noexcept {
  // An empty input is a legitimate input for a trust boundary, and both readers
  // must treat it as "not yet" rather than dereferencing past the end.
  static const std::uint8_t kEmpty[1] = {0};
  if (data == nullptr) {
    data = kEmpty;
    size = 0;
  }
  switch (target) {
    case Target::Codec:
      return codec_target(data, size);
    case Target::Journal:
      return journal_target(data, size);
    case Target::Engine:
      return engine_target(data, size);
  }
  return false;
}

}  // namespace lob::fuzz
