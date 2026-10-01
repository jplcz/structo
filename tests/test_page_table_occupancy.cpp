// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/page_table_occupancy.hpp>
#include <structo/arch/page_table_traits.hpp>

#include <cstdint>
#include <reloco/array.hpp>
#include <reloco/span.hpp>

using structo::arch::page_table_entry;
using structo::arch::page_table_occupancy;

namespace {

class PageTableOccupancyTest : public ::testing::Test {};

struct my_pte_tag {};
using entry = page_table_entry<my_pte_tag>;

} // namespace

TEST_F(PageTableOccupancyTest, DefaultConstructedIsEmpty) {
  page_table_occupancy<std::uint32_t> occ;
  EXPECT_TRUE(occ.empty());
  EXPECT_EQ(occ.count(), 0u);
}

TEST_F(PageTableOccupancyTest, ExplicitInitialCountIsHonored) {
  page_table_occupancy<std::uint16_t> occ(5);
  EXPECT_EQ(occ.count(), 5u);
  EXPECT_FALSE(occ.empty());
}

TEST_F(PageTableOccupancyTest, IncrementIncreasesCountAndClearsEmpty) {
  page_table_occupancy<std::uint32_t> occ;
  occ.increment();
  EXPECT_EQ(occ.count(), 1u);
  EXPECT_FALSE(occ.empty());
  occ.increment();
  EXPECT_EQ(occ.count(), 2u);
}

TEST_F(PageTableOccupancyTest, DecrementAboveZeroReturnsFalseAndDoesNotEmpty) {
  page_table_occupancy<std::uint32_t> occ(2);
  EXPECT_FALSE(occ.decrement());
  EXPECT_EQ(occ.count(), 1u);
  EXPECT_FALSE(occ.empty());
}

TEST_F(PageTableOccupancyTest, DecrementToZeroReturnsTrueAndBecomesEmpty) {
  page_table_occupancy<std::uint32_t> occ(1);
  EXPECT_TRUE(occ.decrement());
  EXPECT_TRUE(occ.empty());
}

TEST_F(PageTableOccupancyTest, MultipleIncrementDecrementRoundTripBackToEmpty) {
  page_table_occupancy<std::uint32_t> occ;
  for (int i = 0; i < 10; ++i) {
    occ.increment();
  }
  EXPECT_EQ(occ.count(), 10u);
  for (int i = 0; i < 9; ++i) {
    EXPECT_FALSE(occ.decrement());
  }
  EXPECT_TRUE(occ.decrement());
  EXPECT_TRUE(occ.empty());
}

TEST_F(PageTableOccupancyTest, IncrementPastMaxTraps) {
  page_table_occupancy<std::uint8_t> occ(255);
  EXPECT_DEATH({ occ.increment(); }, "");
}

TEST_F(PageTableOccupancyTest, DecrementPastZeroTraps) {
  page_table_occupancy<std::uint32_t> occ;
  EXPECT_DEATH({ (void)occ.decrement(); }, "");
}

TEST_F(PageTableOccupancyTest, CountNonNullScansSpanAndIgnoresNullEntries) {
  reloco::array<entry, 8> table{};
  table[2] = entry{1ull};
  table[5] = entry{2ull};
  auto count = page_table_occupancy<std::uint32_t>::count_non_null<entry>(reloco::span<const entry>(table));
  EXPECT_EQ(count, 2u);
}

TEST_F(PageTableOccupancyTest, CountNonNullOfAllNullTableIsZero) {
  reloco::array<entry, 8> table{};
  auto count = page_table_occupancy<std::uint32_t>::count_non_null<entry>(reloco::span<const entry>(table));
  EXPECT_EQ(count, 0u);
}

TEST_F(PageTableOccupancyTest, CountNonNullSeedsAFreshOccupancyCounter) {
  reloco::array<entry, 16> table{};
  table[0] = entry{0xAAull};
  table[4] = entry{0xBBull};
  table[9] = entry{0xCCull};
  auto count = page_table_occupancy<std::uint32_t>::count_non_null<entry>(reloco::span<const entry>(table));
  page_table_occupancy<std::uint32_t> occ(count);
  EXPECT_EQ(occ.count(), 3u);
  EXPECT_FALSE(occ.decrement());
  EXPECT_FALSE(occ.decrement());
  EXPECT_TRUE(occ.decrement());
  EXPECT_TRUE(occ.empty());
}

TEST_F(PageTableOccupancyTest, ConstexprUsageCompiles) {
  constexpr auto make = []() constexpr {
    page_table_occupancy<std::uint32_t> occ;
    occ.increment();
    occ.increment();
    occ.decrement();
    return occ.count();
  };
  static_assert(make() == 1u);
  SUCCEED();
}
