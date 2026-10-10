// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/memory_hotplug.hpp>
#include <structo/memory_segment_lookup.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo;

struct lpage {
  std::atomic<std::int32_t> refs{0};
};
struct lnode {
  std::uint8_t node{0};
};
using lmap = fixed_memory_segment_map<lpage, lnode, 4, 32, page_4k, default_phys_space, 4>;
using lpfn = lmap::pfn_type;

struct single_thread_traits {
  static constexpr std::size_t shards = 1;
  static std::size_t current_shard() noexcept { return 0; }
  using mutex_type = std::mutex;
  static void wait(std::atomic<std::uint32_t> &, std::uint32_t) noexcept {}
  static void wake_all(std::atomic<std::uint32_t> &) noexcept {}
};

using lhotplug = memory_hotplug<lmap, single_thread_traits>;

lhotplug g_hp;
struct hp_provider {
  static lhotplug &hotplug() noexcept { return g_hp; }
};
using hp_lookup = memory_segment_lookup<hotplug_segment_source<hp_provider>>;

class LookupHotplug : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(g_hp.add({lpfn{0x100}, 32, lnode{1}, reloco::span<lpage>(a_)}).has_value());
    ASSERT_TRUE(g_hp.add({lpfn{0x200}, 16, lnode{2}, reloco::span<lpage>(b_)}).has_value());
  }
  void TearDown() override {
    (void)g_hp.remove(lpfn{0x100}, [](const lmap::segment &) {});
    (void)g_hp.remove(lpfn{0x200}, [](const lmap::segment &) {});
  }
  std::vector<lpage> a_{32};
  std::vector<lpage> b_{16};
};

TEST_F(LookupHotplug, PfnAndPhysToPage) {
  EXPECT_EQ(hp_lookup::pfn_to_page(0x105), &a_[5]);
  EXPECT_EQ(hp_lookup::phys_to_page(0x105'123), &a_[5]); // any byte of the page
  EXPECT_EQ(hp_lookup::pfn_to_page(0x20f), &b_[15]);
  EXPECT_EQ(hp_lookup::pfn_to_page(0x150), nullptr);
  EXPECT_TRUE(hp_lookup::pfn_valid(0x100));
  EXPECT_FALSE(hp_lookup::pfn_valid(0x120));
}

TEST_F(LookupHotplug, ReverseLookup) {
  EXPECT_EQ(hp_lookup::page_to_pfn(a_[7]), 0x107u);
  EXPECT_EQ(hp_lookup::page_to_phys(b_[1]), std::uint64_t{0x201} << 12);
  lpage stray;
  EXPECT_FALSE(hp_lookup::page_to_pfn(stray).has_value());
}

TEST_F(LookupHotplug, TagAndSegment) {
  EXPECT_EQ(hp_lookup::tag_of(0x205)->node, 2u);
  EXPECT_FALSE(hp_lookup::tag_of(0x300).has_value());
  auto s = hp_lookup::segment_of(0x110);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->page_count, 32u);
}

TEST_F(LookupHotplug, WithPageAndTryGet) {
  std::uint8_t node = 0;
  EXPECT_TRUE(hp_lookup::with_page(0x204, [&](lpage &, const lnode &t) { node = t.node; }));
  EXPECT_EQ(node, 2u);
  EXPECT_FALSE(hp_lookup::with_page(0x400, [](lpage &, const lnode &) {}));

  // Free page (refs == 0) refuses the reference; a used one accepts it.
  auto try_get = [](lpage &p) {
    auto r = p.refs.load();
    return r > 0 && p.refs.compare_exchange_strong(r, r + 1);
  };
  EXPECT_EQ(hp_lookup::try_get_page(0x101, try_get), nullptr);
  a_[1].refs = 1;
  EXPECT_EQ(hp_lookup::try_get_page(0x101, try_get), &a_[1]);
  EXPECT_EQ(a_[1].refs.load(), 2);
}

TEST_F(LookupHotplug, ForEachSkipsHoles) {
  std::vector<std::uint64_t> seen;
  // 0x11e..0x121 covers the end of segment a and then a hole.
  hp_lookup::for_each_page(0x11e, 4, [&](std::uint64_t pfn, lpage &) { seen.push_back(pfn); });
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0], 0x11eu);
  EXPECT_EQ(seen[1], 0x11fu);
  seen.clear();
  hp_lookup::for_each_page(0x11e, 0x120, [&](std::uint64_t pfn, lpage &) { seen.push_back(pfn); });
  EXPECT_EQ(seen.size(), 2u + 16u); // 0x11e, 0x11f, then all 16 pages of b
}

std::vector<lpage> g_static_pages(16);
struct static_provider {
  static lmap map() noexcept {
    lmap m;
    auto c = m.check_insert({lpfn{0x40}, 16, lnode{3}, reloco::span<lpage>(g_static_pages)});
    auto r = m.commit(*c);
    return *r;
  }
};
using static_lookup = memory_segment_lookup<static_segment_source<static_provider>>;

TEST(LookupStatic, WorksWithoutHotplug) {
  EXPECT_EQ(static_lookup::pfn_to_page(0x44), &g_static_pages[4]);
  EXPECT_EQ(static_lookup::pfn_to_page(0x50), nullptr);
  EXPECT_EQ(static_lookup::page_to_pfn(g_static_pages[2]), 0x42u);
}

struct pfn_hook {
  static std::uint64_t pfn(const lpage &p) noexcept { return 0x1000 + static_cast<std::uint64_t>(p.refs.load()); }
};
using hooked_lookup = memory_segment_lookup<hotplug_segment_source<hp_provider>, pfn_hook>;

TEST_F(LookupHotplug, PfnHookSkipsSegmentScan) {
  a_[3].refs = 7;
  EXPECT_EQ(hooked_lookup::page_to_pfn(a_[3]), 0x1007u); // taken from the hook, not from the map
  lpage stray; // not in any segment: the hook still answers, as the caller guarantees a real page
  EXPECT_EQ(hooked_lookup::page_to_phys(stray), std::uint64_t{0x1000} << 12);
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
