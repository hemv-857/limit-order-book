#include "core/events.hpp"

#include <string>

namespace lob {

std::string Event::to_string() const {
  // Deliberately a plain, fixed field order with no locale dependence. The
  // differential test compares these strings, and the golden files store them,
  // so the format is a contract: changing it changes the tests on purpose.
  std::string out;
  out.reserve(128);
  const auto num = [&out](std::int64_t v) { out += std::to_string(v); };
  const auto unum = [&out](std::uint64_t v) { out += std::to_string(v); };

  unum(seq.value);
  out += ' ';
  out += lob::to_string(type);
  out += ' ';
  unum(symbol.value);
  out += ' ';
  unum(order_id.value);
  out += ' ';
  unum(participant.value);
  out += ' ';
  out += lob::to_string(side);

  switch (type) {
    case EventType::Rejected:
      out += ' ';
      out += lob::to_string(reject_code);
      break;
    case EventType::Trade:
      out += ' ';
      unum(trade_id.value);
      out += ' ';
      num(price.value);
      out += ' ';
      num(qty.value);
      out += ' ';
      unum(maker_order_id.value);
      out += ' ';
      num(maker_price.value);
      break;
    case EventType::BookUpdate:
      out += ' ';
      num(price.value);
      out += ' ';
      num(qty.value);
      out += ' ';
      out += lob::to_string(action);
      break;
    case EventType::Accepted:
    case EventType::Replaced:
      out += ' ';
      num(price.value);
      out += ' ';
      num(qty.value);
      out += ' ';
      num(leaves_qty.value);
      out += ' ';
      out += lob::to_string(order_type);
      out += ' ';
      out += lob::to_string(tif);
      out += post_only ? " po" : " -";
      break;
    case EventType::Cancelled:
      out += ' ';
      num(leaves_qty.value);
      out += ' ';
      out += lob::to_string(reason);
      break;
    case EventType::StopTriggered:
      out += ' ';
      num(price.value);
      out += ' ';
      num(qty.value);
      out += ' ';
      out += lob::to_string(order_type);
      break;
    case EventType::Expired:
      break;
  }
  return out;
}

}  // namespace lob
