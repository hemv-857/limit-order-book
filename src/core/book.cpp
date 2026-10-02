#include "core/book.hpp"

#include <cassert>
#include <cstddef>

namespace lob {
namespace {

/// Fold a 64-bit word into a running digest.
///
/// This is a boost-style hash combine rather than FNV-1a: it costs three
/// instructions per word and has good avalanche for the highly structured data
/// we feed it (level indices, sequential order ids). Exact cryptographic
/// strength is irrelevant; the requirement is that it is deterministic, cheap,
/// and sensitive to a single-bit change anywhere in the book.
inline void hash_mix(std::uint64_t& h, std::uint64_t v) noexcept {
  h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6U) + (h >> 2U);
}

/// Mask of bits strictly below `bit` in a word. `bit == 63` would need a shift
/// by 64, which is undefined, so that case is special-cased.
inline std::uint64_t bits_below(unsigned bit) noexcept {
  return bit >= 63U ? ~0ULL : ((1ULL << (bit + 1U)) - 1ULL);
}

/// Mask of bits at or above `bit`.
inline std::uint64_t bits_from(unsigned bit) noexcept {
  return bit == 0U ? ~0ULL : (~((1ULL << bit) - 1ULL));
}

}  // namespace

Book::Book(const SymbolConfig& cfg, OrderArena& arena, OrderIndexTable& index)
    : cfg_(cfg),
      min_price_(cfg.min_price),
      max_price_(cfg.max_price),
      domain_(cfg.price_domain()),
      bitmap_words_((static_cast<std::size_t>(domain_) + 63U) / 64U),
      levels_(domain_),
      occupied_(bitmap_words_, 0),
      arena_(&arena),
      index_(&index) {}

void Book::set_occupied(LevelIndex idx, bool occupied) noexcept {
  const std::size_t word = idx / 64U;
  const std::uint64_t bit = 1ULL << (idx % 64U);
  if (occupied) {
    occupied_[word] |= bit;
  } else {
    occupied_[word] &= ~bit;
  }
}

LevelIndex Book::prev_occupied(std::size_t idx) const noexcept {
  if (idx == 0) {
    return kNullLevel;
  }
  std::size_t word = (idx - 1U) / 64U;
  std::uint64_t w = occupied_[word] & bits_below(static_cast<unsigned>((idx - 1U) % 64U));
  for (;;) {
    if (w != 0) {
      // Highest set bit is the nearest occupied level below idx.
      const unsigned msb = 63U - static_cast<unsigned>(__builtin_clzll(w));
      return static_cast<LevelIndex>((word * 64U) + msb);
    }
    if (word == 0) {
      return kNullLevel;
    }
    w = occupied_[--word];
  }
}

LevelIndex Book::next_occupied(std::size_t idx) const noexcept {
  if (idx >= domain_) {
    return kNullLevel;
  }
  const std::size_t start = idx + 1U;
  if (start >= domain_) {
    return kNullLevel;
  }
  std::size_t word = start / 64U;
  std::uint64_t w = occupied_[word] & bits_from(static_cast<unsigned>(start % 64U));
  for (;;) {
    if (w != 0) {
      const unsigned ctz = static_cast<unsigned>(__builtin_ctzll(w));
      return static_cast<LevelIndex>((word * 64U) + ctz);
    }
    if (++word >= bitmap_words_) {
      return kNullLevel;
    }
    w = occupied_[word];
  }
}

bool Book::add_to_queue(OrderIndex idx) noexcept {
  if (idx == kNullOrder) {
    return false;
  }
  Order& order = (*arena_)[idx];
  if (order.level == kNullLevel || !in_domain(order.price)) {
    return false;
  }
  const LevelIndex li = order.level;
  Level& lv = levels_[li];

  if (lv.empty()) {
    // Newly occupied level: claim it for this order's side and possibly become
    // the new extreme. Both are O(1) comparisons against cached indices.
    lv.side = order.side;
    lv.head = idx;
    lv.tail = idx;
    set_occupied(li, true);
    ++active_levels_;
    if (order.side == Side::Buy) {
      if (best_bid_ == kNullLevel || li > best_bid_) {
        best_bid_ = li;
      }
    } else {
      if (best_ask_ == kNullLevel || li < best_ask_) {
        best_ask_ = li;
      }
    }
  } else {
    // A level holding both sides would be a crossed book, which the engine
    // must never allow. Refusing here keeps that impossible by construction
    // rather than by careful ordering of the caller's checks.
    if (lv.side != order.side) {
      return false;
    }
    (*arena_)[lv.tail].next = idx;
    order.prev = lv.tail;
  }

  order.next = kNullOrder;
  order.level = li;
  lv.tail = idx;
  ++lv.order_count;
  lv.aggregate_qty += order.leaves_qty;
  total_qty_ += order.leaves_qty;
  return true;
}

