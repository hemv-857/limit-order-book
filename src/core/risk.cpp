#include "core/risk.hpp"

#include <algorithm>
#include <utility>

namespace lob {
namespace {}  // namespace

void RateLimiter::configure(std::uint32_t limit_per_second, std::uint32_t max_participants,
                            std::int64_t window_ns) {
  limit_ = limit_per_second;
  window_ns_ = window_ns;
  std::size_t n = 8;
  while (n < static_cast<std::size_t>(max_participants) * 2U) {
    n <<= 1U;
  }
  buckets_.assign(n, Bucket{});
  mask_ = n - 1U;
  load_ = 0;
}

std::size_t RateLimiter::find_slot(ParticipantId id) const noexcept {
  std::size_t pos = static_cast<std::size_t>(mix_id(id.value)) & mask_;
  for (;;) {
    const Bucket& b = buckets_[pos];
    if (!b.in_use || b.id.value == id.value) {
      return pos;
    }
    pos = (pos + 1U) & mask_;
  }
}

bool RateLimiter::allow(ParticipantId participant, Timestamp now) noexcept {
  if (limit_ == 0) {
    return true;
  }
  std::size_t slot = find_slot(participant);
  Bucket& b = buckets_[slot];
  if (!b.in_use) {
    if (load_ * 2U >= buckets_.size()) {
      // Fail closed: a participant the table cannot track is refused rather than
      // triggering a rehash on the order path.
      return false;
    }
    b.in_use = true;
    b.id = participant;
    b.window_start = now;
    b.count = 1;
    ++load_;
    return true;
  }
  if (now.value - b.window_start.value >= window_ns_) {
    b.window_start = now;
    b.count = 1;
    return true;
  }
  if (b.count >= limit_) {
    return false;
  }
  ++b.count;
  return true;
}

void RateLimiter::reset() noexcept {
  for (Bucket& b : buckets_) {
    b.in_use = false;
    b.count = 0;
  }
  load_ = 0;
}

RiskManager::RiskManager(const std::vector<SymbolConfig>& configs) {
  // One shared limiter for the venue, configured to the highest per-symbol
  // limit: a participant sends to many symbols, so per-symbol counters would
  // let it multiply its effective rate by the number of symbols it trades.
  std::uint32_t highest = 0;
  std::uint32_t participants = 0;
  for (const SymbolConfig& cfg : configs) {
    highest = std::max(highest, cfg.rate_limit_per_second);
    participants = std::max(participants, cfg.max_participants);
  }
  limiter_.configure(highest, std::max(participants, 1U));
}

}  // namespace lob