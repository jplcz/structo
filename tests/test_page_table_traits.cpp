// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/page_table_traits.hpp>
#include <structo/phys_page.hpp>

#include <cstddef>
#include <cstdint>

using structo::page_4k;
using structo::arch::page_table_entry;
using structo::arch::page_table_level;
using structo::arch::page_table_levels;

namespace {

class PageTableTraitsTest : public ::testing::Test {};

// ARM64 / x86-64 shaped: 4KB granule, 4 levels, 48-bit VA.
using arm64_4k_4level = page_table_levels<page_4k, 48, page_table_level<9, 39, false>, page_table_level<9, 30, true>,
                                           page_table_level<9, 21, true>, page_table_level<9, 12, true>>;

// RISC-V Sv39 shaped: 4KB granule, 3 levels, 39-bit VA.
using sv39 =
    page_table_levels<page_4k, 39, page_table_level<9, 30, true>, page_table_level<9, 21, true>,
                       page_table_level<9, 12, true>>;

// A single-level, flat (identity-ish) configuration -- the degenerate case.
using single_level = page_table_levels<page_4k, 21, page_table_level<9, 12, true>>;

struct my_pte_tag {};
using my_entry = page_table_entry<my_pte_tag>;

} // namespace

TEST_F(PageTableTraitsTest, StaticShapeMetadataIsCorrect) {
  static_assert(arm64_4k_4level::level_count == 4);
  static_assert(arm64_4k_4level::va_bits == 48);
  static_assert(arm64_4k_4level::entry_count<0>() == 512);
  static_assert(arm64_4k_4level::entry_count<3>() == 512);
  static_assert(!arm64_4k_4level::allows_leaf<0>());
  static_assert(arm64_4k_4level::allows_leaf<1>());
  static_assert(arm64_4k_4level::allows_leaf<2>());
  static_assert(arm64_4k_4level::allows_leaf<3>());
  SUCCEED();
}

TEST_F(PageTableTraitsTest, IndexOfDecomposesEveryLevelWithoutLossOrOverlap) {
  constexpr std::uint64_t va = 0x0000'7f12'3456'7890ull;

  auto l0 = arm64_4k_4level::index_of<0>(va);
  auto l1 = arm64_4k_4level::index_of<1>(va);
  auto l2 = arm64_4k_4level::index_of<2>(va);
  auto l3 = arm64_4k_4level::index_of<3>(va);
  auto offset = arm64_4k_4level::page_offset(va);

  EXPECT_EQ(l0, (va >> 39) & 0x1FFu);
  EXPECT_EQ(l1, (va >> 30) & 0x1FFu);
  EXPECT_EQ(l2, (va >> 21) & 0x1FFu);
  EXPECT_EQ(l3, (va >> 12) & 0x1FFu);
  EXPECT_EQ(offset, va & 0xFFFu);

  // Reassembling the pieces must reproduce the original address exactly:
  // no bit may be dropped (a gap) or counted twice (an overlap).
  std::uint64_t reassembled = (static_cast<std::uint64_t>(l0) << 39) | (static_cast<std::uint64_t>(l1) << 30) |
                              (static_cast<std::uint64_t>(l2) << 21) | (static_cast<std::uint64_t>(l3) << 12) |
                              offset;
  EXPECT_EQ(reassembled, va);
}

TEST_F(PageTableTraitsTest, ZeroAddressAndAllOnesAddressDecomposeToExtremes) {
  EXPECT_EQ(arm64_4k_4level::index_of<0>(std::uint64_t(0)), 0u);
  EXPECT_EQ(arm64_4k_4level::page_offset(std::uint64_t(0)), 0u);

  constexpr std::uint64_t max_48bit = (std::uint64_t(1) << 48) - 1;
  EXPECT_EQ(arm64_4k_4level::index_of<0>(max_48bit), 511u);
  EXPECT_EQ(arm64_4k_4level::index_of<3>(max_48bit), 511u);
  EXPECT_EQ(arm64_4k_4level::page_offset(max_48bit), 0xFFFu);
}

TEST_F(PageTableTraitsTest, ThreeLevelSv39ShapeDecomposesCorrectly) {
  constexpr std::uint64_t va = 0x0000'003f'8020'1000ull;
  auto s0 = sv39::index_of<0>(va);
  auto s1 = sv39::index_of<1>(va);
  auto s2 = sv39::index_of<2>(va);
  EXPECT_EQ(s0, (va >> 30) & 0x1FFu);
  EXPECT_EQ(s1, (va >> 21) & 0x1FFu);
  EXPECT_EQ(s2, (va >> 12) & 0x1FFu);
}

TEST_F(PageTableTraitsTest, SingleLevelConfigurationIsValid) {
  constexpr std::uint64_t va = 0x0000'0000'001f'f123ull;
  EXPECT_EQ(single_level::level_count, 1u);
  EXPECT_EQ(single_level::index_of<0>(va), (va >> 12) & 0x1FFu);
}

TEST_F(PageTableTraitsTest, DefaultConstructedEntryIsNull) {
  my_entry e;
  EXPECT_TRUE(e.is_null());
  EXPECT_FALSE(e);
  EXPECT_EQ(e, my_entry{});
}

TEST_F(PageTableTraitsTest, NullptrConstructedEntryIsNull) {
  my_entry e(nullptr);
  EXPECT_TRUE(e.is_null());
}

TEST_F(PageTableTraitsTest, RawConstructedNonZeroEntryIsNotNull) {
  my_entry e(0x1000'0001ull);
  EXPECT_FALSE(e.is_null());
  EXPECT_TRUE(static_cast<bool>(e));
  EXPECT_EQ(e.value, 0x1000'0001ull);
}

TEST_F(PageTableTraitsTest, EntriesWithEqualRawValueCompareEqual) {
  my_entry a(0x42ull);
  my_entry b(0x42ull);
  my_entry c(0x43ull);
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}
