#pragma once

#include "core/book.hpp"
#include "core/config.hpp"
#include "core/types.hpp"

namespace lob::testing {

/// Owns the three things a Book needs and constructs them in the right order.
///
/// Book takes references to an arena and an id index rather than owning them,
/// because a symbol needs two books over the same orders (resting and stops).
/// Tests that only want a single book get this fixture instead of repeating the
/// wiring.
struct BookFixture {
  explicit BookFixture(const SymbolConfig& cfg)
      : arena(cfg.max_open_orders), index(), book(cfg, arena, index) {
    index.reset(cfg.max_open_orders);
  }

  OrderArena arena;
  OrderIndexTable index;
  Book book;
};

}  // namespace lob::testing