void Book::remove_from_queue(OrderIndex idx) noexcept {
  if (idx == kNullOrder) {
    return;
  }
  Order& order = (*arena_)[idx];
  const LevelIndex li = order.level;
  if (li == kNullLevel) {
    return;
  }
  Level& lv = levels_[li];

  if (order.prev != kNullOrder) {
    (*arena_)[order.prev].next = order.next;
  } else {
    lv.head = order.next;
  }
  if (order.next != kNullOrder) {
    (*arena_)[order.next].prev = order.prev;
  } else {
    lv.tail = order.prev;
  }

  lv.aggregate_qty -= order.leaves_qty;
  total_qty_ -= order.leaves_qty;
  --lv.order_count;

  order.prev = kNullOrder;
  order.next = kNullOrder;
  order.level = kNullLevel;

  if (lv.empty()) {
    set_occupied(li, false);
    --active_levels_;
    if (best_bid_ == li) {
      best_bid_ = prev_occupied(static_cast<std::size_t>(li));
    }
    if (best_ask_ == li) {
      best_ask_ = next_occupied(static_cast<std::size_t>(li));
    }
  }
}

void Book::reduce_level(LevelIndex idx, Quantity qty) noexcept {
  levels_[idx].aggregate_qty -= qty;
  total_qty_ -= qty;
}

void Book::move_to_back_of_level(OrderIndex idx, LevelIndex new_level) noexcept {
  if (idx == kNullOrder || new_level >= domain_) {
    return;
  }
  remove_from_queue(idx);
  (*arena_)[idx].price = price_of(new_level);
  (*arena_)[idx].level = new_level;
  add_to_queue(idx);
}

TopOfBook Book::top_of_book() const noexcept {
  TopOfBook tob;
  if (best_bid_ != kNullLevel) {
    const Level& lv = levels_[best_bid_];
    tob.has_bid = true;
    tob.best_bid = price_of(best_bid_);
    tob.best_bid_qty = lv.aggregate_qty;
    tob.best_bid_orders = lv.order_count;
  }
  if (best_ask_ != kNullLevel) {
    const Level& lv = levels_[best_ask_];
    tob.has_ask = true;
    tob.best_ask = price_of(best_ask_);
    tob.best_ask_qty = lv.aggregate_qty;
    tob.best_ask_orders = lv.order_count;
  }
  return tob;
}

std::uint64_t Book::state_hash() const noexcept {
  std::uint64_t h = 0xCBF29CE484222325ULL;
  // Configuration is part of the state: two books with different tick sizes are
  // not interchangeable, so a replay hash comparison must notice.
  hash_mix(h, static_cast<std::uint64_t>(min_price_));
  hash_mix(h, static_cast<std::uint64_t>(max_price_));
  hash_mix(h, static_cast<std::uint64_t>(cfg_.tick_size));
  hash_mix(h, static_cast<std::uint64_t>(cfg_.lot_size));

  for (std::size_t li = 0; li < domain_; ++li) {
    const auto idx = static_cast<LevelIndex>(li);
    if ((occupied_[li / 64U] & (1ULL << (li % 64U))) == 0) {
      continue;  // empty levels are implied by the walk order, so skip them
    }
    const Level& lv = levels_[idx];
    hash_mix(h, static_cast<std::uint64_t>(idx));
    hash_mix(h, static_cast<std::uint64_t>(lv.side));
    hash_mix(h, static_cast<std::uint64_t>(lv.aggregate_qty.value));
    hash_mix(h, static_cast<std::uint64_t>(lv.order_count));

    // Walk the queue in priority order: the digest must depend on who is in
    // front, not merely on who exists.
    for (OrderIndex cur = lv.head; cur != kNullOrder; cur = (*arena_)[cur].next) {
      const Order& o = (*arena_)[cur];
      hash_mix(h, o.order_id.value);
      hash_mix(h, static_cast<std::uint64_t>(o.price.value));
      hash_mix(h, static_cast<std::uint64_t>(o.leaves_qty.value));
      hash_mix(h, static_cast<std::uint64_t>(o.filled_qty.value));
      hash_mix(h, static_cast<std::uint64_t>(o.total_qty.value));
      hash_mix(h, static_cast<std::uint64_t>(o.display_qty.value));
      hash_mix(h, static_cast<std::uint64_t>(o.trigger_price.value));
      hash_mix(h, static_cast<std::uint64_t>(o.stop_limit_price.value));
      hash_mix(h, static_cast<std::uint64_t>(o.participant.value));
      hash_mix(h, o.arrival_seq.value);
      hash_mix(h, static_cast<std::uint64_t>(o.type));
      hash_mix(h, static_cast<std::uint64_t>(o.tif));
      hash_mix(h, static_cast<std::uint64_t>(o.flags));
      hash_mix(h, static_cast<std::uint64_t>(o.generation));
    }
  }
  return h;
}

