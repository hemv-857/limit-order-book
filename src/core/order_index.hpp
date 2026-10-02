#pragma once

#include "core/order.hpp"
#include "core/types.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace lob {

/// Open-addressing, linear-probing map from OrderId to arena slot.
///
/// Two properties matter for a matching engine and drive the whole design:
///
/// 1. **It never rehashes.** The table is sized once from the configured
///    per-symbol order limit. When it is full, insertion fails and the caller
///    rejects the order with RejectCode::BookFull. Rehashing would allocate and
///    would produce a latency spike proportional to the table size at exactly
///    the moment the venue is busiest.
///
/// 2. **Deletion never leaves tombstones.** Cancel-heavy workloads would
///    otherwise accumulate dead slots until the load factor looked full while
///    the table was mostly empty. Backward-shift deletion compacts the probe
///    cluster instead, so every occupied slot is always reachable.
///
/// Occupancy is encoded in the generation word (0 means free) rather than in a
/// separate byte or a sentinel OrderId, because OrderId 0 is a legal value on
/// the wire and must not be special-cased.
class OrderIndexTable {
 public:
  /// Default-constructs a valid, empty table. Not merely a convenience: a
  /// default-constructed table that is used before `reset()` would have a zero
  /// bucket mask and probe out of bounds on an empty vector.
  OrderIndexTable() {
    reset(0);
  }

  ~OrderIndexTable() = default;

  OrderIndexTable(const OrderIndexTable&) = delete;
  OrderIndexTable& operator=(const OrderIndexTable&) = delete;
  OrderIndexTable(OrderIndexTable&&) = default;
  OrderIndexTable& operator=(OrderIndexTable&&) = default;

  /// Create a table able to hold at least `min_capacity` entries without
  /// exceeding the configured load factor.
  void reset(std::uint32_t min_capacity) {
    std::uint32_t buckets = 8;
    while (static_cast<std::uint64_t>(buckets) * kLoadFactorNum <
           static_cast<std::uint64_t>(min_capacity) * kLoadFactorDen) {
      buckets <<= 1U;
    }
    slots_.assign(buckets, Slot{});
    mask_ = static_cast<std::size_t>(buckets - 1U);
    max_size_ = static_cast<std::uint32_t>((static_cast<std::uint64_t>(buckets) * kLoadFactorNum) /
                                           kLoadFactorDen);
    size_ = 0;
  }

  /// Insert a mapping. Returns false when the table is at its load limit; the
  /// caller must treat that as a rejection, never as a retry.
  ///
  /// Precondition: `generation` must be non-zero. Zero is the free-slot marker,
  /// so storing it would make a live order invisible. OrderArena guarantees this
  /// by never handing out a slot with generation 0.
  [[nodiscard]] bool insert(OrderId key, OrderIndex value, std::uint32_t generation) noexcept {
    assert(generation != 0 && "generation 0 would be indistinguishable from a free slot");
    if (size_ >= max_size_) {
      return false;
    }
    std::size_t pos = home(key);
    while (slots_[pos].generation != 0) {
      if (slots_[pos].key == key) {
        // Duplicate live id. Refusing to overwrite is deliberate: silently
        // rebinding an id would orphan the order the client still believes is
        // live, and the client could never cancel it.
        return false;
      }
      pos = (pos + 1U) & mask_;
    }
    slots_[pos] = Slot{key, value, generation};
    ++size_;
    return true;
  }

  /// Look up a key. `generation` guards against a stale handle: an index that
  /// has been recycled will not match, so a late cancel can never remove an
  /// order that happens to occupy the recycled slot.
  [[nodiscard]] OrderIndex find(OrderId key, std::uint32_t generation) const noexcept {
    std::size_t pos = home(key);
    for (;;) {
      const Slot& slot = slots_[pos];
      if (slot.generation == 0) {
        return kNullOrder;
      }
      if (slot.key == key && slot.generation == generation) {
        return slot.value;
      }
      pos = (pos + 1U) & mask_;
    }
  }

  /// Look up a key ignoring generation, for recovery paths that hold a raw
  /// index rather than a checked handle.
  [[nodiscard]] OrderIndex find_any(OrderId key) const noexcept {
    std::size_t pos = home(key);
    for (;;) {
      const Slot& slot = slots_[pos];
      if (slot.generation == 0) {
        return kNullOrder;
      }
      if (slot.key == key) {
        return slot.value;
      }
      pos = (pos + 1U) & mask_;
    }
  }

  [[nodiscard]] bool contains(OrderId key) const noexcept {
    return find_any(key) != kNullOrder;
  }

  /// Erase a key. Uses backward-shift deletion: after removing a slot, any
  /// entry whose probe chain passed through it is moved back, so no tombstone
  /// is needed and lookups stay correct.
  bool erase(OrderId key) noexcept {
    std::size_t pos = home(key);
    for (;;) {
      const Slot& slot = slots_[pos];
      if (slot.generation == 0) {
        return false;
      }
      if (slot.key == key) {
        erase_at(pos);
        --size_;
        return true;
      }
      pos = (pos + 1U) & mask_;
    }
  }

  [[nodiscard]] std::uint32_t size() const noexcept {
    return size_;
  }
  [[nodiscard]] std::uint32_t max_size() const noexcept {
    return max_size_;
  }
  [[nodiscard]] std::size_t bucket_count() const noexcept {
    return slots_.size();
  }

 private:
  struct Slot {
    OrderId key{};
    OrderIndex value{kNullOrder};
    std::uint32_t generation{0};  ///< 0 marks the slot free
  };

  // Keep the load factor at or below 1/2: linear probing degrades badly past
  // that point, and the table is sized once so the memory is spent anyway.
  static constexpr std::uint64_t kLoadFactorNum = 1;
  static constexpr std::uint64_t kLoadFactorDen = 2;

  /// splitmix64 finaliser. Order ids arrive from clients, so they may be
  /// sequential, clustered, or adversarially chosen; a cheap high-quality
  /// finaliser keeps the probe chains short in all of those cases.
  [[nodiscard]] static std::uint64_t mix(std::uint64_t x) noexcept {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31U);
  }

  [[nodiscard]] std::size_t home(OrderId key) const noexcept {
    return static_cast<std::size_t>(mix(key.value)) & mask_;
  }

  void erase_at(std::size_t i) noexcept {
    slots_[i].generation = 0;
    slots_[i].key = OrderId{};
    slots_[i].value = kNullOrder;
    std::size_t j = i;
    for (;;) {
      j = (j + 1U) & mask_;
      if (slots_[j].generation == 0) {
        return;  // end of the probe cluster: nothing else to compact
      }
      const std::size_t k = home(slots_[j].key);
      // Move entry j back to i when its home slot lies cyclically in (i, j],
      // i.e. when its probe chain would have been broken by the hole at i.
      const bool would_break = (j > i && (k <= i || k > j)) || (j < i && k <= i && k > j);
      if (would_break) {
        slots_[i] = slots_[j];
        slots_[j].generation = 0;
        slots_[j].key = OrderId{};
        slots_[j].value = kNullOrder;
        i = j;
      }
    }
  }

  std::vector<Slot> slots_;
  std::size_t mask_{0};
  std::uint32_t size_{0};
  std::uint32_t max_size_{0};
};

}  // namespace lob