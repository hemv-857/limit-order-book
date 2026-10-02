#include "gateway/session.hpp"

#include <gtest/gtest.h>

namespace lob {
namespace {

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

protocol::Inbound order(ParticipantId pid) {
  protocol::Inbound m;
  m.type = protocol::MessageType::NewOrder;
  m.new_order.order_id = OrderId{1};
  m.new_order.participant = pid;
  m.new_order.side = Side::Buy;
  m.new_order.price = Price{100};
  m.new_order.quantity = Quantity{1};
  return m;
}

SessionConfig cfg() {
  SessionConfig c;
  c.hello_timeout = 1000;
  c.auth_timeout = 1000;
  c.heartbeat_timeout = 3000;
  c.heartbeat_interval = 500;
  return c;
}

Session ready_session(std::uint64_t id, SessionConfig c) {
  Session s(id, c, 0);
  s.on_message(hello("k"), 0);
  s.on_message(auth("k", "tok"), 0);
  return s;
}

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST(Session, StartsConnected) {
  Session s(1, cfg(), 0);
  EXPECT_EQ(s.state(), SessionState::Connected);
  EXPECT_FALSE(s.ready());
  EXPECT_GT(s.deadline(), 0) << "a connection must have a handshake deadline";
}

TEST(Session, HelloThenAuthenticateReachesReady) {
  Session s(1, cfg(), 0);
  EXPECT_TRUE(s.on_message(hello("key"), 0).accepted);
  EXPECT_EQ(s.state(), SessionState::AwaitingAuth);
  EXPECT_TRUE(s.on_message(auth("key", "tok"), 10).accepted);
  EXPECT_EQ(s.state(), SessionState::Ready);
  EXPECT_TRUE(s.ready());
  EXPECT_EQ(s.deadline(), 0) << "a ready session has no handshake deadline";
}

TEST(Session, GoodbyeClosesCleanly) {
  Session s = ready_session(1, cfg());
  protocol::Inbound m;
  m.type = protocol::MessageType::Goodbye;
  const SessionVerdict v = s.on_message(m, 100);
  EXPECT_TRUE(v.close);
  EXPECT_EQ(s.state(), SessionState::Closed);
}

// ---------------------------------------------------------------------------
// Invariant 1: nothing reaches the engine unauthenticated
// ---------------------------------------------------------------------------

TEST(Session, OrdersBeforeAuthenticateAreRefusedAndClose) {
  // Every trading message type, from every pre-Ready state.
  for (protocol::MessageType t : {protocol::MessageType::NewOrder, protocol::MessageType::Cancel,
                                  protocol::MessageType::Replace, protocol::MessageType::MassCancel,
                                  protocol::MessageType::Subscribe}) {
    Session s(1, cfg(), 0);
    EXPECT_EQ(s.state(), SessionState::Connected);
    protocol::Inbound m;
    m.type = t;
    const SessionVerdict v = s.on_message(m, 0);
    EXPECT_EQ(v.error, SessionError::NotAuthenticated) << to_string(t);
    EXPECT_FALSE(v.accepted) << to_string(t);
    EXPECT_TRUE(v.close) << to_string(t);
    EXPECT_EQ(s.state(), SessionState::Closed) << to_string(t);
  }
}

TEST(Session, OrderAfterHelloButBeforeAuthenticateIsStillRefused) {
  Session s(1, cfg(), 0);
  ASSERT_TRUE(s.on_message(hello("k"), 0).accepted);
  EXPECT_EQ(s.state(), SessionState::AwaitingAuth);
  const SessionVerdict v = s.on_message(order(ParticipantId{1}), 5);
  EXPECT_EQ(v.error, SessionError::NotAuthenticated);
  EXPECT_EQ(s.state(), SessionState::Closed);
}

TEST(Session, ClosedSessionRejectsEverything) {
  Session s = ready_session(1, cfg());
  s.close();
  EXPECT_FALSE(s.on_message(order(ParticipantId{1}), 200).accepted);
  EXPECT_TRUE(s.on_message(order(ParticipantId{1}), 300).close);
}

// ---------------------------------------------------------------------------
// Invariant 2: the participant is the session's
// ---------------------------------------------------------------------------

TEST(Session, OrderClaimingAnotherParticipantIsRefused) {
  Session s = ready_session(1, cfg());
  const SessionVerdict v = s.on_message(order(ParticipantId{99}), 10);
  EXPECT_EQ(v.error, SessionError::ParticipantMismatch);
  EXPECT_FALSE(v.accepted);
  EXPECT_TRUE(v.close);
}

/// The specific hole a disjunction-over-all-fields check would leave open: a
/// session whose participant is 0 must not let a claim of 99 through because an
/// unrelated default-initialised field happened to match.
TEST(Session, ParticipantZeroDoesNotLetAnotherClaimThrough) {
  Session s(1, cfg(), 0);
  s.on_message(hello("k"), 0);
  s.on_message(auth("k", "tok"), 0);
  // participant_ is still the default 0 here, since the gateway has not bound one.
  ASSERT_TRUE(s.ready());

  const SessionVerdict v = s.on_message(order(ParticipantId{99}), 10);
  EXPECT_EQ(v.error, SessionError::ParticipantMismatch);
  EXPECT_TRUE(v.close);
}

TEST(Session, CancelClaimingAnotherParticipantIsRefused) {
  Session s = ready_session(1, cfg());
  protocol::Inbound m;
  m.type = protocol::MessageType::Cancel;
  m.cancel.participant = ParticipantId{7};
  const SessionVerdict v = s.on_message(m, 10);
  EXPECT_EQ(v.error, SessionError::ParticipantMismatch);
}

TEST(Session, SubscribeCarriesNoParticipantAndIsAccepted) {
  Session s = ready_session(1, cfg());
  protocol::Inbound m;
  m.type = protocol::MessageType::Subscribe;
  m.subscribe_symbol = SymbolId{0};
  EXPECT_TRUE(s.on_message(m, 10).accepted);
}

// ---------------------------------------------------------------------------
// Handshake ordering
// ---------------------------------------------------------------------------

TEST(Session, HelloAfterHelloIsRefused) {
  Session s(1, cfg(), 0);
  ASSERT_TRUE(s.on_message(hello("k"), 0).accepted);
  const SessionVerdict v = s.on_message(hello("k"), 1);
  EXPECT_EQ(v.error, SessionError::UnexpectedHello);
  EXPECT_TRUE(v.close);
}

TEST(Session, EmptySessionIdIsRefused) {
  Session s(1, cfg(), 0);
  EXPECT_EQ(s.on_message(hello(""), 0).error, SessionError::UnexpectedHello);
}

TEST(Session, AuthenticateBeforeHelloIsRefused) {
  Session s(1, cfg(), 0);
  EXPECT_EQ(s.on_message(auth("k", "t"), 0).error, SessionError::AuthenticationFailed);
}

TEST(Session, AuthenticateWithMismatchedSessionIdIsRefused) {
  Session s(1, cfg(), 0);
  ASSERT_TRUE(s.on_message(hello("k1"), 0).accepted);
  EXPECT_EQ(s.on_message(auth("k2", "t"), 0).error, SessionError::AuthenticationFailed);
}

TEST(Session, AuthenticateTwiceIsRefused) {
  Session s(1, cfg(), 0);
  ASSERT_TRUE(s.on_message(hello("k"), 0).accepted);
  ASSERT_TRUE(s.on_message(auth("k", "t"), 0).accepted);
  EXPECT_EQ(s.on_message(auth("k", "t"), 10).error, SessionError::AuthenticationFailed);
}

TEST(Session, ClientSendingServerOnlyMessagesIsRefused) {
  Session s = ready_session(1, cfg());
  for (protocol::MessageType t :
       {protocol::MessageType::MarketDataSnapshot, protocol::MessageType::MarketDataIncrement}) {
    Session fresh = ready_session(2, cfg());
    protocol::Inbound m;
    m.type = t;
    const SessionVerdict v = fresh.on_message(m, 10);
    EXPECT_EQ(v.error, SessionError::UnexpectedMessage) << to_string(t);
    EXPECT_TRUE(v.close);
  }
  EXPECT_TRUE(s.ready());
}

// ---------------------------------------------------------------------------
// Timeouts, on an injected clock so nothing sleeps
// ---------------------------------------------------------------------------

TEST(Session, HelloMustArriveInTime) {
  Session s(1, cfg(), 0);
  EXPECT_EQ(s.deadline(), 1000);
  s.on_tick(999);
  EXPECT_EQ(s.state(), SessionState::Connected) << "one tick before the deadline";
  const SessionVerdict v = s.on_tick(1000);
  EXPECT_EQ(v.error, SessionError::HelloTimeout);
  EXPECT_EQ(s.state(), SessionState::Closed);
}

TEST(Session, AuthenticateMustArriveInTime) {
  Session s(1, cfg(), 0);
  s.on_message(hello("k"), 0);
  const std::int64_t deadline = s.deadline();
  s.on_tick(deadline - 1);
  EXPECT_EQ(s.state(), SessionState::AwaitingAuth);
  EXPECT_EQ(s.on_tick(deadline).error, SessionError::AuthTimeout);
  EXPECT_EQ(s.state(), SessionState::Closed);
}

TEST(Session, HelloResetsTheDeadline) {
  Session s(1, cfg(), 0);
  s.on_tick(900);
  ASSERT_TRUE(s.on_message(hello("k"), 900).accepted);
  EXPECT_EQ(s.deadline(), 900 + 1000) << "the auth deadline starts at the Hello";
}

TEST(Session, HeartbeatIsEmittedOnInterval) {
  Session s = ready_session(1, cfg());
  EXPECT_FALSE(s.on_tick(100).send_heartbeat);
  EXPECT_TRUE(s.on_tick(500).send_heartbeat) << "due at 500ms";
  EXPECT_FALSE(s.on_tick(999).send_heartbeat);
  EXPECT_TRUE(s.on_tick(1000).send_heartbeat) << "and again one interval later";
}

TEST(Session, ReadySessionIsDroppedAfterHeartbeatTimeout) {
  Session s = ready_session(1, cfg());
  s.on_tick(2500);
  EXPECT_EQ(s.state(), SessionState::Ready) << "not yet too quiet";
  const SessionVerdict v = s.on_tick(3000);
  EXPECT_EQ(v.error, SessionError::HeartbeatTimeout);
  EXPECT_EQ(s.state(), SessionState::Closed);
}

TEST(Session, AnyTrafficKeepsAReadySessionAlive) {
  // Silence is only evidence of death if nothing is expected in the first place.
  Session s = ready_session(1, cfg());
  s.on_tick(2900);
  // A message at 2900 resets liveness without being a heartbeat itself.
  protocol::Inbound m;
  m.type = protocol::MessageType::Hello;  // refused, but it still proves liveness
  s.on_message(m, 2900);
  s.on_tick(3000);
  // The Hello closed the session, so rebuild and check the reset properly.
  Session t = ready_session(2, cfg());
  t.on_tick(2900);
  protocol::Inbound hb;
  hb.type = protocol::MessageType::Heartbeat;
  t.on_message(hb, 2900);
  t.on_tick(4000);
  EXPECT_EQ(t.state(), SessionState::Ready) << "a heartbeat at 2900 should push the deadline out";
}

TEST(Session, ServerHeartbeatsCanBeDisabled) {
  SessionConfig c = cfg();
  c.heartbeat_interval = 0;
  Session s = ready_session(1, c);
  EXPECT_FALSE(s.on_tick(10'000).send_heartbeat)
      << "with heartbeats disabled the server must not demand them";
  EXPECT_EQ(s.state(), SessionState::Ready) << "and must not drop a session for not sending them";
}

// ---------------------------------------------------------------------------
// Credential store
// ---------------------------------------------------------------------------

TEST(CredentialStore, ResolvesKnownTokensOnly) {
  CredentialStore creds;
  creds.add("tok-1", ParticipantId{7});
  const auto found = creds.lookup("tok-1");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->value, 7u);
  EXPECT_FALSE(creds.lookup("nope").has_value());
  EXPECT_FALSE(creds.lookup("").has_value());
}

TEST(CredentialStore, ASessionKeyCanOnlyBeClaimedOnce) {
  // Two connections sharing a key would each believe they own the session.
  CredentialStore creds;
  EXPECT_TRUE(creds.claim("key-1"));
  EXPECT_FALSE(creds.claim("key-1"));
  creds.release("key-1");
  EXPECT_TRUE(creds.claim("key-1")) << "release must make the key claimable again";
}

}  // namespace
}  // namespace lob