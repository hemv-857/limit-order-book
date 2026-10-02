#include "core/stp.hpp"

#include <string_view>

namespace lob {

// stp_decide is a pure constexpr function in the header; only the printable name
// needs an out-of-line definition.
std::string_view to_string(StpOutcome outcome) noexcept {
  switch (outcome) {
    case StpOutcome::Proceed:
      return "proceed";
    case StpOutcome::CancelMaker:
      return "cancel_maker";
    case StpOutcome::CancelTaker:
      return "cancel_taker";
    case StpOutcome::CancelBoth:
      return "cancel_both";
    case StpOutcome::DecrementMaker:
      return "decrement_maker";
  }
  return "unknown";
}

}  // namespace lob
