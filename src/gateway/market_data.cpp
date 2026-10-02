#include "gateway/market_data.hpp"

#include <utility>

namespace lob {

protocol::SnapshotPayload make_snapshot(const Book& book, SymbolId symbol,
                                        std::uint64_t sequence) noexcept {
  const TopOfBook tob = book.top_of_book();
  protocol::SnapshotPayload s;
  s.sequence = sequence;
  s.symbol = symbol;
  s.has_bid = tob.has_bid;
  s.best_bid = tob.best_bid.value;
  s.best_bid_qty = tob.best_bid_qty.value;
  s.best_bid_orders = tob.best_bid_orders;
  s.has_ask = tob.has_ask;
  s.best_ask = tob.best_ask.value;
  s.best_ask_qty = tob.best_ask_qty.value;
  s.best_ask_orders = tob.best_ask_orders;
  return s;
}

bool as_increment(const Event& event, protocol::IncrementPayload& out) noexcept {
  if (event.type != EventType::BookUpdate) {
    return false;
  }
  out = protocol::IncrementPayload{};
  out.sequence = event.seq.value;
  out.symbol = event.symbol;
  out.side = event.side;
  out.action = event.action;
  out.price = event.price.value;
  out.quantity = event.qty.value;
  return true;
}

MarketDataPublisher::Subscriber* MarketDataPublisher::find(SessionId session) noexcept {
  const auto it = subscribers_.find(session.value);
  return it == subscribers_.end() ? nullptr : &it->second;
}

const MarketDataPublisher::Subscriber* MarketDataPublisher::find(SessionId session) const noexcept {
  const auto it = subscribers_.find(session.value);
  return it == subscribers_.end() ? nullptr : &it->second;
}

// NOLINTBEGIN(readability-make-member-function-const) -- appends to
// sub.outbox through its reference argument, so genuinely non-const.
bool MarketDataPublisher::enqueue(Subscriber& sub, protocol::MessageType type,
                                  const protocol::Inbound& message) {
  protocol::Inbound m = message;
  m.type = type;
  if (!protocol::encode(m, sub.outbox)) {
    return true;  // oversized single message; the cap check below will catch it
  }
  if (sub.outbox.size() > config_.max_outbox_bytes) {
    // Slow consumer. Dropping the subscriber is the only honest response: the
    // alternative is a client holding a book it believes is current and is not.
    sub.dropped = true;
    sub.outbox.clear();
  }
  return !sub.dropped;
}
// NOLINTEND(readability-make-member-function-const)

bool MarketDataPublisher::subscribe(SessionId session, SymbolId symbol, const Book& book,
                                    std::uint64_t sequence) {
  Subscriber& sub = subscribers_[session.value];
  if (sub.dropped) {
    return false;
  }
  const auto existing = sub.delivered_through.find(symbol.value);
  if (existing != sub.delivered_through.end()) {
    return false;  // already subscribed
  }
  if (sub.delivered_through.size() >= config_.max_symbols_per_session) {
    return false;
  }
  // Seed the boundary *before* queueing, so an increment that arrives during
  // this call cannot be delivered ahead of the snapshot it belongs after.
  sub.delivered_through.emplace(symbol.value, sequence);

  protocol::Inbound m;
  m.snapshot = make_snapshot(book, symbol, sequence);
  return enqueue(sub, protocol::MessageType::MarketDataSnapshot, m);
}

bool MarketDataPublisher::unsubscribe(SessionId session, SymbolId symbol) {
  Subscriber* sub = find(session);
  if (sub == nullptr) {
    return false;
  }
  return sub->delivered_through.erase(symbol.value) != 0;
}

void MarketDataPublisher::remove(SessionId session) {
  subscribers_.erase(session.value);
}

void MarketDataPublisher::publish(const Event& event) {
  protocol::IncrementPayload increment;
  if (!as_increment(event, increment)) {
    return;
  }
  for (auto& [id, sub] : subscribers_) {
    if (sub.dropped) {
      continue;
    }
    const auto it = sub.delivered_through.find(event.symbol.value);
    if (it == sub.delivered_through.end()) {
      continue;  // not subscribed to this symbol
    }
    // Strictly greater: this is what makes the boundary exact. An increment at
    // or below the snapshot's sequence has already been folded into the
    // snapshot, and re-sending it would apply the same change twice.
    if (increment.sequence <= it->second) {
      continue;
    }
    it->second = increment.sequence;

    protocol::Inbound m;
    m.increment = increment;
    enqueue(sub, protocol::MessageType::MarketDataIncrement, m);
  }
}

const std::vector<std::uint8_t>& MarketDataPublisher::outbox(SessionId session) const {
  static const std::vector<std::uint8_t> kEmpty;
  const Subscriber* sub = find(session);
  return sub == nullptr ? kEmpty : sub->outbox;
}

bool MarketDataPublisher::is_dropped(SessionId session) const {
  const Subscriber* sub = find(session);
  return sub != nullptr && sub->dropped;
}

bool MarketDataPublisher::is_subscribed(SessionId session, SymbolId symbol) const {
  const Subscriber* sub = find(session);
  return sub != nullptr && sub->delivered_through.contains(symbol.value);
}

void MarketDataPublisher::take(SessionId session, std::vector<std::uint8_t>& out) {
  Subscriber* sub = find(session);
  if (sub == nullptr) {
    out.clear();
    return;
  }
  // Move, not swap. The reactor reuses one write buffer across sessions, and a
  // swap would hand the subscriber whatever that buffer happened to contain --
  // silently feeding one client another client's bytes.
  out = std::move(sub->outbox);
  sub->outbox.clear();
}

std::uint64_t MarketDataPublisher::delivered_through(SessionId session, SymbolId symbol) const {
  const Subscriber* sub = find(session);
  if (sub == nullptr) {
    return 0;
  }
  const auto it = sub->delivered_through.find(symbol.value);
  return it == sub->delivered_through.end() ? 0 : it->second;
}

}  // namespace lob