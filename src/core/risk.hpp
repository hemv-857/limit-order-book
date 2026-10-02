#pragma once

#include "core/config.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lob {

struct NewOrderRequest;

/// Per-participant order rate limiting.
///
/// A fixed one-second window keyed on the *injected* timestamp, not on a wall
/// clock. That is a deliberate choice: a wall-clock limiter makes the engine
/// non-deterministic and therefore untestable and unreplayable. A fixed window
/// also has a known burst property (up to 2x the limit across a boundary) which
/// is why it is called out in docs/MATCHING_RULES.md rather than hidden.
///
/// An entry is created lazily on first use and evicted when the participant
/// stops sending for a while, so a long-lived engine does not accumulate one
/// entry per id it has ever seen.
class RateLimiter {
 public:
  /// `max_participants` sizes the table once. Growing it later would allocate
  /// inside `allow`, which runs on the order path, so a full table fails closed
  /// (the participant is refused) instead of reallocating under load.
  void configure(std::uint32_t limit_per_second, std::uint32_t max_participants = 1024,
                 std::int64_t window_ns = 1'000'000'000);

  /// Consume one unit of the participant's allowance. Returns false if the
  /// participant is over its limit for the current window.
  [[nodiscard]] bool allow(ParticipantId participant, Timestamp now) noexcept;

  void reset() noexcept;

  [[nodiscard]] std::uint32_t limit() const noexcept {
    return limit_;
  }
  [[nodiscard]] std::size_t tracked_participants() const noexcept {
    return load_;
  }

  /// Splitmix64 finaliser; participant ids come from clients and may be
  /// sequential or clustered, so the mixer keeps probe chains short.
  [[nodiscard]] static std::uint64_t mix_id(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
  }

 private:
  struct Bucket {
    ParticipantId id{};
    Timestamp window_start{};
    std::uint32_t count{0};
    bool in_use{false};
  };

  /// Open-addressed, power-of-two table keyed by participant id. Sized once by
  /// configure() and never resized.
  [[nodiscard]] std::size_t find_slot(ParticipantId id) const noexcept;

  std::vector<Bucket> buckets_;
  std::size_t mask_{0};
  std::size_t load_{0};
  std::uint32_t limit_{0};
  std::int64_t window_ns_{1'000'000'000};
};

/// Owner of the per-participant rate limiter.
///
/// The other pre-trade checks live in `validate_new_order` (validate.hpp): they
/// are pure functions of the request and the symbol rules, which makes them
/// trivial to test and to mirror in the reference engine. What needs mutable
/// state -- and therefore cannot be a pure function -- is rate limiting, so it
/// lives here behind this small wrapper.
class RiskManager {
 public:
  RiskManager() = default;
  explicit RiskManager(const std::vector<SymbolConfig>& configs);

  /// Consume one unit of the participant's allowance. Called only after the
  /// other checks pass, so a structurally invalid order does not burn quota.
  [[nodiscard]] bool consume_rate_limit(ParticipantId participant, Timestamp now) noexcept {
    return limiter_.allow(participant, now);
  }

  [[nodiscard]] RateLimiter& rate_limiter() noexcept {
    return limiter_;
  }

  void reset_rate_limits() noexcept {
    limiter_.reset();
  }

 private:
  RateLimiter limiter_;
};

}  // namespace lob