// End-to-end over real TCP.
//
// Everything else in this repo was tested in isolation: a book, an engine, a
// codec, a session machine, a reactor. None of it was ever running together, and
// that gap is where the bugs live.
//
// Shutdown is tested first and on its own, before any feature test. The previous
// attempt at this file built everything, then discovered the acceptor thread
// could not be joined and hung every single test. A property the whole program
// depends on should be the first thing proven, not the last.

#include "venue/venue.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace lob {
namespace {

SymbolConfig sym_cfg(std::string_view name) {
  SymbolConfig c;
  c.name = name;
  c.min_price = 0;
  c.max_price = 10'000;
  c.tick_size = 1;
  c.lot_size = 1;
  c.max_order_qty = 1'000'000;
  c.max_notional = 1'000'000'000;
  c.max_open_orders = 4096;
  return c;
}

/// A venue on its own thread with a real listening socket.
class VenueFixture {
 public:
  explicit VenueFixture(std::vector<SymbolConfig> symbols, std::size_t shards = 2) {
    VenueConfig cfg;
    cfg.symbols = std::move(symbols);
    cfg.shards = shards;
    venue_ = std::make_unique<Venue>(std::move(cfg));
    port_ = venue_->listen_on(0);
    thread_ = std::thread([this] { venue_->run(); });
  }

  ~VenueFixture() {
    shutdown();
  }

  /// Stop and join with a deadline. Returns false if the thread would not exit,
  /// which is a failure to report rather than a hang to sit in.
  bool shutdown(std::chrono::milliseconds limit = std::chrono::milliseconds(3000)) {
    if (venue_ == nullptr) {
      return true;
    }
    venue_->stop();
    if (thread_.joinable()) {
      // join() has no timeout, so the exit is verified by the flag first and the
      // join is then expected to return immediately.
      const auto deadline = std::chrono::steady_clock::now() + limit;
      while (!venue_->stopped() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!venue_->stopped()) {
        return false;
      }
      thread_.join();
    }
    venue_->shutdown();
    venue_.reset();
    return true;
  }

  /// Stop the loop and join the shards so the book can be read without racing.
  /// state_hash() reaches into the shard engines directly, so calling it while the
  /// venue is still serving reads memory the workers are mutating. That is not
  /// theoretical: under concurrent runs the recovered hash mismatched about one
  /// time in six, because the "before" hash was captured mid-processing.
  void quiesce() {
    venue_->stop();
    if (thread_.joinable()) {
      thread_.join();
    }
    venue_->engine().drain();
  }

  template <typename Predicate>
  bool wait_for(Predicate predicate,
                std::chrono::milliseconds limit = std::chrono::milliseconds(4000)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
  }

  [[nodiscard]] std::uint16_t port() const {
    return port_;
  }
  [[nodiscard]] Venue& venue() {
    return *venue_;
  }

 private:
  std::unique_ptr<Venue> venue_;
  std::thread thread_;
  std::uint16_t port_ = 0;
};

// ---------------------------------------------------------------------------
// Shutdown, first
// ---------------------------------------------------------------------------

TEST(VenueShutdown, AcceptorThreadStartsAndStopsCleanly) {
  VenueFixture f({sym_cfg("XYZ")});
  ASSERT_NE(f.port(), 0) << "could not bind a port";
  EXPECT_FALSE(f.venue().stopped());
  EXPECT_TRUE(f.shutdown()) << "the acceptor thread did not exit within the deadline";
}

TEST(VenueShutdown, StopsCleanlyWithALiveConnection) {
  // A connection mid-session must not keep the loop alive.
  VenueFixture f({sym_cfg("XYZ")});
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(f.port());
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0);
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().stats().connections_accepted >= 1; }));
  ::close(fd);
  EXPECT_TRUE(f.shutdown());
}

TEST(VenueShutdown, ShutdownIsIdempotent) {
  // Double shutdown must be a no-op rather than a double close: a fixture that
  // calls it and a destructor that calls it again is the normal pattern.
  VenueConfig cfg;
  cfg.symbols = {sym_cfg("XYZ")};
  cfg.shards = 1;
  Venue venue{std::move(cfg)};
  ASSERT_NE(venue.listen_on(0), 0u);
  venue.shutdown();
  venue.shutdown();
  SUCCEED();
}

// ---------------------------------------------------------------------------
// Handshake and trading
// ---------------------------------------------------------------------------

