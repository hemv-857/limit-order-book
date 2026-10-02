// Suppressions, all for the same three reasons:
//  - reinterpret_cast of a sockaddr to the AF-specific struct: dictated by the
//    socket API, which takes `struct sockaddr*`.
//  - htons/ntohs/accept: C varargs, no C++ overload exists.
//  - static_cast from Connection& to VenueConnection&: safe and correct here --
//    VenueConnection has exactly one base, no virtuals, and the reactor only ever
//    holds pointers the owner minted.
//
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg)
// NOLINTBEGIN(cppcoreguidelines-pro-type-static-cast-downcast)

#include "venue/venue.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace lob {
namespace {

[[nodiscard]] bool would_block(int err) noexcept {
  return err == EAGAIN || err == EWOULDBLOCK;
}

}  // namespace

Venue::Venue(VenueConfig config)
    : config_(std::move(config)),
      sequencer_(config_.shards == 0 ? 1 : config_.shards),
      engine_(config_.symbols, config_.shards == 0 ? 1 : config_.shards),
      publisher_(config_.market_data) {}

Venue::~Venue() {
  shutdown();
}

void Venue::shutdown() {
  stop();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
  for (auto& [id, conn] : connections_) {
    (void)id;
    if (conn.fd >= 0) {
      reactor_.release(conn.fd);
      ::close(conn.fd);
      conn.fd = -1;
    }
  }
  connections_.clear();
  // Last: the shard workers exit only once draining is set and their queues are
  // empty, and a request still in flight must be applied before we return.
  engine_.drain();
}

Connection* Venue::connection(std::uint64_t id) {
  const auto it = connections_.find(id);
  return it == connections_.end() ? nullptr : &it->second;
}

std::uint16_t Venue::listen_on(std::uint16_t port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    return 0;
  }
  // Non-blocking, and not optional: accept_ready() loops until accept() refuses,
  // which on a blocking listener means the second accept() waits forever for a
  // connection that may never arrive. The acceptor loop then cannot be stopped,
  // which is precisely how this hung.
  const int listener_flags = ::fcntl(listen_fd_, F_GETFL, 0);
  if (listener_flags < 0 || ::fcntl(listen_fd_, F_SETFL, listener_flags | O_NONBLOCK) < 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return 0;
  }
  int one = 1;
  (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return 0;
  }
  if (::listen(listen_fd_, 128) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return 0;
  }
  socklen_t len = sizeof(addr);
  if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
    return 0;
  }
  port_ = ntohs(addr.sin_port);
  return port_;
}

void Venue::on_accept(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    ::close(fd);
    return;
  }
  int one = 1;
  (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  const std::uint64_t id = next_id_++;
  VenueConnection& c = connections_.emplace(id, VenueConnection{}).first->second;
  c.id = id;
  c.fd = fd;
  c.session = Session(id, config_.session, 0);
  (void)reactor_.manage(fd, id);
  stats_.connections_accepted.fetch_add(1, std::memory_order_relaxed);
}

void Venue::accept_ready() {
  while (true) {
    const int fd = ::accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      return;  // EAGAIN: drained
    }
    on_accept(fd);
  }
}

void Venue::on_readable(Connection& raw) {
  auto& conn = static_cast<VenueConnection&>(raw);
  // The reactor accumulates raw bytes in the connection's inbox; the frame reader
  // is what turns them into messages. Handing the buffer over and clearing it is
  // the join between the two -- without it bytes arrive and are never decoded.
  conn.reader.append(conn.inbox);
  conn.inbox.clear();

  protocol::DecodeError error = protocol::DecodeError::None;
  while (true) {
    const auto message = conn.reader.next(error);
    if (!message) {
      if (error != protocol::DecodeError::Truncated) {
        stats_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
        close_connection(conn, to_string(error));
      }
      return;
    }
    stats_.frames_decoded.fetch_add(1, std::memory_order_relaxed);
    handle_message(conn, *message);
    if (!connections_.contains(conn.id)) {
      return;  // the message closed it
    }
  }
}

