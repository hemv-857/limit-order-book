// venue: run the trading venue.
//
// Deliberately thin. Every decision worth making lives in the library where it can
// be tested; this just parses arguments, builds a Venue and runs it until
// interrupted.

// main() is not noexcept and the analyser's exception-escape rule assumes it is,
// which is not a property worth asserting about a process entry point. The
// varargs below are printf-family, which have no C++ overload. SymbolConfig is
// trivially copyable, so `std::move` on it is a no-op that only hides the copy.
//
// NOLINTBEGIN(bugprone-exception-escape)
// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
// NOLINTBEGIN(performance-move-const-arg)

#include "venue/venue.hpp"

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

// A signal handler cannot take a lock or allocate, so the only thing it may do
// is set a flag the run loop already consults. Calling stop() from here would
// touch a reactor from async-signal context, which is exactly the sort of thing
// that works until it does not.
//
// File-scope is not a style choice here: a handler has no other way to reach
// program state.
//
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<bool> g_stop_requested{false};

extern "C" void on_signal(int /*signal*/) {
  g_stop_requested.store(true, std::memory_order_relaxed);
}

[[nodiscard]] long long arg_or(int argc, char** argv, const char* name, long long fallback) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) == name) {
      return std::strtoll(argv[i + 1], nullptr, 10);
    }
  }
  return fallback;
}

}  // namespace

int main(int argc, char** argv) {
  const auto port = static_cast<std::uint16_t>(arg_or(argc, argv, "--port", 9000));
  const auto shards = static_cast<std::size_t>(arg_or(argc, argv, "--shards", 2));
  const long long count = arg_or(argc, argv, "--symbols", 1);
  const bool stop_after_ms = false;
  (void)stop_after_ms;

  lob::VenueConfig config;
  config.shards = shards == 0 ? 1 : shards;
  // SymbolConfig::name is a view, so the strings have to outlive the config. Not a
  // dangling view in a test binary that happens to survive: the venue outlives
  // main's locals by however long it runs.
  std::vector<std::string> names;
  names.reserve(static_cast<std::size_t>(count <= 0 ? 1 : count));
  for (long long i = 0; i < (count <= 0 ? 1 : count); ++i) {
    names.push_back("SYM" + std::to_string(i));
    lob::SymbolConfig symbol;
    symbol.name = names.back();
    symbol.min_price = 0;
    symbol.max_price = 100'000;
    symbol.tick_size = 1;
    symbol.lot_size = 1;
    symbol.max_order_qty = 1'000'000;
    symbol.max_notional = 1'000'000'000;
    symbol.max_open_orders = 4096;
    config.symbols.push_back(symbol);
  }

  // Read the count before the move: after it the vector is empty, and a banner
  // reporting zero symbols is worse than no banner.
  const std::size_t symbol_count = config.symbols.size();
  lob::Venue venue{std::move(config)};
  const std::uint16_t bound = venue.listen_on(port);
  if (bound == 0) {
    (void)std::fprintf(stderr, "venue: could not bind port %u\n", static_cast<unsigned>(port));
    return 1;
  }
  if (std::signal(SIGINT, on_signal) == SIG_ERR || std::signal(SIGTERM, on_signal) == SIG_ERR) {
    (void)std::fprintf(stderr, "venue: could not install signal handlers\n");
  }

  (void)std::fprintf(stderr, "venue: listening on 127.0.0.1:%u, %zu shard(s), %zu symbol(s)\n",
                     static_cast<unsigned>(bound), venue.engine().shard_count(), symbol_count);
  // Poll in short slices so a signal is noticed promptly without the signal
  // handler touching anything but an atomic.
  while (!g_stop_requested.load(std::memory_order_relaxed) && !venue.stopped()) {
    venue.poll_once(50);
  }
  venue.stop();

  (void)std::fprintf(stderr,
                     "venue: stopped. connections=%llu frames=%llu submitted=%llu "
                     "refused=%llu protocol_errors=%llu subscriptions=%llu\n",
                     static_cast<unsigned long long>(venue.stats().connections_accepted.load()),
                     static_cast<unsigned long long>(venue.stats().frames_decoded.load()),
                     static_cast<unsigned long long>(venue.stats().requests_submitted.load()),
                     static_cast<unsigned long long>(venue.stats().requests_refused.load()),
                     static_cast<unsigned long long>(venue.stats().protocol_errors.load()),
                     static_cast<unsigned long long>(venue.stats().subscriptions.load()));
  return 0;
}

// NOLINTEND(performance-move-const-arg)
// NOLINTEND(cppcoreguidelines-pro-type-vararg)
// NOLINTEND(bugprone-exception-escape)