protocol::Inbound hello(std::string key) {
  protocol::Inbound m;
  m.type = protocol::MessageType::Hello;
  m.session_id = std::move(key);
  return m;
}

protocol::Inbound auth(std::string key, std::string token) {
  protocol::Inbound m;
  m.type = protocol::MessageType::Authenticate;
  m.session_id = std::move(key);
  m.participant_token = std::move(token);
  return m;
}

protocol::Inbound order(ParticipantId pid, OrderId id, Side side, Price px, Quantity qty,
                        SymbolId symbol = SymbolId{0}) {
  protocol::Inbound m;
  m.type = protocol::MessageType::NewOrder;
  m.new_order.order_id = id;
  m.new_order.participant = pid;
  m.new_order.symbol = symbol;
  m.new_order.side = side;
  m.new_order.type = OrderType::Limit;
  m.new_order.tif = TimeInForce::GTC;
  m.new_order.price = px;
  m.new_order.quantity = qty;
  return m;
}

protocol::Inbound subscribe(SymbolId symbol) {
  protocol::Inbound m;
  m.type = protocol::MessageType::Subscribe;
  m.subscribe_symbol = symbol;
  return m;
}

/// A blocking client that speaks the wire protocol directly, so a codec mistake
/// cannot cancel against a matching mistake in the test.
class TestClient {
 public:
  explicit TestClient(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
      std::abort();
    }
    // Non-blocking, and it matters: drain() polls until a deadline, and on a
    // blocking socket the read simply waits forever once the venue has sent
    // everything it has. Every test that never read a byte hid this.
    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      std::abort();
    }
  }
  ~TestClient() {
    if (fd_ >= 0) {
      ::close(fd_);
    }
  }

  void send(const protocol::Inbound& message) {
    std::vector<std::uint8_t> buf;
    if (!protocol::encode(message, buf)) {
      std::abort();
    }
    send_bytes(reinterpret_cast<const char*>(buf.data()), buf.size());
  }

  void send_bytes(const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
      const ssize_t n = ::write(fd_, data + sent, len - sent);
      if (n <= 0) {
        return;  // peer went away
      }
      sent += static_cast<std::size_t>(n);
    }
  }

  std::string drain(std::chrono::milliseconds wait = std::chrono::milliseconds(200)) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < deadline) {
      char buf[65536];
      const ssize_t n = ::read(fd_, buf, sizeof(buf));
      if (n > 0) {
        out.append(buf, static_cast<std::size_t>(n));
        continue;
      }
      if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        break;  // the venue closed the connection
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return out;
  }

 private:
  int fd_ = -1;
};

