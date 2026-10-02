#include "gateway/session.hpp"

namespace lob {

std::string_view to_string(SessionState state) noexcept {
  switch (state) {
    case SessionState::Connected:
      return "connected";
    case SessionState::AwaitingAuth:
      return "awaiting_auth";
    case SessionState::Ready:
      return "ready";
    case SessionState::Closed:
      return "closed";
  }
  return "unknown";
}

std::string_view to_string(SessionError error) noexcept {
  switch (error) {
    case SessionError::None:
      return "none";
    case SessionError::NotAuthenticated:
      return "not_authenticated";
    case SessionError::UnexpectedHello:
      return "unexpected_hello";
    case SessionError::AuthenticationFailed:
      return "authentication_failed";
    case SessionError::ParticipantMismatch:
      return "participant_mismatch";
    case SessionError::UnexpectedMessage:
      return "unexpected_message";
    case SessionError::HelloTimeout:
      return "hello_timeout";
    case SessionError::AuthTimeout:
      return "auth_timeout";
    case SessionError::HeartbeatTimeout:
      return "heartbeat_timeout";
    case SessionError::DuplicateSession:
      return "duplicate_session";
  }
  return "unknown";
}

SessionVerdict Session::refuse(SessionError error) {
  last_error_ = error;
  // Every refusal closes the session. Being lenient here -- say, tolerating a
  // stray order before Ready -- is exactly how an unauthenticated peer reaches
  // the book.
  state_ = SessionState::Closed;
  return SessionVerdict{state_, error, false, true, false};
}

SessionVerdict Session::accept(SessionState next, std::int64_t now) {
  state_ = next;
  last_seen_ = now;
  last_error_ = SessionError::None;
  switch (next) {
    case SessionState::AwaitingAuth:
      deadline_ = now + config_.auth_timeout;
      break;
    case SessionState::Ready:
      deadline_ = 0;
      next_heartbeat_ = now + config_.heartbeat_interval;
      break;
    case SessionState::Connected:
      deadline_ = now + config_.hello_timeout;
      break;
    case SessionState::Closed:
      deadline_ = 0;
      break;
  }
  return SessionVerdict{state_, SessionError::None, true, false, false};
}

SessionVerdict Session::on_message(const protocol::Inbound& message, std::int64_t now) {
  if (state_ == SessionState::Closed) {
    return SessionVerdict{state_, SessionError::None, false, true, false};
  }

  // A closed session accepts nothing. Checked first so no message can reopen it.
  switch (message.type) {
    case protocol::MessageType::Goodbye:
      state_ = SessionState::Closed;
      deadline_ = 0;
      return SessionVerdict{state_, SessionError::None, true, true, false};

    case protocol::MessageType::Heartbeat:
      // Any message proves liveness, not just a heartbeat.
      last_seen_ = now;
      last_error_ = SessionError::None;
      return SessionVerdict{state_, SessionError::None, true, false, false};

    case protocol::MessageType::Hello:
      if (state_ != SessionState::Connected) {
        return refuse(SessionError::UnexpectedHello);
      }
      if (message.session_id.empty()) {
        return refuse(SessionError::UnexpectedHello);
      }
      session_key_ = message.session_id;
      return accept(SessionState::AwaitingAuth, now);

    case protocol::MessageType::Authenticate:
      if (state_ != SessionState::AwaitingAuth) {
        return refuse(SessionError::AuthenticationFailed);
      }
      if (message.session_id != session_key_) {
        return refuse(SessionError::AuthenticationFailed);
      }
      // The participant is resolved by the gateway and bound to the session.
      // It never comes from the request that follows.
      return accept(SessionState::Ready, now);

    case protocol::MessageType::NewOrder:
    case protocol::MessageType::Cancel:
    case protocol::MessageType::Replace:
    case protocol::MessageType::MassCancel:
    case protocol::MessageType::Subscribe: {
      if (state_ != SessionState::Ready) {
        return refuse(SessionError::NotAuthenticated);
      }
      // Invariant 2: a request naming a different participant is refused, not
      // rewritten. Rewriting would hide an impersonation attempt behind a
      // silently accepted order.
      //
      // Only the field belonging to *this* message type is checked. Testing all
      // four with a disjunction looks equivalent and is not: the other three are
      // default-initialised to 0, so a session whose participant is 0 would let
      // any claim through on the strength of an unrelated field.
      ParticipantId claimed = ParticipantId{0};
      switch (message.type) {
        case protocol::MessageType::NewOrder:
          claimed = message.new_order.participant;
          break;
        case protocol::MessageType::Cancel:
          claimed = message.cancel.participant;
          break;
        case protocol::MessageType::Replace:
          claimed = message.replace.participant;
          break;
        case protocol::MessageType::MassCancel:
          claimed = message.mass_cancel.participant;
          break;
        default:
          break;  // Subscribe carries no participant
      }
      if (claimed != participant_) {
        return refuse(SessionError::ParticipantMismatch);
      }
      last_seen_ = now;
      last_error_ = SessionError::None;
      return SessionVerdict{state_, SessionError::None, true, false, false};
    }

    case protocol::MessageType::MarketDataSnapshot:
    case protocol::MessageType::MarketDataIncrement:
      // Server -> client only. A client sending these is confused or hostile.
      return refuse(SessionError::UnexpectedMessage);
  }
  return refuse(SessionError::UnexpectedMessage);
}

SessionVerdict Session::on_tick(std::int64_t now) {
  if (state_ == SessionState::Closed) {
    return SessionVerdict{state_, SessionError::None, false, true, false};
  }

  // Handshake deadlines.
  if (deadline_ != 0 && now >= deadline_) {
    if (state_ == SessionState::Connected) {
      return refuse(SessionError::HelloTimeout);
    }
    return refuse(SessionError::AuthTimeout);
  }

  // A live session is dropped once the peer has been silent past the heartbeat
  // timeout, even if server-initiated heartbeats are disabled. Silence is only
  // evidence of death if nothing is expected in the first place.
  if (state_ == SessionState::Ready) {
    if (config_.heartbeat_interval > 0 && now - last_seen_ >= config_.heartbeat_timeout) {
      return refuse(SessionError::HeartbeatTimeout);
    }
    if (config_.heartbeat_interval > 0 && now >= next_heartbeat_) {
      next_heartbeat_ = now + config_.heartbeat_interval;
      return SessionVerdict{state_, SessionError::None, false, false, true};
    }
  }
  return SessionVerdict{state_, SessionError::None, false, false, false};
}

}  // namespace lob