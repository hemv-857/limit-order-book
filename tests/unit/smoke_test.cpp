#include "core/types.hpp"

#include <gtest/gtest.h>

#include <limits>

namespace lob {
namespace {

TEST(Types, StrongTypesRejectImplicitNarrowing) {
  // A Price must not be constructible from an int without going through the
  // explicit constructor; this is the whole point of the wrapper.
  static_assert(!std::is_convertible_v<int, Price>);
  static_assert(!std::is_convertible_v<Quantity, Price>);
  static_assert(std::is_nothrow_constructible_v<Price, std::int64_t>);
}

TEST(Types, PriceComparisonIsTotal) {
  EXPECT_LT(Price{1}, Price{2});
  EXPECT_GT(Price{0}, Price{-1});
  EXPECT_EQ(Price{7}, (Price{7}));
}

TEST(Types, QuantityArithmetic) {
  Quantity q{10};
  q += Quantity{5};
  EXPECT_EQ(q.value, 15);
  q -= Quantity{20};
  EXPECT_EQ(q.value, -5);
}

TEST(Types, CheckedMulDetectsOverflow) {
  const auto ok = checked_mul(std::numeric_limits<std::int64_t>::max() - 1, 1);
  ASSERT_TRUE(ok.ok);
  EXPECT_EQ(ok.value, std::numeric_limits<std::int64_t>::max() - 1);

  const auto bad = checked_mul(std::numeric_limits<std::int64_t>::max(), 2);
  EXPECT_FALSE(bad.ok);
}

TEST(Types, CheckedMulHandlesNegativeNotional) {
  // A negative price x negative quantity is a legitimate positive notional.
  const auto ok = checked_mul(-3, -4);
  ASSERT_TRUE(ok.ok);
  EXPECT_EQ(ok.value, 12);
}

TEST(Types, CheckedAddDetectsOverflow) {
  EXPECT_TRUE(checked_add(1, 2).ok);
  EXPECT_FALSE(checked_add(std::numeric_limits<std::int64_t>::max(), 1).ok);
}

TEST(Types, NoSequenceSentinelIsMax) {
  EXPECT_EQ(no_sequence().value, std::numeric_limits<std::uint64_t>::max());
}

TEST(Types, FitsTickIndexRejectsNegative) {
  EXPECT_TRUE(Price{0}.fits_tick_index());
  EXPECT_FALSE(Price{-1}.fits_tick_index());
  EXPECT_TRUE(Price{static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())}
                  .fits_tick_index());
}

}  // namespace
}  // namespace lob