std::vector<protocol::Inbound> decode_all(const std::string& bytes) {
  std::vector<protocol::Inbound> out;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    std::size_t consumed = 0;
    protocol::DecodeError error = protocol::DecodeError::None;
    const auto m = protocol::decode(std::string_view(bytes).substr(offset), consumed, error);
    if (!m) {
      break;
    }
    out.push_back(*m);
    offset += consumed;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Recovery
//
// The point of these is that M3's journal is not a library nothing calls. A venue
// that journals, dies, and comes back with a bit-identical book is the only proof
// that "recovery" is a property of the product rather than of a unit test.
// ---------------------------------------------------------------------------

/// A venue that journals to `path`.
class JournalledVenue {
 public:
  JournalledVenue(std::string path, bool recover, std::vector<SymbolConfig> symbols) {
    VenueConfig cfg;
    cfg.symbols = std::move(symbols);
    cfg.shards = 2;
    cfg.journal_path = path;
    cfg.recover_from_journal = recover;
    venue_ = std::make_unique<Venue>(std::move(cfg));
    // listen_on opens the journal (replaying first when asked) and binds; the loop
    // still has to run, or nothing is ever accepted, processed or answered.
    port_ = venue_->listen_on(0);
    thread_ = std::thread([this] { venue_->run(); });
  }
  ~JournalledVenue() {
    venue_->stop();
    if (thread_.joinable()) {
      thread_.join();
    }
    // shutdown() is what closes the journal, so a recovery test would otherwise
    // read back a log that was never flushed.
    venue_->shutdown();
    venue_.reset();
  }

  [[nodiscard]] std::uint16_t port() const {
    return port_;
  }
  [[nodiscard]] Venue& venue() {
    return *venue_;
  }

  /// See VenueFixture::quiesce(): state_hash() races the shard workers unless the
  /// engine has been drained first.
  void quiesce() {
    venue_->stop();
    if (thread_.joinable()) {
      thread_.join();
    }
    venue_->engine().drain();
  }

  template <typename Predicate>
  bool wait_for(Predicate predicate,
                std::chrono::milliseconds limit = std::chrono::milliseconds(4000)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
  }

 private:
  std::unique_ptr<Venue> venue_;
  std::thread thread_;
  std::uint16_t port_ = 0;
};

std::vector<SymbolConfig> one_symbol() {
  return {sym_cfg("XYZ")};
}

/// Unique per process as well as per test. A fixed name made these tests collide
/// with any overlapping run -- two ctest invocations, or a rerun while a previous
/// one was still shutting down -- and the loser would recover from the other's
/// journal and fail with a confusing hash mismatch. That showed up as an
/// intermittent failure that would not reproduce.
std::string temp_journal_path(const char* tag) {
  return std::string("/tmp/lob_e2e_") + std::to_string(::getpid()) + "_" + tag + ".journal";
}

TEST(VenueRecovery, AJournalledVenueReplaysIntoAnIdenticalBook) {
  const std::string path = temp_journal_path("basic");
  ::unlink(path.c_str());

  std::uint64_t before = 0;
  {
    JournalledVenue first(path, false, one_symbol());
    ASSERT_NE(first.port(), 0) << "could not bind";
    ASSERT_TRUE(first.venue().journal_healthy());

    TestClient client(first.port());
    client.send(hello("s"));
    client.send(auth("s", "t"));
    for (int i = 1; i <= 12; ++i) {
      client.send(order(ParticipantId{0}, OrderId{static_cast<std::uint64_t>(i)},
                        i % 2 == 0 ? Side::Buy : Side::Sell, Price{100 + (i % 4)},
                        Quantity{static_cast<std::int64_t>(i % 5 + 1)}));
    }
    // Cancels and a replace, so recovery has to replay every record kind, not just
    // new orders.
    CancelRequest cx;
    cx.seq = Sequence{0};
    cx.ts = Timestamp{0};
    cx.symbol = SymbolId{0};
    cx.order_id = OrderId{3};
    cx.participant = ParticipantId{0};
    protocol::Inbound cancel;
    cancel.type = protocol::MessageType::Cancel;
    cancel.cancel = cx;
    client.send(cancel);

    protocol::Inbound rep;
    rep.type = protocol::MessageType::Replace;
    rep.replace.symbol = SymbolId{0};
    rep.replace.order_id = OrderId{5};
    rep.replace.participant = ParticipantId{0};
    rep.replace.new_quantity = Quantity{2};
    client.send(rep);

    ASSERT_TRUE(first.wait_for([&first] {
      const TopOfBook t = first.venue().top_of_book(SymbolId{0});
      return t.has_bid || t.has_ask;
    }));
    EXPECT_GT(first.venue().journal_records(), 0u) << "the venue journalled nothing";
    first.quiesce();
    before = first.venue().engine().state_hash();
  }  // graceful shutdown: flushes and fsyncs

  // A second venue, recovering from the same log.
  {
    JournalledVenue second(path, true, one_symbol());
    ASSERT_NE(second.port(), 0);
    second.quiesce();
    EXPECT_EQ(second.venue().engine().state_hash(), before)
        << "the recovered book differs from the one that was journalled";
  }
  ::unlink(path.c_str());
}

/// Recovery must be complete before the venue serves. A half-applied book is worse
/// than not starting, so the recovered state has to be visible without waiting for
/// traffic to push it through the shards.
TEST(VenueRecovery, RecoveredStateIsVisibleBeforeAnyTraffic) {
  const std::string path = temp_journal_path("ready");
  ::unlink(path.c_str());
  std::uint64_t before = 0;
  {
    JournalledVenue first(path, false, one_symbol());
    TestClient client(first.port());
    client.send(hello("s"));
    client.send(auth("s", "t"));
    client.send(order(ParticipantId{0}, OrderId{1}, Side::Buy, Price{77}, Quantity{9}));
    ASSERT_TRUE(
        first.wait_for([&first] { return first.venue().top_of_book(SymbolId{0}).has_bid; }));
    before = first.venue().engine().state_hash();
  }
  {
    JournalledVenue second(path, true, one_symbol());
    // The book must already be right, with no client and no polling: that is what
    // proves the drain-at-startup actually applied the log before serving began.
    EXPECT_EQ(second.venue().engine().state_hash(), before);
    // top_of_book is asynchronous (it asks the loop for a snapshot), so the wait
    // here is about the snapshot arriving, not about the book being built.
    ASSERT_TRUE(
        second.wait_for([&second] { return second.venue().top_of_book(SymbolId{0}).has_bid; }));
    const TopOfBook t = second.venue().top_of_book(SymbolId{0});
    ASSERT_TRUE(t.has_bid);
    EXPECT_EQ(t.best_bid.value, 77);
    EXPECT_EQ(t.best_bid_qty.value, 9);
  }
  ::unlink(path.c_str());
}

/// A truncated log must recover everything before the tear rather than refusing
/// to start. A crash mid-append is normal, not exceptional.
/// Two restarts, not one. Replaying a log and then truncating it looks fine after a
/// single restart -- the book is right and nobody looks again. It is the *second*
/// restart that finds the history gone, so that is the one worth a test.
TEST(VenueRecovery, SurvivesASecondRestart) {
  const std::string path = temp_journal_path("twice");
  ::unlink(path.c_str());
  std::uint64_t after_first = 0;
  std::uint64_t after_second = 0;
  {
    JournalledVenue first(path, false, one_symbol());
    TestClient client(first.port());
    client.send(hello("s"));
    client.send(auth("s", "t"));
    client.send(order(ParticipantId{0}, OrderId{1}, Side::Buy, Price{10}, Quantity{1}));
    client.send(order(ParticipantId{0}, OrderId{2}, Side::Buy, Price{11}, Quantity{2}));
    ASSERT_TRUE(first.wait_for([&first] {
      const TopOfBook t = first.venue().top_of_book(SymbolId{0});
      return t.has_bid;
    }));
    after_first = first.venue().engine().state_hash();
  }
  {
    JournalledVenue second(path, true, one_symbol());
    EXPECT_EQ(second.venue().engine().state_hash(), after_first)
        << "the first restart did not reproduce the book";
    TestClient client(second.port());
    client.send(hello("s"));
    client.send(auth("s", "t"));
    client.send(order(ParticipantId{0}, OrderId{3}, Side::Buy, Price{12}, Quantity{3}));
    ASSERT_TRUE(second.wait_for(
        [&second] { return second.venue().top_of_book(SymbolId{0}).best_bid.value == 12; }));
    after_second = second.venue().engine().state_hash();
  }
  {
    // If the writer truncated on open, this venue recovers only order 3 and the
    // hash below will not match.
    JournalledVenue third(path, true, one_symbol());
    EXPECT_EQ(third.venue().engine().state_hash(), after_second)
        << "the second restart lost pre-crash history: the log was truncated on open";
    ASSERT_TRUE(third.wait_for([&third] {
      const TopOfBook t = third.venue().top_of_book(SymbolId{0});
      return t.has_bid && t.best_bid.value == 12;
    }));
  }
  ::unlink(path.c_str());
}

TEST(VenueRecovery, ATornTailRecoversEverythingBeforeIt) {
  const std::string path = temp_journal_path("torn");
  ::unlink(path.c_str());
  std::size_t full_size = 0;
  {
    JournalledVenue first(path, false, one_symbol());
    TestClient client(first.port());
    client.send(hello("s"));
    client.send(auth("s", "t"));
    for (int i = 1; i <= 8; ++i) {
      // Far apart, or they cross and net out to an empty book. Recovery is about a
      // resting book, and the first torn-tail draft crossed by accident.
      client.send(order(ParticipantId{0}, OrderId{static_cast<std::uint64_t>(i)},
                        i % 2 == 0 ? Side::Buy : Side::Sell, i % 2 == 0 ? Price{50} : Price{500},
                        Quantity{1}));
    }
    ASSERT_TRUE(first.wait_for([&first] {
      const TopOfBook t = first.venue().top_of_book(SymbolId{0});
      return t.has_bid || t.has_ask;
    }));
  }
  {
    // Read, tear, write back.
    std::string bytes;
    {
      const int fd = ::open(path.c_str(), O_RDONLY);
      ASSERT_GE(fd, 0);
      char buf[4096];
      while (true) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) {
          break;
        }
        bytes.append(buf, static_cast<std::size_t>(n));
      }
      ::close(fd);
    }
    full_size = bytes.size();
    ASSERT_GT(full_size, 8u);
    bytes.resize(full_size - 5);  // mid-record tear
    const int fd = ::open(path.c_str(), O_WRONLY | O_TRUNC);
    ASSERT_GE(fd, 0);
    // Checked rather than (void)-cast: write() is warn_unused_result and GCC
    // rejects the discarded form outright, so a short write here would otherwise
    // go unnoticed and the test would silently stop testing what it claims.
    ASSERT_EQ(::write(fd, bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
    ::close(fd);

    JournalledVenue second(path, true, one_symbol());
    ASSERT_NE(second.port(), 0);
    // Recovery must not refuse to start, and must have applied a prefix.
    EXPECT_TRUE(second.venue().journal_healthy());
  }
  ::unlink(path.c_str());
}

TEST(VenueE2E, DecodesAHandshakeOnARealSocket) {
  VenueFixture f({sym_cfg("XYZ")});
  TestClient client(f.port());
  client.send(hello("s1"));
  client.send(auth("s1", "t"));
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().stats().frames_decoded >= 2u; }))
      << "the handshake frames were never decoded";
  EXPECT_GE(f.venue().stats().connections_accepted, 1u);
}

