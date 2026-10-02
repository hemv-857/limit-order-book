// Reactor: readiness-based I/O over epoll (Linux) or kqueue (macOS/BSD).
//
// One interface, two backends. Which one is compiled is decided by the
// preprocessor, not at runtime, because a production venue should not be
// discovering at startup that it picked the wrong one.
//
// The loop is deliberately simple and *level*-triggered. Level-triggered means
// the kernel re-reports readiness until it is drained, so a missed edge is a
// performance wart rather than a lost wakeup. Edge-triggered would demand a drain
// loop that cannot be interrupted, which is a well-known source of stuck
// connections; the extra syscalls are cheaper than that class of bug.
//
// The contract, and it is the part that is easy to get wrong:
//
//   - Reads and writes are non-blocking. EAGAIN/EWOULDBLOCK is the normal "not
//     ready", never an error, and never a reason to spin.
//   - A write may be short. `handle_write` loops until the queue drains or the
//     socket reports EAGAIN, and only then re-arms for writability.
//   - A read may be short. Bytes accumulate in the connection's read buffer and
//     are turned into frames as they complete; nothing is assumed to arrive
//     whole.

#pragma once

#include "gateway/session.hpp"
#include "gateway/write_queue.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lob {

/// Per-connection state owned by the reactor.
struct Connection {
  std::uint64_t id = 0;
  int fd = -1;
  WriteQueue outbox;
  /// Bytes received but not yet formed into a frame.
  std::string inbox;
  bool wants_write = false;
  bool closing = false;
  Session session;
};

/// Why the reactor asked the owner to do something.
enum class ReactorEvent : std::uint8_t {
  Readable,  ///< a complete frame is available in the connection's inbox
  Writable,  ///< the outbox can accept more, or has pending bytes
  Closed,    ///< peer closed or errored; the connection should be dropped
};

/// The reactor's owner: whatever holds the sockets and the engine.
class ReactorHandler {
 public:
  ReactorHandler() = default;
  ReactorHandler(const ReactorHandler&) = delete;
  ReactorHandler& operator=(const ReactorHandler&) = delete;
  ReactorHandler(ReactorHandler&&) = delete;
  ReactorHandler& operator=(ReactorHandler&&) = delete;
  virtual ~ReactorHandler() = default;

  /// The reactor hands every callback a reference to the *live* connection, so
  /// this is how it finds it. The reactor deliberately does not own connection
  /// state: an owner that can hand back a dangling or copied Connection is a
  /// source of bugs the reactor cannot defend against.
  [[nodiscard]] virtual Connection* connection(std::uint64_t id) = 0;

  /// A connection was accepted. The handler must register it by calling
  /// `reactor.manage(fd, id)`.
  virtual void on_accept(int fd) = 0;

  /// `bytes` were appended to the connection's inbox. The handler should consume
  /// whole frames from `inbox`.
  virtual void on_readable(Connection& connection) = 0;

  /// The connection's outbox may have been written in part. The handler should
  /// top it up from market data if it has any.
  virtual void on_writable(Connection& connection) = 0;

  /// The connection should be torn down.
  virtual void on_closed(Connection& connection) = 0;
};

/// True when the backend is kqueue rather than epoll. Only used by tests and by
/// the diagnostics banner.
[[nodiscard]] bool reactor_uses_kqueue() noexcept;

class Reactor {
 public:
  explicit Reactor(ReactorHandler& handler);
  ~Reactor();

  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;
  Reactor(Reactor&&) = delete;
  Reactor& operator=(Reactor&&) = delete;

  /// Take ownership of `fd`, which must already be non-blocking.
  /// Returns false if the fd could not be registered.
  bool manage(int fd, std::uint64_t connection_id);

  /// Stop watching `fd` and forget the connection. Does not close the fd.
  void release(int fd);

  /// Arm or disarm the write filter for `fd`.
  ///
  /// Public because the owner is the only thing that knows when there is pending
  /// output. An always-armed write filter makes a level-triggered kqueue spin at
  /// 100% CPU on an idle connection; an always-disarmed one never flushes a
  /// partially written frame. On epoll this is a no-op, since that backend has no
  /// separate write filter to toggle.
  void set_write_interest(int fd, bool wanted);

  /// Wait for readiness and dispatch. `timeout_ms` of -1 blocks indefinitely.
  /// Returns the number of connections dispatched.
  int poll_once(int timeout_ms);

  /// Run until `stop()` is called.
  void run();

  void stop() noexcept {
    running_.store(false, std::memory_order_release);
  }

  /// True once stop() has been called. An owner looping around poll_once() must
  /// check this: a stop flag nothing reads leaves the loop spinning, and joining
  /// that thread then hangs forever.
  [[nodiscard]] bool stopped() const noexcept {
    return !running_.load(std::memory_order_acquire);
  }

  /// Listener fds to poll. Handled by the owner; the reactor only watches
  /// connections it was told about.
  void add_listener(int fd) {
    listeners_.push_back(fd);
  }
  [[nodiscard]] const std::vector<int>& listeners() const noexcept {
    return listeners_;
  }

  /// Read size per `read()` call. Small enough that a test can force a short
  /// read by sending more than one chunk.
  static constexpr int kReadChunk = 4096;

 private:
  void handle_readable(int fd);
  void handle_writable(int fd);
#ifndef __linux__
  /// Arm or disarm the write filter. Writability comes and goes with the outbox,
  /// and an always-armed write filter spins a level-triggered kqueue at 100% CPU
  /// on an idle connection.
#endif

  ReactorHandler& handler_;
  /// Starts true: a constructed reactor is meant to be polled until stopped.
  /// Defaulting this to false made stopped() report true before anything ran, so
  /// an owner looping on it never entered the loop at all.
  ///
  /// Atomic, and it has to be: stop() is called from one thread and stopped() is
  /// read from the loop on another. As a plain bool the loop is free to hoist the
  /// read out entirely and spin forever on a value that has already changed --
  /// which is exactly what happened, and is invisible without a race detector.
  std::atomic<bool> running_{true};
  std::vector<int> listeners_;
  std::unordered_map<int, std::uint64_t> ids_;
  /// Connections live in the handler; the reactor keeps only fd -> id.
  std::unordered_map<std::uint64_t, int> fds_;
  int epoll_fd_ = -1;   ///< Linux only; -1 on kqueue
  int kqueue_fd_ = -1;  ///< BSD/macOS only; -1 on epoll
};

}  // namespace lob