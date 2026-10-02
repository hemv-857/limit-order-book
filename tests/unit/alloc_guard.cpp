// Global replacement of the allocation functions, so the test can assert that
// matching and book maintenance never reach the heap.
//
// operator new/delete are replaced rather than malloc because every allocation
// the standard library makes routes through them, and the core uses only the
// standard library. (A malloc-level hook would additionally catch direct
// malloc calls, but nothing in src/core makes any.)

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace alloc_guard {

std::atomic<bool> g_enabled{false};
std::atomic<std::size_t> g_allocations{0};

void enable() noexcept {
  g_allocations.store(0, std::memory_order_relaxed);
  g_enabled.store(true, std::memory_order_release);
}

void disable() noexcept {
  g_enabled.store(false, std::memory_order_release);
}

std::size_t count() noexcept {
  return g_allocations.load(std::memory_order_acquire);
}

inline void* counted(std::size_t size) {
  if (g_enabled.load(std::memory_order_acquire)) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  // operator new must throw on failure rather than return null.
  void* p = std::malloc(size != 0 ? size : 1);
  return p;
}

inline void* counted_aligned(std::size_t size, std::size_t alignment) {
  if (g_enabled.load(std::memory_order_acquire)) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
  }
  void* p = nullptr;
  if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment,
                     size != 0 ? size : 1) != 0) {
    return nullptr;
  }
  return p;
}

}  // namespace alloc_guard

void* operator new(std::size_t n) {
  return alloc_guard::counted(n);
}
void* operator new[](std::size_t n) {
  return alloc_guard::counted(n);
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  return alloc_guard::counted(n);
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return alloc_guard::counted(n);
}
void* operator new(std::size_t n, std::align_val_t a) {
  return alloc_guard::counted_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return alloc_guard::counted_aligned(n, static_cast<std::size_t>(a));
}

void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete[](void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
  std::free(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  std::free(p);
}  // NOLINT
void operator delete[](void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}