TEST(VenueE2E, AnOrderReachesTheEngineAndChangesTheBook) {
  VenueFixture f({sym_cfg("XYZ")});
  TestClient client(f.port());
  client.send(hello("s"));
  client.send(auth("s", "t"));
  client.send(order(ParticipantId{0}, OrderId{1}, Side::Buy, Price{100}, Quantity{5}));
  ASSERT_TRUE(f.wait_for([&f] {
    const TopOfBook tob = f.venue().top_of_book(SymbolId{0});
    return tob.has_bid && tob.best_bid.value == 100 && tob.best_bid_qty.value == 5;
  })) << "the order never reached the engine";
}

TEST(VenueE2E, TwoClientsTradeWithEachOther) {
  VenueFixture f({sym_cfg("XYZ")});
  TestClient maker(f.port());
  TestClient taker(f.port());
  maker.send(hello("m"));
  maker.send(auth("m", "t"));
  taker.send(hello("k"));
  taker.send(auth("k", "t"));
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().stats().connections_accepted >= 2u; }));

  maker.send(order(ParticipantId{0}, OrderId{1}, Side::Sell, Price{101}, Quantity{3}));
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().top_of_book(SymbolId{0}).has_ask; }));
  taker.send(order(ParticipantId{0}, OrderId{2}, Side::Buy, Price{101}, Quantity{3}));
  ASSERT_TRUE(f.wait_for([&f] {
    const TopOfBook tob = f.venue().top_of_book(SymbolId{0});
    return !tob.has_ask && !tob.has_bid;
  })) << "the two orders never crossed";
}

