// lobctl: a minimal client for the venue.
//
// Exists because OPERATIONS.md tells you how to start the venue and, without this,
// gives you no way to talk to it. Deliberately thin: it speaks the wire protocol
// directly rather than through the server's own encoder, so a codec bug on the
// server cannot cancel against a matching bug here.

// Same three suppressions as the venue binary, for the same reasons: the socket
// API takes `struct sockaddr*`, printf-family has no C++ overload, and a process
// entry point is not usefully noexcept.
//
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
// NOLINTBEGIN(bugprone-exception-escape)
// NOLINTBEGIN(concurrency-mt-unsafe)
// The remaining cert-err33-c hits are std::fflush on an interactive stream, where
// checking the return would mean handling a broken pipe as a fatal error on a tool
// whose whole job is printing to a terminal.
// NOLINTBEGIN(cert-err33-c)

#include "protocol/codec.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

int connect_to(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

bool send_frame(int fd, const lob::protocol::Inbound& m) {
  std::vector<std::uint8_t> buf;
  if (!lob::protocol::encode(m, buf)) {
    return false;
  }
  std::size_t sent = 0;
  while (sent < buf.size()) {
    const ssize_t n = ::write(fd, buf.data() + sent, buf.size() - sent);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

/// Throwaway order id for an interactive tool. A local PRNG rather than rand():
/// the analyser's objection to rand() is worth honouring even where it does not
/// matter, and a fixed seed would make a session reproducible by accident.
std::uint64_t next_order_id() {
  static std::mt19937_64 source(std::random_device{}());
  return (source() % 1'000'000ULL) + 1ULL;
}

int usage() {
  std::fprintf(stderr,
               "usage: lobctl --port <p> <command>\n"
               "\n"
               "  handshake                 hello + authenticate, then wait\n"
               "  subscribe <symbol>        subscribe and print the market data feed\n"
               "  buy  <symbol> <price> <qty> [order_id]\n"
               "  sell <symbol> <price> <qty> [order_id]\n"
               "  cancel <symbol> <order_id>\n"
               "  ping                      send a heartbeat\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    return usage();
  }
  const std::string_view port_arg(argv[1]);
  if (port_arg != "--port" || argc < 4) {
    return usage();
  }
  const auto port = static_cast<std::uint16_t>(std::strtoul(argv[2], nullptr, 10));
  const std::string_view command(argv[3]);

  const int fd = connect_to(port);
  if (fd < 0) {
    std::fprintf(stderr, "lobctl: could not connect to 127.0.0.1:%u\n", port);
    return 1;
  }

  // Handshake first: every later command is refused without it, which is the point
  // of the state machine.
  lob::protocol::Inbound hello;
  hello.type = lob::protocol::MessageType::Hello;
  hello.session_id = "lobctl";
  if (!send_frame(fd, hello)) {
    std::fprintf(stderr, "lobctl: could not send hello\n");
    ::close(fd);
    return 1;
  }
  lob::protocol::Inbound auth;
  auth.type = lob::protocol::MessageType::Authenticate;
  auth.session_id = "lobctl";
  auth.participant_token = "lobctl";
  if (!send_frame(fd, auth)) {
    std::fprintf(stderr, "lobctl: could not send authenticate\n");
    ::close(fd);
    return 1;
  }

  const auto need = [&](std::size_t n) { return static_cast<std::size_t>(argc) >= 4U + n; };

  if (command == "handshake") {
    (void)std::printf("lobctl: handshake sent for session 'lobctl'\n");
  } else if (command == "ping") {
    lob::protocol::Inbound m;
    m.type = lob::protocol::MessageType::Heartbeat;
    if (!send_frame(fd, m)) {
      std::fprintf(stderr, "lobctl: could not send heartbeat\n");
      ::close(fd);
      return 1;
    }
    (void)std::printf("lobctl: heartbeat sent\n");
  } else if (command == "subscribe" && need(1U)) {
    lob::protocol::Inbound m;
    m.type = lob::protocol::MessageType::Subscribe;
    m.subscribe_symbol.value = static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10));
    if (!send_frame(fd, m)) {
      std::fprintf(stderr, "lobctl: could not send subscribe\n");
      ::close(fd);
      return 1;
    }
    // Stream the feed until interrupted. Print every frame so the snapshot/increment
    // boundary is visible in the output.
    (void)std::printf("lobctl: subscribed to symbol %s, streaming (ctrl-c to stop)\n", argv[4]);
    std::fflush(stdout);
    std::vector<char> buffer(65536);
    lob::protocol::FrameReader reader;
    while (true) {
      const ssize_t n = ::read(fd, buffer.data(), buffer.size());
      if (n <= 0) {
        break;
      }
      reader.append(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
      while (true) {
        lob::protocol::DecodeError error = lob::protocol::DecodeError::None;
        const auto frame = reader.next(error);
        if (!frame) {
          break;
        }
        if (frame->type == lob::protocol::MessageType::MarketDataSnapshot) {
          const auto& k = frame->snapshot;
          (void)std::printf("snapshot seq=%llu symbol=%u bid=%s%lldx%lld ask=%s%lldx%lld\n",
                            static_cast<unsigned long long>(k.sequence), k.symbol.value,
                            k.has_bid ? "" : "-", static_cast<long long>(k.best_bid),
                            static_cast<long long>(k.best_bid_qty), k.has_ask ? "" : "-",
                            static_cast<long long>(k.best_ask),
                            static_cast<long long>(k.best_ask_qty));
        } else if (frame->type == lob::protocol::MessageType::MarketDataIncrement) {
          const auto& k = frame->increment;
          (void)std::printf("increment seq=%llu side=%s price=%lld qty=%lld action=%.*s\n",
                            static_cast<unsigned long long>(k.sequence),
                            k.side == lob::Side::Buy ? "buy" : "sell",
                            static_cast<long long>(k.price), static_cast<long long>(k.quantity),
                            static_cast<int>(lob::to_string(k.action).size()),
                            lob::to_string(k.action).data());
        } else {
          (void)std::printf("frame type=%.*s\n",
                            static_cast<int>(lob::protocol::to_string(frame->type).size()),
                            lob::protocol::to_string(frame->type).data());
        }
        std::fflush(stdout);
      }
    }
  } else if ((command == "buy" || command == "sell") && need(3U)) {
    lob::protocol::Inbound m;
    m.type = lob::protocol::MessageType::NewOrder;
    // argv is: command symbol price qty [order_id]. This was reading the *price*
    // argument, so every order silently got the price as its id.
    m.new_order.order_id.value = need(4U) ? std::strtoull(argv[7], nullptr, 10) : next_order_id();
    m.new_order.participant = lob::ParticipantId{0};
    m.new_order.symbol.value = static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10));
    m.new_order.side = command == "buy" ? lob::Side::Buy : lob::Side::Sell;
    m.new_order.type = lob::OrderType::Limit;
    m.new_order.tif = lob::TimeInForce::GTC;
    m.new_order.price.value = std::strtoll(argv[5], nullptr, 10);
    m.new_order.quantity.value = std::strtoll(argv[6], nullptr, 10);
    if (!send_frame(fd, m)) {
      std::fprintf(stderr, "lobctl: could not send order\n");
      ::close(fd);
      return 1;
    }
    (void)std::printf("lobctl: %.*s %s qty=%s price=%s sent, order_id=%llu\n",
                      static_cast<int>(command.size()), command.data(), argv[4], argv[6], argv[5],
                      static_cast<unsigned long long>(m.new_order.order_id.value));
  } else if (command == "cancel" && need(2U)) {
    lob::protocol::Inbound m;
    m.type = lob::protocol::MessageType::Cancel;
    m.cancel.order_id.value = std::strtoull(argv[5], nullptr, 10);
    m.cancel.participant = lob::ParticipantId{0};
    m.cancel.symbol.value = static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 10));
    if (!send_frame(fd, m)) {
      std::fprintf(stderr, "lobctl: could not send cancel\n");
      ::close(fd);
      return 1;
    }
    (void)std::printf("lobctl: cancel %s sent\n", argv[5]);
  } else {
    ::close(fd);
    return usage();
  }

  // Goodbye, so the venue logs a clean close rather than an error.
  lob::protocol::Inbound bye;
  bye.type = lob::protocol::MessageType::Goodbye;
  (void)send_frame(fd, bye);
  ::close(fd);
  return 0;
}

// NOLINTEND(cert-err33-c)
// NOLINTEND(concurrency-mt-unsafe)
// NOLINTEND(bugprone-exception-escape)
// NOLINTEND(cppcoreguidelines-pro-type-vararg)
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
