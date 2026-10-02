#include "core/types.hpp"

#include <cstddef>
#include <type_traits>

namespace lob {

// The engine is sharded, memory-mapped and marshalled onto the wire, so the size
// and alignment of its value types are part of its contract, not an accident.
// A silent change here would corrupt every snapshot and journal record written
// by an older build, so it is checked at compile time rather than documented and
// hoped for.

static_assert(sizeof(OrderId) == 8, "OrderId must be a bare 64-bit word");
static_assert(sizeof(ParticipantId) == 4, "ParticipantId must be a bare 32-bit word");
static_assert(sizeof(SymbolId) == 4, "SymbolId must be a bare 32-bit word");
static_assert(sizeof(Price) == 8, "Price must be a bare 64-bit word");
static_assert(sizeof(Quantity) == 8, "Quantity must be a bare 64-bit word");
static_assert(sizeof(Sequence) == 8, "Sequence must be a bare 64-bit word");
static_assert(sizeof(Timestamp) == 8, "Timestamp must be a bare 64-bit word");
static_assert(sizeof(TradeId) == 8, "TradeId must be a bare 64-bit word");

static_assert(std::is_trivially_copyable_v<OrderId>);
static_assert(std::is_trivially_copyable_v<Price>);
static_assert(std::is_trivially_copyable_v<Quantity>);
static_assert(std::is_nothrow_constructible_v<OrderId, std::uint64_t>);
static_assert(std::is_nothrow_default_constructible_v<OrderId>);

// Two's complement signed representation is assumed by the checked arithmetic in
// types.hpp; verify it rather than trusting it, since C++20 still permits
// padding bits and a non-two's-complement type would make checked_mul wrong.
static_assert(std::numeric_limits<std::int64_t>::is_iec559 ||
                  (static_cast<std::int64_t>(-1) == ~static_cast<std::int64_t>(0)),
              "int64_t must be two's complement for checked arithmetic to hold");

}  // namespace lob