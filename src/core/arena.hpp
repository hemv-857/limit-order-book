#pragma once

#include "core/order.hpp"

#include <cstdint>
#include <vector>

namespace lob {

/// Fixed-capacity pool of Order nodes.
///
/// The array is sized once, in the constructor, and never reallocated. That is
/// the whole point: it is what makes "zero heap allocation while matching" a
/// structural guarantee rather than a hope, and it is why links are indices
/// instead of pointers (a reallocation would invalidate every pointer into the
/// arena, and would silently change what a dangling link points at).
///
/// When the pool is exhausted the engine rejects the order with
/// RejectCode::BookFull rather than growing. Growing under load is exactly the
/// unbounded latency spike the design is meant to eliminate.
class OrderArena {
 public:
  /// `capacity` is the number of *usable* order slots. The backing array holds
  /// one more element than that, because index 0 is reserved as the null
  /// sentinel and usable indices run 1..capacity.
  explicit OrderArena(std::uint32_t capacity)
      : nodes_(static_cast<std::size_t>(capacity) + 1U), capacity_(capacity) {
    // Seed the free list back-to-front so allocation hands out slots in
    // ascending index order, which keeps live orders packed at the front of the
    // array. Slot 0 is the null sentinel: it is seeded as free so a naive walk
    // terminates, but it is never handed out, because the loop stops at i > 1.
    // Walks one past capacity because usable slots are 1..capacity inclusive;
    // stopping at `i > 1` over `i = capacity` would leave slot `capacity` off
    // the list entirely.
    for (std::uint32_t i = capacity + 1U; i > 1; --i) {
      nodes_[i - 1].free_next = (i == capacity + 1U) ? kNullOrder : static_cast<OrderIndex>(i);
    }
    nodes_[0].free_next = kNullOrder;
    free_head_ = capacity > 1 ? OrderIndex{1} : kNullOrder;
  }

  ~OrderArena() = default;

  OrderArena(const OrderArena&) = delete;
  OrderArena& operator=(const OrderArena&) = delete;
  OrderArena(OrderArena&&) = default;
  OrderArena& operator=(OrderArena&&) = default;

  /// Take a slot off the free list, or return kNullOrder (zero) when full.
  /// Indices handed out are always >= 1.
  ///
  /// The generation is incremented on *both* allocation and release so that a
  /// live order's generation is never 0. That matters because OrderIndexTable
  /// uses generation 0 as its free-slot marker; if a live order could carry 0,
  /// it would be indistinguishable from an empty table slot and become
  /// invisible to lookups and cancels.
  [[nodiscard]] OrderIndex allocate() noexcept {
    if (free_head_ == kNullOrder) {
      return kNullOrder;
    }
    const OrderIndex idx = free_head_;
    free_head_ = nodes_[idx].free_next;
    nodes_[idx].free_next = kNullOrder;
    ++nodes_[idx].generation;
    ++live_;
    return idx;
  }

  /// Return a slot to the free list.
  ///
  /// The generation is bumped here rather than on allocation so that any
  /// handle captured before the release is stale the moment the slot goes back
  /// to the pool, even if it has not yet been handed to somebody else.
  void release(OrderIndex idx) noexcept {
    if (idx == kNullOrder) {
      return;
    }
    ++nodes_[idx].generation;
    nodes_[idx].order_id = OrderId{};
    nodes_[idx].prev = kNullOrder;
    nodes_[idx].next = kNullOrder;
    nodes_[idx].level = kNullLevel;
    nodes_[idx].leaves_qty = Quantity{0};
    nodes_[idx].filled_qty = Quantity{0};
    nodes_[idx].flags = 0;
    nodes_[idx].free_next = free_head_;
    free_head_ = idx;
    --live_;
  }

  [[nodiscard]] Order& operator[](OrderIndex idx) noexcept {
    return nodes_[idx];
  }
  [[nodiscard]] const Order& operator[](OrderIndex idx) const noexcept {
    return nodes_[idx];
  }

  /// Usable slots. The backing vector has one more element than this; that
  /// extra one is the null sentinel at index 0.
  [[nodiscard]] std::uint32_t capacity() const noexcept {
    return capacity_;
  }
  [[nodiscard]] std::uint32_t live_count() const noexcept {
    return live_;
  }
  [[nodiscard]] std::uint32_t free_count() const noexcept {
    return capacity_ - live_;
  }

 private:
  std::vector<Order> nodes_;
  std::uint32_t capacity_;
  std::uint32_t live_{0};
  OrderIndex free_head_{kNullOrder};
};

}  // namespace lob