#include "gateway/reactor.hpp"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace lob {
namespace {

// ---------------------------------------------------------------------------
// WriteQueue: partial-write accounting
// ---------------------------------------------------------------------------

TEST(WriteQueue, StartsEmpty) {
  WriteQueue q;
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
  EXPECT_EQ(q.readable().size(), 0u);
  EXPECT_FALSE(q.wants_write());
}

TEST(WriteQueue, AppendIsVisibleOldestFirst) {
  WriteQueue q;
  q.append("hello");
  q.append(" world");
  EXPECT_EQ(q.readable(), "hello world");
  EXPECT_EQ(q.size(), 11u);
}

TEST(WriteQueue, ConsumeAdvancesTheFront) {
  WriteQueue q;
  q.append("hello world");
  q.consume(6);
  EXPECT_EQ(q.readable(), "world") << "a partial write must leave the remainder queued";
  EXPECT_EQ(q.size(), 5u);
  q.consume(5);
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.readable().size(), 0u);
}

/// The property that matters most: a short write followed by more writes must
/// reassemble to exactly the original byte stream. Any double-send or skipped
/// chunk shows up here.
TEST(WriteQueue, ShortWritesReassembleExactly) {
  std::string original;
  for (int i = 0; i < 1000; ++i) {
    original += static_cast<char>('a' + (i % 26));
  }
  WriteQueue q;
  q.append(original);

  std::string received;
  // Write in uneven chunks, including single bytes.
  const std::size_t pattern[] = {1, 7, 1, 100, 3, 4096, 1, 250};
  std::size_t i = 0;
  std::size_t p = 0;
  while (!q.empty()) {
    const std::size_t want = pattern[p++ % 8];
    const std::size_t n = std::min(want, q.readable().size());
    received.append(q.readable().substr(0, n));
    q.consume(n);
    ++i;
  }
  EXPECT_EQ(received, original) << "stream corrupted after " << i << " writes";
}

TEST(WriteQueue, OverReportingIsClampedNotTrusted) {
  // A caller that claims to have written more than it did would skip unsent
  // bytes and desynchronise the stream with no other symptom.
  WriteQueue q;
  q.append("abcdef");
  q.consume(100);
  EXPECT_TRUE(q.empty());
}

TEST(WriteQueue, ConsumeZeroLeavesEverythingQueued) {
  WriteQueue q;
  q.append("abc");
  q.consume(0);
  EXPECT_EQ(q.readable(), "abc");
}

TEST(WriteQueue, EmptyQueueConsumeIsSafe) {
  WriteQueue q;
  q.consume(5);
  EXPECT_TRUE(q.empty());
}

TEST(WriteQueue, CompactionReclaimsSpaceWithoutReordering) {
  WriteQueue q;
  std::string expect;
  for (int round = 0; round < 200; ++round) {
    std::string chunk(64, static_cast<char>('A' + (round % 26)));
    expect += chunk;
    q.append(chunk);
    // Consume a variable fraction, forcing the dead prefix to grow.
    q.consume(1 + static_cast<std::size_t>(round % 60));
  }
  EXPECT_EQ(q.readable(), expect.substr(expect.size() - q.size()));
  q.compact();
  EXPECT_EQ(q.readable(), expect.substr(expect.size() - q.size()))
      << "compaction must not reorder or drop bytes";
}

// ---------------------------------------------------------------------------
// Reactor over a real socketpair
// ---------------------------------------------------------------------------

/// A handler that just records what the reactor told it and owns its connections.
class RecordingHandler : public ReactorHandler {
 public:
  Connection* connection(std::uint64_t id) override {
    const auto it = connections_.find(id);
    return it == connections_.end() ? nullptr : &it->second;
  }
  void on_accept(int) override {}
  void on_readable(Connection& c) override {
    ++readable_calls;
    last_inbox_size = c.inbox.size();
  }
  void on_writable(Connection& c) override {
    ++writable_calls;
    (void)c;
  }
  void on_closed(Connection& c) override {
    ++closed_calls;
    (void)c;
  }

  std::unordered_map<std::uint64_t, Connection> connections_;
  std::size_t last_inbox_size = 0;
  int readable_calls = 0;
  int writable_calls = 0;
  int closed_calls = 0;
};

class SocketPair {
 public:
  SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_) != 0) {
      std::abort();
    }
    for (int fd : fds_) {
      const int flags = ::fcntl(fd, F_GETFL, 0);
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
  }
  ~SocketPair() {
    for (int fd : fds_) {
      if (fd >= 0) {
        ::close(fd);
      }
    }
  }
  int client() const {
    return fds_[0];
  }
  int server() const {
    return fds_[1];
  }
  void set_send_buffer(int bytes) {
    ::setsockopt(server(), SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
  }

 private:
  int fds_[2] = {-1, -1};
};

TEST(Reactor, BackendIsSelectedAtCompileTime) {
  // Documented as a compile-time choice; this pins which one this host got.
#if defined(__linux__)
  EXPECT_FALSE(reactor_uses_kqueue());
#else
  EXPECT_TRUE(reactor_uses_kqueue());
#endif
}

TEST(Reactor, DeliversAFrameWrittenByThePeer) {
  SocketPair pair;
  RecordingHandler handler;
  Reactor reactor(handler);
  handler.connections_.emplace(
      1, Connection{1, pair.server(), {}, {}, false, false, Session(1, SessionConfig{}, 0)});
  ASSERT_TRUE(reactor.manage(pair.server(), 1));

  const std::string payload = "LO\x01\x03\x00\x00\x00\x00";
  ASSERT_EQ(::write(pair.client(), payload.data(), payload.size()),
            static_cast<ssize_t>(payload.size()));

  EXPECT_GT(reactor.poll_once(500), 0);
  EXPECT_GE(handler.readable_calls, 1);
  EXPECT_EQ(handler.connections_[1].inbox, payload);
}