void Venue::on_writable(Connection& connection) {
  flush(static_cast<VenueConnection&>(connection));
}

void Venue::on_closed(Connection& connection) {
  close_connection(static_cast<VenueConnection&>(connection), "peer closed");
}

void Venue::handle_message(VenueConnection& conn, const protocol::Inbound& message) {
  const SessionVerdict verdict = conn.session.on_message(message, 0);

  switch (message.type) {
    case protocol::MessageType::Hello:
    case protocol::MessageType::Authenticate:
      // Nothing is acknowledged on the wire: the client learns it is authenticated
      // by its next request being accepted rather than refused.
      if (!verdict.accepted) {
        stats_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
        close_connection(conn, to_string(verdict.error));
      }
      return;

    case protocol::MessageType::Subscribe: {
      if (!verdict.accepted) {
        stats_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
        close_connection(conn, to_string(verdict.error));
        return;
      }
      // Ask the owning shard. Reading the book here would race its worker.
      SubscribeRequest req;
      req.seq = sequencer_.next_sequence();
      req.ts = sequencer_.next_timestamp();
      req.symbol = message.subscribe_symbol;
      req.session_id = conn.id;
      if (!engine_.submit(req)) {
        close_connection(conn, "shard unavailable");
        return;
      }
      conn.pending_subscriptions.push_back(message.subscribe_symbol);
      return;
    }

    case protocol::MessageType::NewOrder:
    case protocol::MessageType::Cancel:
    case protocol::MessageType::Replace:
    case protocol::MessageType::MassCancel: {
      if (!verdict.accepted) {
        stats_.requests_refused.fetch_add(1, std::memory_order_relaxed);
        close_connection(conn, to_string(verdict.error));
        return;
      }
      if (submit_to_engine(*&message)) {
        stats_.requests_submitted.fetch_add(1, std::memory_order_relaxed);
      } else {
        stats_.requests_refused.fetch_add(1, std::memory_order_relaxed);
        close_connection(conn, "shard queue full");
      }
      return;
    }

    case protocol::MessageType::Heartbeat:
      return;

    case protocol::MessageType::Goodbye:
      close_connection(conn, "client said goodbye");
      return;

    case protocol::MessageType::MarketDataSnapshot:
    case protocol::MessageType::MarketDataIncrement:
      // Server -> client only. A client sending one is confused or hostile.
      stats_.protocol_errors.fetch_add(1, std::memory_order_relaxed);
      close_connection(conn, "server-only message from client");
      return;
  }
}

bool Venue::submit_to_engine(const protocol::Inbound& message) {
  switch (message.type) {
    case protocol::MessageType::NewOrder: {
      NewOrderRequest r = message.new_order;
      // Sequence and timestamp are the gateway's to assign, never the client's.
      r.seq = sequencer_.next_sequence();
      r.ts = sequencer_.next_timestamp();
      return engine_.submit(r);
    }
    case protocol::MessageType::Cancel: {
      CancelRequest r = message.cancel;
      r.seq = sequencer_.next_sequence();
      r.ts = sequencer_.next_timestamp();
      return engine_.submit(r);
    }
    case protocol::MessageType::Replace: {
      ReplaceRequest r = message.replace;
      r.seq = sequencer_.next_sequence();
      r.ts = sequencer_.next_timestamp();
      return engine_.submit(r);
    }
    case protocol::MessageType::MassCancel: {
      MassCancelRequest r = message.mass_cancel;
      r.seq = sequencer_.next_sequence();
      r.ts = sequencer_.next_timestamp();
      return engine_.submit(r);
    }
    default:
      return false;
  }
}

TopOfBook Venue::top_of_book(SymbolId symbol) noexcept {
  request_top_of_book(symbol);
  const std::lock_guard<std::mutex> lock(tops_mutex_);
  const auto it = last_tops_.find(symbol.value);
  return it == last_tops_.end() ? TopOfBook{} : it->second;
}

