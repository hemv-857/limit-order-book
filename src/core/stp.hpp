#pragma once

#include "core/config.hpp"
#include "core/types.hpp"

#include <cstdint>
#include <string_view>

namespace lob {

/// What self-trade prevention decided to do about a candidate fill.
enum class StpOutcome : std::uint8_t {
  /// Not a self trade, or STP disabled: proceed with the fill.
  Proceed = 0,
  /// Cancel the resting maker and keep looking further down the queue.
  CancelMaker = 1,
  /// Cancel the incoming aggressor and stop matching.
  CancelTaker = 2,
  /// Cancel both and stop matching.
  CancelBoth = 3,
  /// Fill the aggressor in full but reduce the maker by the fill quantity,
  /// cancelling it if it reaches zero.
  DecrementMaker = 4,
};

/// Decide how to handle a would-be fill between `maker` and `taker`.
///
/// A pure function so that the reference engine, the unit tests and the
/// documentation all describe the same rule with no shared state. The order of
/// the comparison is irrelevant (equality), but both sides are compared rather
/// than relying on a single id space so that a future venue can distinguish
/// sub-accounts within one participant.
[[nodiscard]] constexpr StpOutcome stp_decide(StpMode mode, ParticipantId maker,
                                              ParticipantId taker) noexcept {
  if (mode == StpMode::None || maker.value != taker.value) {
    return StpOutcome::Proceed;
  }
  switch (mode) {
    case StpMode::None:
      return StpOutcome::Proceed;
    case StpMode::CancelOldest:
      return StpOutcome::CancelMaker;
    case StpMode::CancelNewest:
      return StpOutcome::CancelTaker;
    case StpMode::CancelBoth:
      return StpOutcome::CancelBoth;
    case StpMode::DecrementAndCancel:
      return StpOutcome::DecrementMaker;
  }
  return StpOutcome::Proceed;
}

[[nodiscard]] std::string_view to_string(StpOutcome outcome) noexcept;

}  // namespace lob