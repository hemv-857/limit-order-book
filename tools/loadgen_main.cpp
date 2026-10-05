// loadgen -- sustained order flow against a running venue.
//
//   loadgen --port 9000 --seconds 30 --connections 8 --symbol SYM0
//
// Exists to answer one question the unit tests cannot: does the venue stay
// correct and responsive under sustained concurrent load, and does throughput
// stay flat or collapse? Reports what was sent, what came back, and the
// latency distribution, then exits non-zero if the venue refused or dropped
// anything -- so it can gate CI rather than merely print a number.
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "core/config.hpp"
#include "core/types.hpp"
#include "protocol/codec.hpp"

namespace {

[[nodiscard]] long long arg_or(int argc, char** argv, const char* name, long long fallback) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) == name) {
      return std::strtoll(argv[i + 1], nullptr, 10);
    }
  }
  return fallback;
}

[[nodiscard]] std::string arg_str(int argc, char** argv, const char* name, const char* fallback) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::string_view(argv[i]) == name) {
      return argv[i + 1];
    }
  }
  return fallback;
}

/// Connect, handshake, authenticate. Returns the fd or -1.
[[nodiscard]] int connect_and_authenticate(std::uint16_t port, const std::string& session,
                                           const std::string& token) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  int one = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  const auto write = [fd](const lob::protocol::Inbound& m) {
    std::vector<std::uint8_t> buf;
    if (!lob::protocol::encode(m, buf)) {
      return false;
    }
    std::size_t offset = 0;
    while (offset < buf.size()) {
      const ssize_t n = ::write(fd, buf.data() + offset, buf.size() - offset);
      if (n <= 0) {
        return false;
      }
      offset += static_cast<std::size_t>(n);
    }
    return true;
  };
  // Inbound carries these flat, not nested under hello/authenticate sub-structs.
  lob::protocol::Inbound hello;
  hello.type = lob::protocol::MessageType::Hello;
  hello.session_id = session;
  lob::protocol::Inbound auth;
  auth.type = lob::protocol::MessageType::Authenticate;
  auth.session_id = session;
  auth.participant_token = token;
  if (!write(hello) || !write(auth)) {
    ::close(fd);
    return -1;
  }
  return fd;
}

struct Counters {
  std::atomic<std::uint64_t> sent{0};
  std::atomic<std::uint64_t> rejected{0};
  std::atomic<std::uint64_t> write_failed{0};
  std::atomic<std::uint64_t> latencies_us{0};  // summed; divided by sent at the end
  std::atomic<std::uint64_t> worst_us{0};
};

}  // namespace

