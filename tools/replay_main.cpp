// replay -- inspect a journal offline, and prove it still rebuilds a book.
//
//   replay <journal-path>            summary: records by kind, torn tail, hash
//   replay <journal-path> --verify   rebuild into an engine and print its hash
//   replay <journal-path> --dump N   print the first N records
//
// The point is operational: an operator who has a journal from a crashed venue
// needs to know whether it is intact, how far it gets, and what book it produces
// -- without starting a venue and hoping.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/engine.hpp"
#include "core/types.hpp"
#include "journal/journal.hpp"

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: replay <journal-path> [--verify] [--dump N] [--symbol N]\n"
               "  --verify   rebuild the book and print the resulting state hash\n"
               "  --dump N   print the first N records\n"
               "  --symbol N only consider records for symbol N (with --verify)\n");
  return 2;
}

const char* kind_name(lob::RecordKind kind) {
  switch (kind) {
    case lob::RecordKind::NewOrder:
      return "new_order";
    case lob::RecordKind::Cancel:
      return "cancel";
    case lob::RecordKind::Replace:
      return "replace";
    case lob::RecordKind::MassCancel:
      return "mass_cancel";
    case lob::RecordKind::Invalid:
      return "invalid";
  }
  return "unknown";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    return usage();
  }
  const std::string path = argv[1];
  bool verify = false;
  std::size_t dump = 0;
  std::optional<lob::SymbolId> only;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--verify") {
      verify = true;
    } else if (arg == "--dump" && i + 1 < argc) {
      dump = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
    } else if (arg == "--symbol" && i + 1 < argc) {
      only = lob::SymbolId{static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 10))};
    } else {
      return usage();
    }
  }

  // Every segment, not just the base file: a rolled journal split across files
  // and reading only the first would silently under-report what was recovered.
  const std::string bytes = lob::read_all_segments(path);
  if (bytes.empty()) {
    std::fprintf(stderr, "replay: %s is empty or unreadable\n", path.c_str());
    return 1;
  }

  // Rebuilding needs a symbol config wide enough for whatever the log contains.
  // One symbol per possible id keeps the engine's own bounds checks satisfied
  // without having to guess a symbol table.
  std::vector<lob::SymbolConfig> symbols;
  std::uint64_t max_symbol = 0;
  {
    std::size_t ignored = 0;
    (void)lob::replay(bytes, [&](const lob::JournalRecord& r) {
      std::uint64_t s = 0;
      switch (r.kind) {
        case lob::RecordKind::NewOrder:
          s = r.new_order.symbol.value;
          break;
        case lob::RecordKind::Cancel:
          s = r.cancel.symbol.value;
          break;
        case lob::RecordKind::Replace:
          s = r.replace.symbol.value;
          break;
        case lob::RecordKind::MassCancel:
          s = 0;
          break;
        case lob::RecordKind::Invalid:
          break;
      }
      if (s > max_symbol) {
        max_symbol = s;
      }
      ++ignored;
    });
  }
  // SymbolConfig is keyed by position: SymbolId N is the Nth entry. The price
  // grid has to be wide enough for whatever prices the log contains, otherwise
  // every replayed order is rejected for being out of band and the tool would
  // report an empty book for a perfectly good journal.
  std::int64_t lowest = 0;
  std::int64_t highest = 0;
  bool first_price = true;
  (void)lob::replay(bytes, [&](const lob::JournalRecord& r) {
    if (r.kind != lob::RecordKind::NewOrder) {
      return;
    }
    const std::int64_t p = r.new_order.price.value;
    if (first_price) {
      lowest = p;
      highest = p;
      first_price = false;
    } else {
      if (p < lowest) {
        lowest = p;
      }
      if (p > highest) {
        highest = p;
      }
    }
  });
  const std::int64_t pad = 64;
  for (std::uint64_t i = 0; i <= max_symbol; ++i) {
    lob::SymbolConfig cfg;
    cfg.name = "replay";
    cfg.min_price = lowest - pad;
    cfg.max_price = highest + pad;
    cfg.lot_size = 1;
    cfg.max_order_qty = 1'000'000'000;
    cfg.max_notional = 1'000'000'000'000;
    cfg.max_open_orders = 1'000'000;
    symbols.push_back(cfg);
  }

  lob::Engine engine(symbols, lob::EngineConfig{});
  std::size_t counts[5] = {0, 0, 0, 0, 0};
  std::size_t applied = 0;
  std::size_t printed = 0;

  const lob::ReplayReport report = lob::replay(bytes, [&](const lob::JournalRecord& r) {
    // Indexed explicitly, not by casting the enum: RecordKind starts at
    // Invalid = 0, so the cast shifted every count by one and reported five new
    // orders as five cancels.
    switch (r.kind) {
      case lob::RecordKind::NewOrder:
        ++counts[0];
        break;
      case lob::RecordKind::Cancel:
        ++counts[1];
        break;
      case lob::RecordKind::Replace:
        ++counts[2];
        break;
      case lob::RecordKind::MassCancel:
        ++counts[3];
        break;
      case lob::RecordKind::Invalid:
        ++counts[4];
        break;
    }
    if (dump != 0U && printed < dump) {
      std::printf("%-12s", kind_name(r.kind));
      switch (r.kind) {
        case lob::RecordKind::NewOrder:
          std::printf(" symbol=%llu order=%llu %s qty=%lld price=%lld",
                      static_cast<unsigned long long>(r.new_order.symbol.value),
                      static_cast<unsigned long long>(r.new_order.order_id.value),
                      r.new_order.side == lob::Side::Buy ? "buy" : "sell",
                      static_cast<long long>(r.new_order.quantity.value),
                      static_cast<long long>(r.new_order.price.value));
          break;
        case lob::RecordKind::Cancel:
          std::printf(" symbol=%llu order=%llu",
                      static_cast<unsigned long long>(r.cancel.symbol.value),
                      static_cast<unsigned long long>(r.cancel.order_id.value));
          break;
        case lob::RecordKind::Replace:
          std::printf(" symbol=%llu order=%llu new_qty=%lld",
                      static_cast<unsigned long long>(r.replace.symbol.value),
                      static_cast<unsigned long long>(r.replace.order_id.value),
                      static_cast<long long>(r.replace.new_quantity.value));
          break;
        case lob::RecordKind::MassCancel:
          std::printf(" participant=%llu",
                      static_cast<unsigned long long>(r.mass_cancel.participant.value));
          break;
        case lob::RecordKind::Invalid:
          std::printf(" (undecodable)");
          break;
      }
      std::printf("\n");
      ++printed;
    }
    if (!verify || r.kind == lob::RecordKind::Invalid) {
      return;
    }
    bool want = true;
    if (only.has_value()) {
      switch (r.kind) {
        case lob::RecordKind::NewOrder:
          want = r.new_order.symbol.value == only->value;
          break;
        case lob::RecordKind::Cancel:
          want = r.cancel.symbol.value == only->value;
          break;
        case lob::RecordKind::Replace:
          want = r.replace.symbol.value == only->value;
          break;
        default:
          want = only->value == 0U;
          break;
      }
    }
    if (!want) {
      return;
    }
    // Rejections are expected and not errors: the log records what the venue
    // accepted, and replaying the same sequence against the same engine must
    // reach the same book, including refusing the same requests.
    switch (r.kind) {
      case lob::RecordKind::NewOrder:
        (void)engine.submit(r.new_order);
        break;
      case lob::RecordKind::Cancel:
        (void)engine.submit(r.cancel);
        break;
      case lob::RecordKind::Replace:
        (void)engine.submit(r.replace);
        break;
      case lob::RecordKind::MassCancel:
        (void)engine.submit(r.mass_cancel);
        break;
      case lob::RecordKind::Invalid:
        break;
    }
    ++applied;
  });

  std::printf("journal:  %s\n", path.c_str());
  std::printf("bytes:    %zu\n", report.bytes_consumed);
  std::printf("records:  %zu new_order  %zu cancel  %zu replace  %zu mass_cancel\n", counts[0],
              counts[1], counts[2], counts[3]);
  std::printf("torn:     %s\n", report.truncated ? "yes (tail discarded)" : "no");
  if (verify) {
    std::printf("applied:  %zu\n", applied);
    std::printf("hash:     %016llx\n", static_cast<unsigned long long>(engine.state_hash()));
    for (std::size_t i = 0; i < symbols.size(); ++i) {
      const lob::SymbolId id{static_cast<std::uint32_t>(i)};
      const lob::TopOfBook top = engine.book(id).top_of_book();
      if (top.has_bid || top.has_ask) {
        std::printf("  symbol %zu: bid=%lldx%lld ask=%lldx%lld crossed=%s\n", i,
                    static_cast<long long>(top.best_bid.value),
                    static_cast<long long>(top.best_bid_qty.value),
                    static_cast<long long>(top.best_ask.value),
                    static_cast<long long>(top.best_ask_qty.value), top.crossed() ? "YES" : "no");
      }
    }
  }
  // A torn tail is worth a non-zero exit: the operator asked whether this journal
  // is trustworthy and the answer is "up to the last record".
  return report.truncated ? 3 : 0;
}