TEST(VenueE2E, AnOrderBeforeAuthenticateIsRefused) {
  VenueFixture f({sym_cfg("XYZ")});
  TestClient client(f.port());
  client.send(order(ParticipantId{0}, OrderId{1}, Side::Buy, Price{100}, Quantity{1}));
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().stats().requests_refused >= 1u; }))
      << "an unauthenticated order was not refused";
}

TEST(VenueE2E, GarbageDoesNotDisturbOtherClients) {
  VenueFixture f({sym_cfg("XYZ")});
  TestClient bad(f.port());
  TestClient good(f.port());
  const char junk[] = "\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff\xff";
  bad.send_bytes(junk, sizeof(junk) - 1);
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().stats().protocol_errors >= 1u; }))
      << "garbage was not detected as a protocol error";

  good.send(hello("g"));
  good.send(auth("g", "t"));
  good.send(order(ParticipantId{0}, OrderId{1}, Side::Buy, Price{50}, Quantity{2}));
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().top_of_book(SymbolId{0}).has_bid; }))
      << "one bad connection disturbed a good one";
}

TEST(VenueE2E, TwoSymbolsLandOnDifferentShardsWithSeparateBooks) {
  VenueFixture f({sym_cfg("AAA"), sym_cfg("BBB")}, 2);
  ASSERT_EQ(f.venue().engine().shard_count(), 2u);
  TestClient client(f.port());
  client.send(hello("s"));
  client.send(auth("s", "t"));
  client.send(order(ParticipantId{0}, OrderId{1}, Side::Buy, Price{10}, Quantity{1}, SymbolId{0}));
  client.send(order(ParticipantId{0}, OrderId{2}, Side::Sell, Price{20}, Quantity{2}, SymbolId{1}));
  ASSERT_TRUE(f.wait_for([&f] {
    return f.venue().top_of_book(SymbolId{0}).has_bid && f.venue().top_of_book(SymbolId{1}).has_ask;
  })) << "both shards did not receive their symbol's order";
  // Each symbol's book must be its own, not merged.
  EXPECT_FALSE(f.venue().top_of_book(SymbolId{0}).has_ask);
  EXPECT_FALSE(f.venue().top_of_book(SymbolId{1}).has_bid);
}

