// The syscall structs below (epoll_event, kevent, struct iovec) are fixed-layout
// C types dictated by the OS ABI: they are declared as arrays and passed by
// pointer to the kernel, which is the only correct way to call them.
//
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays)
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-array-to-pointer-decay)

#include "gateway/reactor.hpp"

#include <cerrno>
#include <cstring>

#ifdef __linux__
#include <sys/epoll.h>
#include <unistd.h>
#else
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace lob {
namespace {

/// The live Connection for an fd, or nullptr if it has gone away. Every callback
/// goes through this so a connection dropped mid-poll is handled rather than
/// dereferenced.
[[nodiscard]] Connection* lookup(ReactorHandler& handler,
                                 const std::unordered_map<int, std::uint64_t>& ids, int fd) {
  const auto it = ids.find(fd);
  if (it == ids.end()) {
    return nullptr;
  }
  return handler.connection(it->second);
}

/// A read returning this means "nothing right now", not "failure". Treating it
/// as an error is how reactors disconnect healthy idle clients.
[[nodiscard]] bool would_block(int err) noexcept {
  return err == EAGAIN || err == EWOULDBLOCK;
}

}  // namespace

bool reactor_uses_kqueue() noexcept {
#ifdef __linux__
  return false;
#else
  return true;
#endif
}

#ifdef __linux__

Reactor::Reactor(ReactorHandler& handler)
    : handler_(handler), epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)) {}

Reactor::~Reactor() {
  if (epoll_fd_ >= 0) {
    ::close(epoll_fd_);
  }
}

bool Reactor::manage(int fd, std::uint64_t connection_id) {
  struct epoll_event ev{};
  ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) != 0) {
    return false;
  }
  ids_[fd] = connection_id;
  fds_[connection_id] = fd;
  return true;
}

void Reactor::release(int fd) {
  (void)::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  const auto it = ids_.find(fd);
  if (it != ids_.end()) {
    fds_.erase(it->second);
    ids_.erase(it);
  }
}

int Reactor::poll_once(int timeout_ms) {
  struct epoll_event events[64];
  const int n = ::epoll_wait(epoll_fd_, events, 64, timeout_ms);
  if (n < 0) {
    return would_block(errno) ? 0 : -1;
  }
  for (int i = 0; i < n; ++i) {
    const int fd = events[i].data.fd;
    const std::uint32_t mask = events[i].events;
    if ((mask & (EPOLLHUP | EPOLLERR)) != 0U) {
      if (Connection* c = lookup(handler_, ids_, fd)) {
        handler_.on_closed(*c);
      }
      continue;
    }
    if ((mask & EPOLLOUT) != 0U) {
      handle_writable(fd);
    }
    if ((mask & EPOLLIN) != 0U) {
      handle_readable(fd);
    }
  }
  return n;
}

#else  // kqueue

Reactor::Reactor(ReactorHandler& handler) : handler_(handler), kqueue_fd_(::kqueue()) {}

Reactor::~Reactor() {
  if (kqueue_fd_ >= 0) {
    ::close(kqueue_fd_);
  }
}

bool Reactor::manage(int fd, std::uint64_t connection_id) {
  struct kevent changes[2];
  EV_SET(&changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  // Writability is armed only when there is something to write, and disarmed
  // once drained. An always-armed write filter makes a level-triggered kqueue
  // spin at 100% CPU on an idle connection.
  EV_SET(&changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, 0, 0, 0, nullptr);
  if (::kevent(kqueue_fd_, changes, 1, nullptr, 0, nullptr) < 0) {
    return false;
  }
  ids_[fd] = connection_id;
  fds_[connection_id] = fd;
  return true;
}

void Reactor::release(int fd) {
  struct kevent changes[2];
  EV_SET(&changes[0], static_cast<uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  EV_SET(&changes[1], static_cast<uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
  (void)::kevent(kqueue_fd_, changes, 2, nullptr, 0, nullptr);
  const auto it = ids_.find(fd);
  if (it != ids_.end()) {
    fds_.erase(it->second);
    ids_.erase(it);
  }
}

/// Arm or disarm the write filter. Separate from `manage` because writability
/// comes and goes with the outbox.
// NOLINTBEGIN(readability-make-member-function-const) -- mutates the kqueue.
void Reactor::set_write_interest(int fd, bool wanted) {
  const short flags = static_cast<short>(wanted ? (EV_ADD | EV_CLEAR) : EV_DELETE);
  struct kevent change{};
  EV_SET(&change, static_cast<uintptr_t>(fd), EVFILT_WRITE, flags, 0, 0, nullptr);
  (void)::kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr);
}
// NOLINTEND(readability-make-member-function-const)

int Reactor::poll_once(int timeout_ms) {
  struct kevent events[64];
  struct timespec ts{};
  struct timespec* tsp = nullptr;
  if (timeout_ms >= 0) {
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1000000L;
    tsp = &ts;
  }
  const int n = ::kevent(kqueue_fd_, nullptr, 0, events, 64, tsp);
  if (n < 0) {
    return would_block(errno) ? 0 : -1;
  }
  for (int i = 0; i < n; ++i) {
    const int fd = static_cast<int>(events[i].ident);
    if (events[i].flags & EV_EOF) {
      // Peer closed. Still drain whatever it sent first: a clean shutdown often
      // carries a Goodbye frame in the same segment as the FIN.
      handle_readable(fd);
      if (Connection* c = lookup(handler_, ids_, fd)) {
        handler_.on_closed(*c);
      }
      continue;
    }
    if (events[i].filter == EVFILT_WRITE) {
      handle_writable(fd);
    } else {
      handle_readable(fd);
    }
  }
  return n;
}

#endif  // __linux__

void Reactor::handle_readable(int fd) {
  Connection* c = lookup(handler_, ids_, fd);
  if (c == nullptr) {
    return;
  }
  char buf[Reactor::kReadChunk];
  // Loop until the socket reports "not now". Level-triggered readiness means an
  // early return costs only another syscall, but looping means a burst arrives in
  // one wakeup rather than one wakeup per chunk.
  while (true) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n > 0) {
      c->inbox.append(buf, static_cast<std::size_t>(n));
      if (static_cast<std::size_t>(n) < sizeof(buf)) {
        break;  // drained for now
      }
      continue;
    }
    if (n == 0) {
      break;  // EOF, reported by the backend's hangup branch
    }
    if (would_block(errno)) {
      break;  // the normal "nothing right now"
    }
    handler_.on_closed(*c);
    return;
  }
  if (!c->inbox.empty()) {
    handler_.on_readable(*c);
  }
}

void Reactor::handle_writable(int fd) {
  if (Connection* c = lookup(handler_, ids_, fd)) {
    handler_.on_writable(*c);
  }
}

void Reactor::run() {
  running_ = true;
  while (running_) {
    if (poll_once(100) < 0) {
      break;
    }
  }
}

// NOLINTEND(cppcoreguidelines-pro-bounds-array-to-pointer-decay)
// NOLINTEND(cppcoreguidelines-avoid-c-arrays)

}  // namespace lob