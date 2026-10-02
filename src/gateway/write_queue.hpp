// Per-connection output queue with correct partial-write accounting.
//
// Non-blocking sockets accept only what they feel like accepting. A 4 KB frame
// on a socket with 1 KB of buffer space is written in pieces, and getting that
// accounting wrong is one of the easiest ways to corrupt a byte stream: send the
// same bytes twice, or skip a chunk that the kernel accepted, and the client's
// frame reassembly desynchronises permanently with no error anywhere.
//
// The rule this class exists to make unmissable: **the caller reports exactly how
// many bytes the kernel took, and `consume` removes exactly that many from the
// front.** Nothing is inferred from the write call's return value being "less
// than asked", and nothing is re-sent speculatively.
//
// Two implementation choices follow from that:
//
//   - The queue never erases from the front on write. Erasing shifts every
//     remaining byte, which is O(n) per write and, in a chunked queue, silently
//     reallocates. Instead a `head_` offset walks forward and the buffer is
//     compacted only when the dead prefix is worth reclaiming.
//   - `readable()` hands back the whole contiguous buffer as one view, so the
//     caller performs exactly one `write()` per chunk. Splitting a logical frame
//     across two writes is fine; splitting it because a helper decided to is not.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace lob {

class WriteQueue {
 public:
  /// Append bytes to send.
  void append(std::string_view bytes) {
    compact_if_worthwhile();
    buffer_.append(bytes);
  }

  /// The contiguous bytes ready to be written, oldest first. Empty when there is
  /// nothing pending. The view is invalidated by `append` or `consume`.
  ///
  /// Built from data() and size() rather than string_view::substr: substr is not
  /// marked noexcept, and this sits on the write path where the analyser is right
  /// to be suspicious. The two are identical for an in-range head_.
  [[nodiscard]] std::string_view readable() const noexcept {
    return std::string_view(buffer_.data() + head_, buffer_.size() - head_);
  }

  /// Record that the kernel accepted `n` bytes. `n` must not exceed
  /// `readable().size()`: reporting more than was written would silently skip
  /// bytes the peer never received.
  void consume(std::size_t n) noexcept {
    // Clamped rather than trusted. A caller that over-reports would otherwise
    // skip unsent bytes and desynchronise the stream with no other symptom.
    const std::size_t room = buffer_.size() - head_;
    head_ += (n > room) ? room : n;
    if (head_ == buffer_.size()) {
      buffer_.clear();
      head_ = 0;
    }
  }

  [[nodiscard]] bool empty() const noexcept {
    return head_ >= buffer_.size();
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return buffer_.size() - head_;
  }

  /// Called when the kernel refuses to write anything without blocking, i.e.
  /// EAGAIN/EWOULDBLOCK. Waits for write-readiness before trying again. This is
  /// the flag that stops a full send buffer turning into a busy loop.
  [[nodiscard]] bool wants_write() const noexcept {
    return !empty();
  }

  void clear() noexcept {
    buffer_.clear();
    head_ = 0;
  }

  /// Compact when the dead prefix dominates. Amortised: after this, at least half
  /// the buffer is live again, so the next compaction is a whole buffer away.
  void compact_if_worthwhile() {
    if (head_ == 0) {
      return;
    }
    if (head_ >= buffer_.size() / 2) {
      buffer_.erase(0, head_);
      head_ = 0;
    }
  }

  /// Force a compaction regardless of ratio. Only for teardown.
  void compact() {
    buffer_.erase(0, head_);
    head_ = 0;
  }

 private:
  std::string buffer_;
  std::size_t head_ = 0;
};

}  // namespace lob