void Venue::request_top_of_book(SymbolId symbol) noexcept {
  SubscribeRequest req;
  req.seq = sequencer_.next_sequence();
  req.ts = sequencer_.next_timestamp();
  req.symbol = symbol;
  req.session_id = 0;  // 0: an observation, not a subscription
  (void)engine_.submit(req);
}

void Venue::pump_snapshots() {
  const std::vector<ShardSnapshot> answers = engine_.take_snapshots();
  for (const ShardSnapshot& a : answers) {
    {
      const std::lock_guard<std::mutex> lock(tops_mutex_);
      last_tops_[a.symbol.value] = a.book;
    }
    if (a.session_id == 0) {
      continue;  // an observation, not a subscription
    }
    for (auto& [id, c] : connections_) {
      (void)id;
      const auto it =
          std::find(c.pending_subscriptions.begin(), c.pending_subscriptions.end(), a.symbol);
      if (it == c.pending_subscriptions.end()) {
        continue;
      }
      c.pending_subscriptions.erase(it);
      if (publisher_.subscribe(SessionId{c.id}, a.symbol,
                               make_snapshot(a.symbol, a.book, a.sequence))) {
        stats_.subscriptions.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

void Venue::pump_market_data() {
  event_scratch_ = engine_.take_events();
  for (const Event& e : event_scratch_) {
    publisher_.publish(e);
  }

  // Collect connections that must go, then remove them after the walk. Closing
  // erases from this same map, and doing that inside the iteration is undefined
  // behaviour -- which shows up as a spin, not as a crash.
  to_close_.clear();
  for (auto& [id, c] : connections_) {
    (void)id;
    if (publisher_.is_dropped(SessionId{c.id})) {
      to_close_.push_back(&c);
      continue;
    }
    const std::vector<std::uint8_t>& outbox = publisher_.outbox(SessionId{c.id});
    if (!outbox.empty()) {
      c.outbox.append(reinterpret_cast<const char*>(outbox.data()), outbox.size());
      publisher_.take(SessionId{c.id}, market_data_scratch_);
      flush(c);  // queued is not sent: the bytes still have to reach the socket
    }
  }
  for (VenueConnection* c : to_close_) {
    stats_.slow_consumers_dropped.fetch_add(1, std::memory_order_relaxed);
    close_connection(*c, "slow consumer");
  }
}

void Venue::flush(VenueConnection& conn) {
  while (!conn.outbox.empty()) {
    const std::string_view chunk = conn.outbox.readable();
    const ssize_t n = ::write(conn.fd, chunk.data(), chunk.size());
    if (n > 0) {
      // Exactly what the kernel took. Never inferred, never guessed.
      conn.outbox.consume(static_cast<std::size_t>(n));
      continue;
    }
    if (would_block(errno)) {
      reactor_.set_write_interest(conn.fd, true);
      return;
    }
    close_connection(conn, "write failed");
    return;
  }
  reactor_.set_write_interest(conn.fd, false);
}

void Venue::close_connection(VenueConnection& conn, std::string_view why) {
  (void)why;
  const auto it = connections_.find(conn.id);
  if (it == connections_.end()) {
    return;
  }
  reactor_.release(conn.fd);
  publisher_.remove(SessionId{conn.id});
  if (conn.fd >= 0) {
    ::close(conn.fd);
    conn.fd = -1;
  }
  connections_.erase(it);
}

void Venue::poll_once(int timeout_ms) {
  if (listen_fd_ >= 0) {
    struct pollfd pfd{};
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN) != 0) {
      accept_ready();
    }
  }
  pump_snapshots();
  pump_market_data();
  (void)reactor_.poll_once(timeout_ms);
}

void Venue::run() {
  // The exit condition is owned here and nowhere else. A loop whose stop flag
  // nothing reads can never be joined, which is how this hung the first time.
  while (!reactor_.stopped()) {
    struct pollfd pfd{};
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;
    if (listen_fd_ >= 0 && ::poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN) != 0) {
      accept_ready();
    }
    pump_snapshots();
    pump_market_data();
    (void)reactor_.poll_once(0);
  }
}

// NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
// NOLINTEND(cppcoreguidelines-pro-type-vararg)
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace lob