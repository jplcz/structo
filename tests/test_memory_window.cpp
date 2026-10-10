// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/memory_segment_map.hpp>
#include <structo/memory_window.hpp>

#include <cstdint>
#include <vector>

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo;

struct wpage {
  std::uint32_t flags{0};
};

TEST(MemoryWindow, AlignedWindowsCoverSegmentExactlyOnce) {
  std::vector<wpage> pages(100);
  // First PFN 0x1005 with 32-page windows: boundaries at 0x1020, 0x1040, ... so the first window is short.
  page_window_walker<wpage> walker(0x1005, reloco::span<wpage>(pages), 32);
  std::vector<std::pair<std::uint64_t, std::uint64_t>> got;
  for (auto &w : walker) {
    got.emplace_back(w.first_pfn(), w.end_pfn());
  }
  ASSERT_EQ(got.size(), 4u);
  EXPECT_EQ(got[0], std::make_pair(std::uint64_t{0x1005}, std::uint64_t{0x1020}));
  EXPECT_EQ(got[1], std::make_pair(std::uint64_t{0x1020}, std::uint64_t{0x1040}));
  EXPECT_EQ(got[2], std::make_pair(std::uint64_t{0x1040}, std::uint64_t{0x1060}));
  EXPECT_EQ(got[3], std::make_pair(std::uint64_t{0x1060}, std::uint64_t{0x1005 + 100})); // short tail
}

TEST(MemoryWindow, WalkYieldsEveryPageWithPfn) {
  std::vector<wpage> pages(10);
  page_window_walker<wpage> walker(0x40, reloco::span<wpage>(pages), 4);
  std::size_t total = 0;
  for (auto &w : walker) {
    for (auto &wp : w.walk()) {
      wp.page().flags = static_cast<std::uint32_t>(wp.pfn);
      ++total;
    }
  }
  EXPECT_EQ(total, 10u);
  for (std::size_t i = 0; i < pages.size(); ++i) {
    EXPECT_EQ(pages[i].flags, 0x40u + i);
  }
}

TEST(MemoryWindow, BoundsCheckedAccess) {
  std::vector<wpage> pages(16);
  page_window_walker<wpage> walker(0x100, reloco::span<wpage>(pages), 8);
  auto w = walker.next();
  ASSERT_TRUE(w.has_value());
  EXPECT_TRUE(w->contains(0x107));
  EXPECT_FALSE(w->contains(0x108));
  EXPECT_FALSE(w->page_at(0x108).has_value());
  EXPECT_FALSE(w->page_at(0xff).has_value());
  auto p = w->page_at(0x103);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(&p->get(), &pages[3]);
  EXPECT_EQ(walker.next_pfn(), 0x108u);
}

TEST(MemoryWindow, FromSegmentAndZeroWindow) {
  using map_t = fixed_memory_segment_map<wpage, int, 4, 32, page_4k, default_phys_space, 4>;
  std::vector<wpage> pages(20);
  map_t::segment seg{map_t::pfn_type{0x200}, 20, 0, reloco::span<wpage>(pages)};
  page_window_walker<wpage> one(seg, 0); // 0 = single window
  auto w = one.next();
  ASSERT_TRUE(w.has_value());
  EXPECT_EQ(w->size(), 20u);
  EXPECT_FALSE(one.next().has_value());
  EXPECT_FALSE(one.next().has_value()); // stays exhausted

  page_window_walker<wpage> empty(0x10, reloco::span<wpage>(), 8);
  EXPECT_FALSE(empty.next().has_value());
}

TEST(MemoryWindowDone, LogMergesAndPendingSkipsDoneRuns) {
  std::vector<structo::pfn_range> storage(4);
  structo::pfn_range_log log{reloco::span<structo::pfn_range>(storage)};
  EXPECT_TRUE(log.add({110, 120}));
  EXPECT_TRUE(log.add({120, 130})); // touches: merged
  EXPECT_TRUE(log.add({150, 160}));
  ASSERT_EQ(log.size(), 2u);
  EXPECT_TRUE(log.covers(125));
  EXPECT_FALSE(log.covers(140));

  std::vector<wpage> pages(100);
  structo::page_window<wpage> w(100, reloco::span<wpage>{pages.data(), pages.size()});
  std::vector<structo::pfn_range> got;
  auto pend = w.pending(log.runs());
  for (auto r : pend)
    got.push_back(r);
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0], (structo::pfn_range{100, 110}));
  EXPECT_EQ(got[1], (structo::pfn_range{130, 150}));
  EXPECT_EQ(got[2], (structo::pfn_range{160, 200}));

  std::uint64_t visited = 0;
  auto walk = w.walk(got[1]);
  for (auto &wp : walk) {
    EXPECT_GE(wp.pfn, 130u);
    EXPECT_LT(wp.pfn, 150u);
    ++visited;
  }
  EXPECT_EQ(visited, 20u);
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