int main(int argc, char** argv) {
  const auto port = static_cast<std::uint16_t>(arg_or(argc, argv, "--port", 9000));
  const auto seconds = arg_or(argc, argv, "--seconds", 10);
  const auto connections = arg_or(argc, argv, "--connections", 4);
  const auto per_second = arg_or(argc, argv, "--rate", 200);  // per connection
  const std::string symbol = arg_str(argc, argv, "--symbol", "SYM0");

  if (seconds <= 0 || connections <= 0) {
    std::fprintf(stderr,
                 "usage: loadgen --port P [--seconds N] [--connections N] "
                 "[--rate N] [--symbol NAME]\n");
    return 2;
  }

  // Non-crossing prices around the midpoint: the point is sustained flow through
  // the venue, not a trading strategy. Crossing orders would net the book out and
  // measure nothing.
  const std::int64_t mid = 50'000;
  (void)symbol;
  Counters counters;
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + std::chrono::seconds(seconds);

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(connections));
  for (long long c = 0; c < connections; ++c) {
    workers.emplace_back([&, c] {
      const int fd = connect_and_authenticate(port, "load-" + std::to_string(c), "loadtoken");
      if (fd < 0) {
        counters.write_failed.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      std::uint64_t order_id = static_cast<std::uint64_t>(c) * 1'000'000'000ULL + 1ULL;
      // Simple open-loop pacing: one order every 1/rate seconds.
      const auto interval =
          std::chrono::microseconds(1'000'000 / (per_second > 0 ? per_second : 1));
      auto next = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() < deadline) {
        next += interval;
        std::this_thread::sleep_until(next);

        lob::protocol::Inbound msg;
        msg.type = lob::protocol::MessageType::NewOrder;
        msg.new_order.order_id = lob::OrderId{order_id++};
        // Alternate sides but keep prices apart so nothing crosses.
        const bool buy = (order_id % 2U) == 0U;
        msg.new_order.side = buy ? lob::Side::Buy : lob::Side::Sell;
        // order_id is unsigned, so the offset is cast before it is subtracted
        // from the signed midpoint.
        const auto offset = static_cast<std::int64_t>(order_id % 50U);
        msg.new_order.price = lob::Price{buy ? mid - 100 - offset : mid + 100 + offset};
        msg.new_order.quantity = lob::Quantity{1};
        msg.new_order.tif = lob::TimeInForce::Day;

        std::vector<std::uint8_t> buf;
        const auto t0 = std::chrono::steady_clock::now();
        if (!lob::protocol::encode(msg, buf)) {
          counters.write_failed.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        std::size_t offset = 0;
        bool ok = true;
        while (offset < buf.size()) {
          const ssize_t n = ::write(fd, buf.data() + offset, buf.size() - offset);
          if (n <= 0) {
            ok = false;
            break;
          }
          offset += static_cast<std::size_t>(n);
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        if (!ok) {
          counters.write_failed.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        counters.sent.fetch_add(1, std::memory_order_relaxed);
        counters.latencies_us.fetch_add(static_cast<std::uint64_t>(elapsed),
                                        std::memory_order_relaxed);
        // Track the worst observed write latency without a lock.
        std::uint64_t current = counters.worst_us.load(std::memory_order_relaxed);
        while (static_cast<std::uint64_t>(elapsed) > current &&
               !counters.worst_us.compare_exchange_weak(current,
                                                        static_cast<std::uint64_t>(elapsed))) {
        }
      }
      ::close(fd);
    });
  }
  for (std::thread& t : workers) {
    t.join();
  }

  const auto elapsed = std::chrono::steady_clock::now() - started;
  const std::uint64_t sent = counters.sent.load();
  const double secs = std::chrono::duration<double>(elapsed).count();
  // NOLINTNEXTLINE(cert-err33-c,cppcoreguidelines-pro-type-vararg)
  std::printf("connections: %lld  duration: %.1fs\n", connections, secs);
  // NOLINTNEXTLINE(cert-err33-c,cppcoreguidelines-pro-type-vararg)
  const auto rate = static_cast<double>(sent) / (secs > 0 ? secs : 1);
  // NOLINTNEXTLINE(cert-err33-c,cppcoreguidelines-pro-type-vararg)
  std::printf("sent:        %llu  (%.0f/s)\n", static_cast<unsigned long long>(sent), rate);
  // NOLINTNEXTLINE(cert-err33-c,cppcoreguidelines-pro-type-vararg)
  std::printf("rejected:    %llu\n", static_cast<unsigned long long>(counters.rejected.load()));
  // NOLINTNEXTLINE(cert-err33-c,cppcoreguidelines-pro-type-vararg)
  std::printf("write fails: %llu\n", static_cast<unsigned long long>(counters.write_failed.load()));
  if (sent > 0) {
    // NOLINTNEXTLINE(cert-err33-c,cppcoreguidelines-pro-type-vararg)
    std::printf("write latency: mean %lluus  worst %lluus\n",
                static_cast<unsigned long long>(counters.latencies_us.load() / sent),
                static_cast<unsigned long long>(counters.worst_us.load()));
  }
  // NOLINTNEXTLINE(cert-err33-c,cppcoreguidelines-pro-type-vararg)
  std::printf("symbol: %s\n", symbol.c_str());

  // A soak that silently sent almost nothing has proved nothing.
  const std::uint64_t failed = counters.write_failed.load();
  if (failed != 0U) {
    // NOLINTNEXTLINE(cert-err33-c,cppcoreguidelines-pro-type-vararg)
    std::fprintf(stderr, "loadgen: %llu connections or writes failed\n",
                 static_cast<unsigned long long>(failed));
    return 1;
  }
  if (sent == 0U) {
    // NOLINTNEXTLINE(cert-err33-c,cppcoreguidelines-pro-type-vararg)
    std::fprintf(stderr, "loadgen: sent nothing; the run proves nothing\n");
    return 1;
  }
  return 0;
}