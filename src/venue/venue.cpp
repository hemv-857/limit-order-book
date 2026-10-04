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
#include <array>
#include <cerrno>
#include <cstdio>
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
  // Close the journal first so the log is fsynced even if a later step misbehaves.
  // This is what makes a *graceful* restart lossless; a crash can still lose
  // whatever was buffered when the process died.
  (void)journal_.close();
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

namespace {

/// Read a whole file. Used only at startup for recovery, so simplicity beats
/// streaming.
std::string read_file(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return {};
  }
  std::string out;
  std::array<char, 65536> buf{};
  while (true) {
    const ssize_t n = ::read(fd, buf.data(), buf.size());
    if (n > 0) {
      out.append(buf.data(), static_cast<std::size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    break;
  }
  ::close(fd);
  return out;
}

}  // namespace

bool Venue::open_journal() {
  if (config_.journal_path.empty()) {
    return false;
  }
  if (config_.recover_from_journal) {
    // Replay BEFORE opening for append. The writer opens with O_TRUNC, so opening
    // first would destroy the very log being recovered from.
    const std::string bytes = read_file(config_.journal_path);
    if (!bytes.empty()) {
      // Replay through the shards rather than a standalone Engine: that is where
      // the book lives, and submitting in journal order reproduces it exactly.
      std::size_t applied = 0;
      bool ok = true;
      const ReplayReport report = replay(bytes, [&](const JournalRecord& r) {
        bool sent = false;
        switch (r.kind) {
          case RecordKind::NewOrder:
            sent = engine_.submit(r.new_order);
            break;
          case RecordKind::Cancel:
            sent = engine_.submit(r.cancel);
            break;
          case RecordKind::Replace:
            sent = engine_.submit(r.replace);
            break;
          case RecordKind::MassCancel:
            sent = engine_.submit(r.mass_cancel);
            break;
          case RecordKind::Invalid:
            break;
        }
        if (sent) {
          ++applied;
        } else {
          ok = false;  // a shard refused: the recovered book would not match
        }
      });
      // Recovery must *complete* before we serve: a half-applied book is worse than
      // not starting. Wait for the shards to consume the replay, but do NOT drain():
      // drain() joins the workers permanently, so the venue would come up with a
      // correct book and no engine behind it to serve the next request.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      while (!engine_.idle()) {
        if (std::chrono::steady_clock::now() > deadline) {
          // NOLINTNEXTLINE(cert-err33-c) -- a failed log line is not actionable here.
          std::fprintf(stderr, "venue: recovery did not settle within 30s\n");
          return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      // NOLINTNEXTLINE(cert-err33-c) -- a failed log line is not actionable here.
      std::fprintf(stderr, "venue: recovered %zu of %zu journal records%s%s\n", applied,
                   report.records_applied, report.truncated ? " (torn tail)" : "",
                   ok ? "" : " -- SHARD REFUSED A RECORD");
    }
  }
  return journal_.open(config_.journal_path, config_.recover_from_journal);
}

std::uint16_t Venue::listen_on(std::uint16_t port) {
  (void)open_journal();
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
    // handle_message can close the connection, which erases it from connections_
    // and frees the very object `conn` refers to. Liveness therefore has to be
    // checked through an id captured *before* the call: the previous guard read
    // conn.id afterwards, which is a read of freed memory. ASan caught exactly
    // that as a heap-use-after-free in __hash_table.
    const std::uint64_t id = conn.id;
    handle_message(conn, *message);
    if (!connections_.contains(id)) {
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
  if (!routed_body(message)) {
    return false;
  }
  // Journal only what the shard accepted. Note this claims nothing: submit()
  // already takes and releases the shard's in-flight slot internally, and claiming
  // again here left the count permanently elevated, so drain never completed.
  if (journal_.is_open()) {
    (void)journal_.append(scratch_record_);
  }
  return true;
}

bool Venue::routed_body(const protocol::Inbound& message) {
  switch (message.type) {
    case protocol::MessageType::NewOrder: {
      NewOrderRequest r = message.new_order;
      // Sequence and timestamp are the gateway's to assign, never the client's.
      r.seq = sequencer_.next_sequence();
      r.ts = sequencer_.next_timestamp();
      scratch_record_.kind = RecordKind::NewOrder;
      scratch_record_.new_order = r;
      return engine_.submit(r);
    }
    case protocol::MessageType::Cancel: {
      CancelRequest r = message.cancel;
      r.seq = sequencer_.next_sequence();
      r.ts = sequencer_.next_timestamp();
      scratch_record_.kind = RecordKind::Cancel;
      scratch_record_.cancel = r;
      return engine_.submit(r);
    }
    case protocol::MessageType::Replace: {
      ReplaceRequest r = message.replace;
      r.seq = sequencer_.next_sequence();
      r.ts = sequencer_.next_timestamp();
      scratch_record_.kind = RecordKind::Replace;
      scratch_record_.replace = r;
      return engine_.submit(r);
    }
    case protocol::MessageType::MassCancel: {
      MassCancelRequest r = message.mass_cancel;
      r.seq = sequencer_.next_sequence();
      r.ts = sequencer_.next_timestamp();
      scratch_record_.kind = RecordKind::MassCancel;
      scratch_record_.mass_cancel = r;
      return engine_.submit(r);
    }
    default:
      return false;
  }
}

TopOfBook Venue::top_of_book(SymbolId symbol) {
  request_top_of_book(symbol);
  const std::lock_guard<std::mutex> lock(tops_mutex_);
  const auto it = last_tops_.find(symbol.value);
  return it == last_tops_.end() ? TopOfBook{} : it->second;
}

void Venue::request_top_of_book(SymbolId symbol) {
  // Queue it rather than submitting here. The shard queues are SPSC and the
  // acceptor loop is their only producer, so an observer submitting from its own
  // thread raced the loop's submissions on the same ring: TSan reported the slot
  // write in push() against the worker's read in pop(), and beyond the race two
  // producers can claim the same head and lose or duplicate a request.
  const std::lock_guard<std::mutex> lock(pending_tops_mutex_);
  pending_tops_.push_back(symbol);
}

void Venue::submit_pending_tops() {
  std::vector<SymbolId> pending;
  {
    const std::lock_guard<std::mutex> lock(pending_tops_mutex_);
    pending.swap(pending_tops_);
  }
  for (const SymbolId symbol : pending) {
    SubscribeRequest req;
    req.seq = sequencer_.next_sequence();
    req.ts = sequencer_.next_timestamp();
    req.symbol = symbol;
    req.session_id = 0;  // 0: an observation, not a subscription
    (void)engine_.submit(req);
  }
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
    submit_pending_tops();
    pump_snapshots();
    pump_market_data();
    (void)reactor_.poll_once(0);
  }
}

// NOLINTEND(cppcoreguidelines-pro-type-static-cast-downcast)
// NOLINTEND(cppcoreguidelines-pro-type-vararg)
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

}  // namespace lob