bool Book::check_invariants(std::string_view* error) const noexcept {
  auto fail = [&](const char* msg) noexcept {
    if (error != nullptr) {
      *error = msg;
    }
    return false;
  };

  // 1. The book is never crossed.
  if (best_bid_ != kNullLevel && best_ask_ != kNullLevel) {
    if (price_of(best_bid_).value >= price_of(best_ask_).value) {
      return fail("book is crossed");
    }
  }

  // 2. The cached extremes really are the extremes, and agree with the bitmap.
  LevelIndex bid = kNullLevel;
  LevelIndex ask = kNullLevel;
  std::uint32_t active = 0;
  Quantity total{0};

  for (std::size_t li = 0; li < domain_; ++li) {
    const auto idx = static_cast<LevelIndex>(li);
    const Level& lv = levels_[idx];
    const bool bit = (occupied_[li / 64U] & (1ULL << (li % 64U))) != 0;

    if (lv.empty()) {
      if (bit) {
        return fail("occupancy bit set on an empty level");
      }
      if (lv.order_count != 0 || lv.aggregate_qty.value != 0) {
        return fail("empty level has non-zero count or aggregate");
      }
      continue;
    }
    if (!bit) {
      return fail("non-empty level has no occupancy bit");
    }
    ++active;
    total += lv.aggregate_qty;

    // Only a bid side can hold bids and only an ask side can hold asks; a level
    // holding both would mean the book crossed.
    if (lv.side == Side::Buy) {
      if (bid == kNullLevel || idx > bid) {
        bid = idx;
      }
    } else {
      if (ask == kNullLevel || idx < ask) {
        ask = idx;
      }
    }

    // 3. Level aggregate equals the sum over its orders, and the queue links
    //    are structurally sound.
    //
    //    Note what is deliberately *not* checked here: that the queue is in
    //    arrival order. It frequently is not, and must not be: a cancel/replace
    //    that loses priority and an iceberg replenishment both move an older
    //    order behind newer ones. Price-time priority is therefore a property
    //    of the engine's decisions, not of the book, and it is verified
    //    end-to-end by the differential test against the reference engine.
    Quantity sum{0};
    std::uint32_t count = 0;
    OrderIndex prev = kNullOrder;
    for (OrderIndex cur = lv.head; cur != kNullOrder; cur = (*arena_)[cur].next) {
      const Order& o = (*arena_)[cur];
      if (o.side != lv.side) {
        return fail("order side differs from its level side");
      }
      if (o.level != idx || o.price.value != price_of(idx).value) {
        return fail("order does not point back at its level");
      }
      if (o.prev != prev) {
        return fail("broken back link in queue");
      }
      if (o.leaves_qty.value <= 0) {
        return fail("non-positive leaves quantity");
      }
      if (o.filled_qty.value < 0 || o.filled_qty.value + o.leaves_qty.value != o.total_qty.value) {
        return fail("filled + leaves does not equal total quantity");
      }
      sum += o.leaves_qty;
      ++count;
      prev = cur;
    }
    if (count != lv.order_count) {
      return fail("level order count mismatch");
    }
    if (sum.value != lv.aggregate_qty.value) {
      return fail("level aggregate does not equal sum of orders");
    }
    if (lv.tail != prev) {
      return fail("level tail is not the last order in the queue");
    }
  }

  if (bid != best_bid_ || ask != best_ask_) {
    return fail("cached extreme does not match the bitmap");
  }
  if (active != active_levels_) {
    return fail("active level count mismatch");
  }
  if (total.value != total_qty_.value) {
    return fail("total resting quantity mismatch");
  }

  // 5. The id index agrees with the arena: no orphans in either direction.
  if (index_->size() != arena_->live_count()) {
    return fail("id index size differs from live order count");
  }
  // Starts at 1: arena slot 0 is the reserved null sentinel and is never an
  // order. Walking it would compare find_any(0) == 0, which is true for every
  // empty book and says nothing.
  for (std::uint32_t i = 1; i <= arena_->capacity(); ++i) {
    const auto idx = static_cast<OrderIndex>(i);
    const Order& o = (*arena_)[idx];
    const bool linked = o.level != kNullLevel;
    const bool in_index = index_->find_any(o.order_id) == idx;
    if (linked != in_index) {
      return fail("id index and book disagree about an order");
    }
  }
  return true;
}

}  // namespace lob