TEST(VenueE2E, ASubscribedClientGetsASnapshotThenIncrements) {
  VenueFixture f({sym_cfg("XYZ")});
  TestClient trader(f.port());
  TestClient watcher(f.port());
  trader.send(hello("t"));
  trader.send(auth("t", "t"));
  watcher.send(hello("w"));
  watcher.send(auth("w", "t"));

  // Build a book before anyone subscribes, so the snapshot has content.
  trader.send(order(ParticipantId{0}, OrderId{1}, Side::Buy, Price{100}, Quantity{5}));
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().top_of_book(SymbolId{0}).has_bid; }));

  watcher.send(subscribe(SymbolId{0}));
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().stats().subscriptions >= 1u; }));
  const auto first = decode_all(watcher.drain());
  ASSERT_FALSE(first.empty()) << "the subscriber received nothing";
  EXPECT_EQ(first.front().type, protocol::MessageType::MarketDataSnapshot);
  EXPECT_EQ(first.front().snapshot.best_bid, 100);

  for (int i = 2; i <= 6; ++i) {
    trader.send(order(ParticipantId{0}, OrderId{static_cast<std::uint64_t>(i)}, Side::Buy,
                      Price{100 + i}, Quantity{1}));
  }
  ASSERT_TRUE(
      f.wait_for([&f] { return f.venue().top_of_book(SymbolId{0}).best_bid.value == 106; }));

  std::string more;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  while (std::chrono::steady_clock::now() < deadline) {
    more += watcher.drain(std::chrono::milliseconds(50));
    const auto decoded = decode_all(more);
    if (!decoded.empty() && decoded.back().type == protocol::MessageType::MarketDataIncrement) {
      break;
    }
  }
  const auto later = decode_all(more);
  ASSERT_FALSE(later.empty()) << "no increments arrived";
  EXPECT_EQ(later.front().type, protocol::MessageType::MarketDataIncrement);
  // Strictly after the snapshot's sequence: the boundary must hold end to end.
  EXPECT_GT(later.front().increment.sequence, first.front().snapshot.sequence);
}

TEST(VenueE2E, ManyClientsUnderLoadLeaveTheBookUncrossed) {
  VenueFixture f({sym_cfg("XYZ")}, 2);
  constexpr int kClients = 6;
  constexpr int kPerClient = 200;
  std::vector<std::unique_ptr<TestClient>> clients;
  for (int i = 0; i < kClients; ++i) {
    clients.push_back(std::make_unique<TestClient>(f.port()));
    clients.back()->send(hello("s" + std::to_string(i)));
    clients.back()->send(auth("s" + std::to_string(i), "t"));
  }
  ASSERT_TRUE(f.wait_for([&f] {
    return f.venue().stats().connections_accepted >= static_cast<std::uint64_t>(kClients);
  }));

  for (int c = 0; c < kClients; ++c) {
    for (int i = 0; i < kPerClient; ++i) {
      const OrderId id{static_cast<std::uint64_t>(c * kPerClient + i + 1)};
      const bool buy = (i % 2) == 0;
      clients[static_cast<std::size_t>(c)]->send(order(
          ParticipantId{0}, id, buy ? Side::Buy : Side::Sell, Price{100 + (i % 7)}, Quantity{1}));
    }
  }

  constexpr std::uint64_t total = static_cast<std::uint64_t>(kClients) * (kPerClient + 2);
  ASSERT_TRUE(f.wait_for([&f] { return f.venue().stats().frames_decoded >= total; },
                         std::chrono::milliseconds(10000)))
      << "not every frame was decoded";
  EXPECT_FALSE(f.venue().engine().event_overflow())
      << "a shard's event ring overflowed, so market data was lost";
  EXPECT_FALSE(f.venue().top_of_book(SymbolId{0}).crossed())
      << "the book was left crossed under concurrent load";
}

}  // namespace
}  // namespace lob
