// Single-producer / single-consumer ring buffer.
//
// One producer thread pushes, one consumer thread pops, no locks. That is the
// whole contract: two producers or two consumers make every ordering argument
// below void and the result is silent corruption, not a detectable error.
//
// Memory ordering, and why each edge needs what it needs
// -------------------------------------------------------
// The ring is correct if and only if a slot's write is visible to the consumer
// before the consumer is told the slot exists, and the consumer's read is
// visible to the producer before the producer reuses the slot. That gives:
//
//   producer: load  tail_ (acquire)   see the consumer's release, so a slot the
//                                    consumer has already popped is safe to reuse
//             store slot   (relaxed) the data itself
//             store  head_ (release) publish: everything above is visible
//
//   consumer: load  head_ (acquire)   see the producer's release
//             load  slot   (relaxed)
//             store tail_  (release)  publish: this slot is free to reuse
//
// head_ is written only by the producer and tail_ only by the consumer, so each
// side can read its own index without any ordering at all; the interesting edges
// are the cross-thread ones above, and those are exactly the acquire/release
// pairs. The data slot accesses are relaxed because the release/acquire pair on
// the index already orders them -- nothing is gained by making them stronger.
//
// Monotonic indices rather than masked ones, so "full" and "empty" are
// distinguishable without wasting a slot: the buffer holds Capacity items and
// uses a total of Capacity + 1 indices.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <type_traits>

namespace lob {

template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
  static_assert(Capacity >= 2, "a ring needs somewhere to stand");

 public:
  SpscRing() = default;
  ~SpscRing() = default;
  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;
  SpscRing(SpscRing&&) = delete;
  SpscRing& operator=(SpscRing&&) = delete;

  /// Producer only. False when full.
  [[nodiscard]] bool push(const T& value) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    // Acquire: pairs with the consumer's release below, so we do not overwrite a
    // slot the consumer has not finished reading.
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail >= Capacity) {
      return false;
    }
    slots_[head % Capacity] = value;
    // Release: publishes the slot write to the consumer that acquires head_.
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  /// Consumer only. False when empty.
  [[nodiscard]] bool pop(T& out) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    // Acquire: pairs with the producer's release, so the slot read below is
    // ordered after the write that filled it.
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) {
      return false;
    }
    out = slots_[tail % Capacity];
    // Release: the slot is now free, and the producer may reuse it.
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool empty() const noexcept {
    return size() == 0;
  }

  static constexpr std::size_t capacity() noexcept {
    return Capacity;
  }

 private:
  // Padded so producer-written and consumer-written indices sit on separate
  // cache lines. Without this the two threads invalidate each other's line on
  // every operation, which is worse than the atomics cost.
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
  alignas(64) std::array<T, Capacity> slots_{};
};

}  // namespace lob