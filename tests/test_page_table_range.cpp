// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/page_table_range.hpp>
#include <structo/phys_page.hpp>

#include <cstddef>
#include <cstdint>
#include <reloco/array.hpp>

using structo::page_4k;
using structo::arch::make_level_range;
using structo::arch::page_table_entry;
using structo::arch::page_table_level;
using structo::arch::page_table_level_range;
using structo::arch::page_table_levels;

namespace {

class PageTableRangeTest : public ::testing::Test {};

using levels = page_table_levels<page_4k, 48, page_table_level<9, 39, false>, page_table_level<9, 30, true>,
                                  page_table_level<9, 21, true>, page_table_level<9, 12, true>>;

struct my_pte_tag {};
using entry = page_table_entry<my_pte_tag>;

using l0_range = page_table_level_range<levels, 0, entry>;
using l3_range = page_table_level_range<levels, 3, entry>;

constexpr std::uint64_t l0_span = l0_range::entry_span; // 1 << 39
constexpr std::uint64_t l3_span = l3_range::entry_span; // 1 << 12 (4KB)

} // namespace

TEST_F(PageTableRangeTest, StaticShapeMatchesLevelTraits) {
  EXPECT_EQ(l0_range::entry_count, 512u);
  EXPECT_EQ(l0_span, std::uint64_t(1) << 39);
  EXPECT_EQ(l3_range::entry_count, 512u);
  EXPECT_EQ(l3_span, std::uint64_t(1) << 12);
}

TEST_F(PageTableRangeTest, RangeWithinSingleEntryYieldsOneStepWithExactBounds) {
  reloco::array<entry, 512> root{};
  std::uint64_t start = 8 * l0_span + 10;
  std::uint64_t end = start + 100;

  l0_range r(root, 0, start, end);
  EXPECT_FALSE(r.empty());

  int count = 0;
  for (auto step : r) {
    EXPECT_EQ(step.index(), 8u);
    EXPECT_EQ(step.entry_base(), 8 * l0_span);
    EXPECT_EQ(step.range_start(), start);
    EXPECT_EQ(step.range_end(), end);
    ++count;
  }
  EXPECT_EQ(count, 1);
}

TEST_F(PageTableRangeTest, RangeSpanningMultipleEntriesClipsFirstAndLastWindows) {
  reloco::array<entry, 512> root{};
  std::uint64_t s = 10 * l0_span + 5;
  std::uint64_t e = 12 * l0_span + 7;

  l0_range r(root, 0, s, e);
  std::size_t expected_idx = 10;
  int count = 0;
  for (auto step : r) {
    EXPECT_EQ(step.index(), expected_idx);
    if (expected_idx == 10) {
      EXPECT_EQ(step.range_start(), s);
      EXPECT_EQ(step.range_end(), 11 * l0_span);
    } else if (expected_idx == 11) {
      EXPECT_EQ(step.range_start(), 11 * l0_span);
      EXPECT_EQ(step.range_end(), 12 * l0_span);
    } else {
      EXPECT_EQ(expected_idx, 12u);
      EXPECT_EQ(step.range_start(), 12 * l0_span);
      EXPECT_EQ(step.range_end(), e);
    }
    ++expected_idx;
    ++count;
  }
  EXPECT_EQ(count, 3);
}

TEST_F(PageTableRangeTest, EmptyRangeYieldsNoSteps) {
  reloco::array<entry, 512> root{};
  l0_range r(root, 0, 100, 100);
  EXPECT_TRUE(r.empty());
  int count = 0;
  for (auto step : r) {
    (void)step;
    ++count;
  }
  EXPECT_EQ(count, 0);
}

TEST_F(PageTableRangeTest, InvertedRangeYieldsNoSteps) {
  reloco::array<entry, 512> root{};
  l0_range r(root, 0, 200, 100); // start > end
  EXPECT_TRUE(r.empty());
}

TEST_F(PageTableRangeTest, RangeEntirelyOutsideTableWindowYieldsNoSteps) {
  reloco::array<entry, 512> root{};
  // table_base_va is far above the queried [0, 50*l0_span) range.
  l0_range r(root, 100 * l0_span, 0, 50 * l0_span);
  EXPECT_TRUE(r.empty());
}

TEST_F(PageTableRangeTest, RangeTouchingEveryEntryYieldsEntryCountSteps) {
  reloco::array<entry, 512> root{};
  l0_range r(root, 0, 0, static_cast<std::uint64_t>(512) * l0_span);
  std::size_t count = 0;
  for (auto step : r) {
    EXPECT_EQ(step.index(), count);
    ++count;
  }
  EXPECT_EQ(count, 512u);
}

TEST_F(PageTableRangeTest, YieldedEntryReferenceAliasesBackingStorage) {
  reloco::array<entry, 512> root{};
  std::uint64_t start = 8 * l0_span;
  std::uint64_t end = start + 1;

  l0_range r(root, 0, start, end);
  for (auto step : r) {
    step.entry() = entry{0xABCDull};
  }
  EXPECT_EQ(root[8].value, 0xABCDull);
}

TEST_F(PageTableRangeTest, MakeLevelRangeHelperDeducesEntryAndAddrType) {
  reloco::array<entry, 512> leaf{};
  std::uint64_t base = 0x1000;
  auto r = make_level_range<levels, 3>(reloco::span<entry>(leaf), base, std::uint64_t(base + 0x1000),
                                        std::uint64_t(base + 0x2000));
  int count = 0;
  for (auto step : r) {
    (void)step;
    ++count;
  }
  EXPECT_EQ(count, 1);
}

TEST_F(PageTableRangeTest, IterSupportsRustStyleAdaptorChain) {
  reloco::array<entry, 512> root{};
  root[10] = entry{1ull};
  root[11] = entry{2ull};

  l0_range r(root, 0, 10 * l0_span, 12 * l0_span);

  std::size_t mapped_count = 0;
  for (auto index : r.iter().filter([](const l0_range::step &s) { return !s.entry().is_null(); }).map(
           [](const l0_range::step &s) { return s.index(); })) {
    EXPECT_TRUE(index == 10u || index == 11u);
    ++mapped_count;
  }
  EXPECT_EQ(mapped_count, 2u);
}

TEST_F(PageTableRangeTest, LeafLevelSinglePageRangeYieldsOneStep) {
  reloco::array<entry, 512> leaf{};
  std::uint64_t base = 0;
  std::uint64_t va = 37 * l3_span + 0x40;
  l3_range r(leaf, base, va, va + 16);
  int count = 0;
  for (auto step : r) {
    EXPECT_EQ(step.index(), 37u);
    EXPECT_EQ(step.range_start(), va);
    EXPECT_EQ(step.range_end(), va + 16);
    ++count;
  }
  EXPECT_EQ(count, 1);
}
