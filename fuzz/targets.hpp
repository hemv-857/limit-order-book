// Fuzz targets, written as plain functions.
//
// The driver is deliberately not part of this file. libFuzzer's runtime does not
// exist on every toolchain this project builds with (see docs/PLAN.md deviation
// D-2), so the *target* is separated from the *driver*: the same function is
// driven by libFuzzer where it is available and by a committed-corpus plus
// mutation driver everywhere else, including under ASan and UBSan in CI.
//
// The contract every target here upholds, and the reason they exist:
//
//   No input may crash, read out of bounds, or leave an inconsistent state.
//
// "Inconsistent" means checked, not assumed. The engine target asserts the book's
// full invariant set after every single operation, so a fuzzer input that reaches
// a reachable-but-wrong state is a failure even when nothing technically
// misbehaves. That is the class of bug a fuzzer is uniquely good at finding and
// unit tests are not: a zero-quantity order resting in the book, a crossed book,
// a priority queue that stopped being a queue.
//
// Targets are deterministic and allocation-bounded. They allocate only in setup,
// never per input, so the fuzzer measures the target rather than the allocator.

#pragma once

#include <cstddef>
#include <cstdint>

namespace lob::fuzz {

enum class Target : std::uint8_t {
  /// Wire decoder: arbitrary bytes in, must never be accepted wrongly or read out
  /// of bounds.
  Codec = 0,
  /// Journal reader: arbitrary bytes must never be misparsed as records, and a
  /// torn tail must always be reported rather than trusted.
  Journal = 1,
  /// Engine input: bytes become requests; the book's invariants must hold after
  /// every one.
  Engine = 2,
};

/// Run one input against one target. Returns true if the input was "interesting",
/// i.e. the target got far enough to be worth keeping in a corpus.
///
/// Never throws and never asserts on the *input*; internal invariants are checked
/// with an abort, because a violated invariant is a bug and not a bad input.
[[nodiscard]] bool run(Target target, const std::uint8_t* data, std::size_t size) noexcept;

/// Human-readable name, for the driver to label crashes.
[[nodiscard]] const char* name(Target target) noexcept;

/// Deterministic scenarios for engine bug classes already found by hand.
///
/// Random input reaches these only by luck -- measured at roughly one run in
/// three thousand for the zero-quantity replace -- so a budget that happens to sit
/// below that threshold reports a clean run while the bug is still there. A
/// fuzzer's job is finding the *unknown*; preserving the *known* is the corpus's
/// job, and expressing it as code keeps it reviewable instead of an opaque blob.
///
/// Each scenario asserts the book's invariants as it goes, so reintroducing any of
/// these bugs fails deterministically.
void run_engine_scenarios() noexcept;

[[nodiscard]] std::size_t engine_scenario_count() noexcept;

}  // namespace lob::fuzz