TEST(Reactor, ReadableIsReportedInSeveralPiecesForALargeWrite) {
  // More than one read chunk, so the reactor must accumulate rather than assume a
  // whole frame arrives at once.
  SocketPair pair;
  RecordingHandler handler;
  Reactor reactor(handler);
  handler.connections_.emplace(
      1, Connection{1, pair.server(), {}, {}, false, false, Session(1, SessionConfig{}, 0)});
  ASSERT_TRUE(reactor.manage(pair.server(), 1));

  const std::string payload(3 * Reactor::kReadChunk, 'x');
  std::size_t written = 0;
  while (written < payload.size()) {
    const ssize_t n = ::write(pair.client(), payload.data() + written, payload.size() - written);
    if (n > 0) {
      written += static_cast<std::size_t>(n);
    } else {
      break;
    }
  }
  ASSERT_GT(written, static_cast<std::size_t>(Reactor::kReadChunk))
      << "test did not send enough to require multiple reads";

  for (int i = 0; i < 8 && handler.connections_[1].inbox.size() < written; ++i) {
    if (reactor.poll_once(200) <= 0) {
      break;
    }
  }
  EXPECT_EQ(handler.connections_[1].inbox.size(), written)
      << "bytes were lost or duplicated across reads";
}

TEST(Reactor, PartialWriteThenTheRestIsSentWithoutDuplication) {
  // The whole point of WriteQueue, exercised through a real socket: shrink the
  // send buffer so the kernel accepts only part of the payload, then write again.
  SocketPair pair;
  pair.set_send_buffer(2048);
  RecordingHandler handler;
  Reactor reactor(handler);
  Connection& c =
      handler.connections_
          .emplace(
              1, Connection{1, pair.server(), {}, {}, false, false, Session(1, SessionConfig{}, 0)})
          .first->second;

  const std::string payload(64 * 1024, 'z');
  c.outbox.append(payload);

  std::string received;
  for (int attempt = 0; attempt < 4096 && !c.outbox.empty(); ++attempt) {
    const std::string_view chunk = c.outbox.readable();
    if (chunk.empty()) {
      break;
    }
    const ssize_t n = ::write(pair.client(), chunk.data(), chunk.size());
    if (n > 0) {
      c.outbox.consume(static_cast<std::size_t>(n));
    }
    // Count only what the *peer* actually received. Recording the write as well
    // would count the same bytes twice and hide exactly the duplication this
    // test exists to catch.
    char drain[64 * 1024];
    const ssize_t d = ::read(pair.server(), drain, sizeof(drain));
    if (d > 0) {
      received.append(drain, static_cast<std::size_t>(d));
    }
  }
  EXPECT_TRUE(c.outbox.empty()) << "queue did not drain";
  EXPECT_EQ(received.size(), payload.size());
  EXPECT_EQ(received, payload) << "partial writes duplicated or skipped bytes";
}

TEST(Reactor, PeerCloseIsReported) {
  SocketPair pair;
  RecordingHandler handler;
  Reactor reactor(handler);
  handler.connections_.emplace(
      1, Connection{1, pair.server(), {}, {}, false, false, Session(1, SessionConfig{}, 0)});
  ASSERT_TRUE(reactor.manage(pair.server(), 1));
  ::close(pair.client());
  // Closing the client twice is harmless for the test but the reactor must not
  // hang waiting for events that will not come.
  bool closed = false;
  for (int i = 0; i < 10 && !closed; ++i) {
    reactor.poll_once(100);
    closed = handler.closed_calls > 0;
  }
  EXPECT_TRUE(closed) << "peer close was never reported";
}

TEST(Reactor, PollOnAnIdleSocketReturnsWithoutEvents) {
  SocketPair pair;
  RecordingHandler handler;
  Reactor reactor(handler);
  handler.connections_.emplace(
      1, Connection{1, pair.server(), {}, {}, false, false, Session(1, SessionConfig{}, 0)});
  ASSERT_TRUE(reactor.manage(pair.server(), 1));
  EXPECT_EQ(reactor.poll_once(10), 0);
  EXPECT_EQ(handler.readable_calls, 0);
  EXPECT_EQ(handler.closed_calls, 0);
}

TEST(Reactor, ReleasedConnectionsAreNoLongerDispatched) {
  SocketPair pair;
  RecordingHandler handler;
  Reactor reactor(handler);
  handler.connections_.emplace(
      1, Connection{1, pair.server(), {}, {}, false, false, Session(1, SessionConfig{}, 0)});
  ASSERT_TRUE(reactor.manage(pair.server(), 1));
  reactor.release(pair.server());

  const std::string payload = "hello";
  ASSERT_EQ(::write(pair.client(), payload.data(), payload.size()),
            static_cast<ssize_t>(payload.size()));
  reactor.poll_once(100);
  EXPECT_EQ(handler.readable_calls, 0);
}

TEST(Reactor, ConnectionLookupForAnUnknownIdReturnsNull) {
  RecordingHandler handler;
  handler.connections_.emplace(1, Connection{1, 3, {}, {}, false, false, Session(1, {}, 0)});
  EXPECT_NE(handler.connection(1), nullptr);
  EXPECT_EQ(handler.connection(999), nullptr);
}

}  // namespace
}  // namespace lob