// Session state machine.
//
// A connection is a small, strict state machine. Strictness is the point: the
// sequence of things a client is allowed to do before it can touch the book is
// exactly the sequence that keeps one participant impersonating another.
//
//   Connected --Hello--> AwaitingAuth --Authenticate--> Ready --Goodbye--> Closed
//        |                     |                            |
//        +---------------------+----------------------------+--> Closed
//                              (any timeout, or protocol error)
//
// Two invariants the rest of the gateway relies on, both enforced here rather
// than at the call sites that might forget:
//
//  1. **No order can be accepted before `Ready`.** Every inbound message is
//     validated against the current state, so there is no path that reaches the
//     engine unauthenticated.
//  2. **The participant id is the session's, never the client's.** A request
//     naming a different participant is a protocol error, not something to
//     quietly rewrite: silently overwriting it would let a bug elsewhere become
//     an impersonation, and quietly accepting it would let an attacker.
//
// Time is injected rather than read from the clock so that every timeout is
// testable without sleeping.

#pragma once

#include "core/engine.hpp"
#include "protocol/codec.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace lob {

enum class SessionState : std::uint8_t {
  Connected,     ///< accepted a connection, nothing received yet
  AwaitingAuth,  ///< Hello done, waiting for Authenticate
  Ready,         ///< authenticated; may send orders
  Closed,        ///< terminal
};

[[nodiscard]] std::string_view to_string(SessionState state) noexcept;

/// Why a session was refused or closed. Kept distinct from DecodeError: a
/// protocol error is the peer's fault, a timeout is not.
enum class SessionError : std::uint8_t {
  None = 0,
  /// A trading message arrived before Ready.
  NotAuthenticated,
  /// Hello arrived when one was not expected (e.g. after Ready).
  UnexpectedHello,
  /// Authenticate with a bad token, or for a session already authenticated.
  AuthenticationFailed,
  /// The message's participant disagrees with the authenticated one.
  ParticipantMismatch,
  /// A message the session state does not allow at all.
  UnexpectedMessage,
  /// Hello did not arrive in time.
  HelloTimeout,
  /// Authenticate did not arrive in time.
  AuthTimeout,
  /// A heartbeat went unanswered for too long.
  HeartbeatTimeout,
  /// Session id already connected.
  DuplicateSession,
};

[[nodiscard]] std::string_view to_string(SessionError error) noexcept;

struct SessionConfig {
  /// How long a connection may stay in `Connected` before it must send Hello.
  std::int64_t hello_timeout = 5'000;
  /// How long `AwaitingAuth` may last.
  std::int64_t auth_timeout = 5'000;
  /// How long an unanswered heartbeat may accumulate before the session is
  /// dropped. Separate from the handshake timeouts because it governs a
  /// long-lived session.
  std::int64_t heartbeat_timeout = 15'000;
  /// Interval at which the server should emit a heartbeat. Zero disables
  /// server-initiated heartbeats, leaving the client to drive them.
  std::int64_t heartbeat_interval = 5'000;
};

/// What the state machine decided about one inbound message.
struct SessionVerdict {
  SessionState state = SessionState::Connected;
  SessionError error = SessionError::None;
  bool accepted = false;
  /// True when the session should be closed after this message.
  bool close = false;
  /// True when the gateway should send a heartbeat now.
  bool send_heartbeat = false;
};

class Session {
 public:
  /// `start_time` must be the same clock the gateway will pass to on_tick, and
  /// must be supplied here: the first handshake deadline has to exist from the
  /// moment the connection is accepted, not from the first message. Setting it on
  /// first use means a peer that connects and then says nothing is never
  /// timed out, because on_tick only fires when a deadline is set.
  Session(std::uint64_t id, SessionConfig config, std::int64_t start_time)
      : id_(id), config_(config), deadline_(start_time + config.hello_timeout) {}

  /// Only so Connection can be aggregate-initialised in tests and diagnostics.
  /// A real session is always constructed with its own id, config and start time.
  Session() = default;

  [[nodiscard]] std::uint64_t id() const noexcept {
    return id_;
  }
  [[nodiscard]] SessionState state() const noexcept {
    return state_;
  }
  [[nodiscard]] bool ready() const noexcept {
    return state_ == SessionState::Ready;
  }
  [[nodiscard]] const std::string& session_key() const noexcept {
    return session_key_;
  }
  [[nodiscard]] ParticipantId participant() const noexcept {
    return participant_;
  }
  [[nodiscard]] SessionError last_error() const noexcept {
    return last_error_;
  }

  /// Feed one decoded message. Pure with respect to time: pass `now` in.
  SessionVerdict on_message(const protocol::Inbound& message, std::int64_t now);

  /// Advance timeouts, and emit a heartbeat when one is due.
  SessionVerdict on_tick(std::int64_t now);

  /// Mark the session closed. Idempotent.
  void close() noexcept {
    state_ = SessionState::Closed;
  }

  /// Deadline by which the current state must progress, or 0 if none.
  [[nodiscard]] std::int64_t deadline() const noexcept {
    return deadline_;
  }

 private:
  SessionVerdict refuse(SessionError error);
  SessionVerdict accept(SessionState next, std::int64_t now);

  std::uint64_t id_ = 0;
  SessionConfig config_{};
  SessionState state_ = SessionState::Connected;
  SessionError last_error_ = SessionError::None;
  std::string session_key_;
  ParticipantId participant_{};
  std::int64_t deadline_ = 0;
  std::int64_t next_heartbeat_ = 0;
  /// Last time the peer proved it was alive.
  std::int64_t last_seen_ = 0;
};

/// A token -> participant mapping. Injected rather than stored, so authentication
/// is a pure function of configuration and can be tested exhaustively.
class CredentialStore {
 public:
  void add(std::string token, ParticipantId participant) {
    tokens_[std::move(token)] = participant;
  }
  [[nodiscard]] std::optional<ParticipantId> lookup(const std::string& token) const {
    const auto it = tokens_.find(token);
    return it == tokens_.end() ? std::nullopt : std::optional<ParticipantId>{it->second};
  }
  /// Session keys already connected. A duplicate key is refused: two connections
  /// sharing one key would each think they own the session.
  bool claim(const std::string& key) {
    return keys_.insert(key).second;
  }
  void release(const std::string& key) {
    keys_.erase(key);
  }

 private:
  std::map<std::string, ParticipantId> tokens_;
  std::set<std::string> keys_;
};

}  // namespace lob