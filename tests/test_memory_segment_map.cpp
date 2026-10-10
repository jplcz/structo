// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/buddy_allocator.hpp>
#include <structo/memory_segment_map.hpp>
#include <structo/memory_segment_os_traits.hpp>

#include <reloco/array.hpp>

#include <cstdint>
#include <vector>

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo;
using reloco::error;

struct page {
  std::uint32_t flags{0};
};
struct node_tag {
  std::uint8_t node{0};
  bool operator==(const node_tag &o) const noexcept { return node == o.node; }
};

// 4 sections of 16 pages (SectionShift = 4) keep the fixtures small.
using map_t = fixed_memory_segment_map<page, node_tag, 6, 64, page_4k, default_phys_space, 4>;
using pfn_t = map_t::pfn_type;

class MemorySegmentMap : public ::testing::Test {
protected:
  std::vector<page> pool_[8];

  map_t::segment make(std::size_t slot, std::uint64_t first, std::uint64_t count, std::uint8_t node) {
    pool_[slot].assign(static_cast<std::size_t>(count), page{});
    return map_t::segment{pfn_t{first}, count, node_tag{node}, reloco::span<page>(pool_[slot])};
  }

  map_t add(const map_t &m, const map_t::segment &s) {
    auto c = m.check_insert(s);
    EXPECT_TRUE(c.has_value());
    auto r = m.commit(*c);
    EXPECT_TRUE(r.has_value());
    return *r;
  }
};

TEST_F(MemorySegmentMap, EmptyFindsNothing) {
  map_t m;
  EXPECT_FALSE(m.find(pfn_t{5}).has_value());
  EXPECT_FALSE(m.find(map_t::phys_type{0x5000}).has_value());
  EXPECT_EQ(m.size(), 0u);
}

TEST_F(MemorySegmentMap, InsertAndLookup) {
  map_t m;
  m = add(m, make(0, 0x100, 40, 1)); // pfn 0x100..0x127: sections 0x10..0x12
  auto h = m.find(pfn_t{0x110});
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(h->page_index, 0x10u);
  EXPECT_EQ(m.tag_at(*h).node, 1);
  m.page_at(*h).flags = 7;
  EXPECT_EQ(pool_[0][0x10].flags, 7u);

  // Physical address overload: pfn 0x105 -> 0x105000.
  auto h2 = m.find(map_t::phys_type{0x105000});
  ASSERT_TRUE(h2.has_value());
  EXPECT_EQ(h2->page_index, 5u);

  EXPECT_FALSE(m.find(pfn_t{0xFF}).has_value());
  EXPECT_FALSE(m.find(pfn_t{0x128}).has_value()); // same section as the tail, past the end
  EXPECT_FALSE(m.find(pfn_t{0x5000}).has_value());
}

TEST_F(MemorySegmentMap, SortedAndMultiSegmentWithGap) {
  map_t m;
  m = add(m, make(0, 0x400, 16, 2));
  m = add(m, make(1, 0x100, 32, 1));
  ASSERT_EQ(m.size(), 2u);
  const auto segs = m.view().segments();
  EXPECT_EQ(segs[0].first.value, 0x100u);
  EXPECT_EQ(segs[1].first.value, 0x400u);
  EXPECT_EQ(m.tag_at(*m.find(pfn_t{0x401})).node, 2);
  EXPECT_EQ(m.tag_at(*m.find(pfn_t{0x11F})).node, 1);
  EXPECT_FALSE(m.find(pfn_t{0x200}).has_value()); // hole between the segments
  EXPECT_EQ(m.view().table_entries(), 0x40u - 0x10u + 1);
}

TEST_F(MemorySegmentMap, CheckRejectsOverlapAndSharedSection) {
  map_t m;
  m = add(m, make(0, 0x100, 20, 1)); // 0x100..0x113, sections 0x10..0x11
  EXPECT_EQ(m.check_insert(make(1, 0x110, 16, 1)).error(), error::already_exists);
  EXPECT_EQ(m.check_insert(make(1, 0x0F0, 17, 1)).error(), error::already_exists);
  EXPECT_EQ(m.check_insert(make(1, 0x114, 4, 1)).error(), error::already_exists); // shares section 0x11
  EXPECT_TRUE(m.check_insert(make(1, 0x120, 4, 1)).has_value());
}

TEST_F(MemorySegmentMap, CheckValidatesArguments) {
  map_t m;
  auto bad = make(0, 0x100, 16, 0);
  bad.page_count = 8; // does not match the descriptor array
  EXPECT_EQ(m.check_insert(bad).error(), error::invalid_argument);
  auto zero = make(0, 0x100, 0, 0);
  EXPECT_EQ(m.check_insert(zero).error(), error::invalid_argument);
  auto wrap = make(0, ~std::uint64_t{0} - 4, 16, 0);
  EXPECT_FALSE(m.check_insert(wrap).has_value());
}

TEST_F(MemorySegmentMap, CommitIsCopyOnWrite) {
  map_t a;
  a = add(a, make(0, 0x100, 16, 1));
  map_t b = add(a, make(1, 0x200, 16, 2));
  EXPECT_EQ(a.size(), 1u); // the old snapshot is untouched and still answers lookups
  EXPECT_TRUE(a.find(pfn_t{0x100}).has_value());
  EXPECT_FALSE(a.find(pfn_t{0x200}).has_value());
  EXPECT_EQ(b.size(), 2u);
  EXPECT_EQ(b.generation(), a.generation() + 1);
}

TEST_F(MemorySegmentMap, StaleChangeIsRejected) {
  map_t a;
  auto c1 = a.check_insert(make(0, 0x100, 16, 1));
  auto c2 = a.check_insert(make(1, 0x200, 16, 1));
  ASSERT_TRUE(c1.has_value() && c2.has_value());
  auto b = a.commit(*c1);
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->commit(*c2).error(), error::try_again); // checked against the older generation
}

TEST_F(MemorySegmentMap, RemoveShrinksTable) {
  map_t m;
  m = add(m, make(0, 0x100, 16, 1));
  m = add(m, make(1, 0x400, 16, 2));
  auto c = m.check_remove(pfn_t{0x400});
  ASSERT_TRUE(c.has_value());
  auto r = m.commit(*c);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->size(), 1u);
  EXPECT_FALSE(r->find(pfn_t{0x400}).has_value());
  EXPECT_TRUE(r->find(pfn_t{0x100}).has_value());
  EXPECT_EQ(r->view().table_entries(), 1u);
  EXPECT_EQ(m.check_remove(pfn_t{0x123}).error(), error::not_found);

  auto last = r->check_remove(pfn_t{0x100});
  ASSERT_TRUE(last.has_value());
  auto empty = r->commit(*last);
  ASSERT_TRUE(empty.has_value());
  EXPECT_EQ(empty->size(), 0u);
  EXPECT_FALSE(empty->find(pfn_t{0x100}).has_value());
}

TEST_F(MemorySegmentMap, CapacityLimits) {
  using tiny = fixed_memory_segment_map<page, node_tag, 1, 4, page_4k, default_phys_space, 4>;
  std::vector<page> p1(16), p2(16), p3(16);
  tiny t;
  auto c = t.check_insert({tiny::pfn_type{0x10}, 16, node_tag{}, reloco::span<page>(p1)});
  ASSERT_TRUE(c.has_value());
  auto t2 = t.commit(*c);
  ASSERT_TRUE(t2.has_value());
  // Only one segment slot.
  EXPECT_EQ(t2->check_insert({tiny::pfn_type{0x20}, 16, node_tag{}, reloco::span<page>(p2)}).error(),
            error::capacity_exceeded);
  // Table entries: a distant segment would need more than 4 sections.
  EXPECT_EQ(t.check_insert({tiny::pfn_type{0x900}, 16, node_tag{}, reloco::span<page>(p3)}).has_value(), true);
  EXPECT_EQ(t2->check_insert({tiny::pfn_type{0x900}, 16, node_tag{}, reloco::span<page>(p3)}).error(),
            error::capacity_exceeded);
}

TEST_F(MemorySegmentMap, CopyingOwnerKeepsLookupsWorking) {
  map_t m;
  m = add(m, make(0, 0x100, 16, 1));
  map_t copy = m;
  EXPECT_TRUE(copy.find(pfn_t{0x105}).has_value());
  m = map_t{};
  EXPECT_FALSE(m.find(pfn_t{0x105}).has_value());
  EXPECT_TRUE(copy.find(pfn_t{0x105}).has_value());
}

// ---- Integration with page_view / buddy_allocator -------------------------------------------------

struct bpage {
  std::uint64_t next{~0ull};
  std::uint64_t prev{~0ull};
  std::uint16_t order{0};
  bool is_free{false};
};
using bmap_t = fixed_memory_segment_map<bpage, node_tag, 4, 32, page_4k, default_phys_space, 8>;
bmap_t g_bmap;

struct bmap_provider {
  static bmap_t::map_type map() noexcept { return g_bmap.view(); }
};

// Handles are PFNs; descriptors are reached through the segment map.
struct bos_traits : memory_segment_os_traits<bos_traits, bmap_provider, bpage, std::uint64_t> {
  static std::uint16_t buddy_order(os_page_type p) noexcept { return deref(p).order; }
  static void set_buddy_order(os_page_type p, std::uint16_t o) noexcept { deref(p).order = o; }
  static bool is_buddy_free(os_page_type p) noexcept { return deref(p).is_free; }
  static void set_buddy_free(os_page_type p, bool f) noexcept { deref(p).is_free = f; }
};

// Pointer handles: the descriptor address is the handle.
struct pos_traits : memory_segment_os_traits<pos_traits, bmap_provider, bpage, bpage *> {
  static std::uint16_t buddy_order(os_page_type p) noexcept { return p->order; }
  static void set_buddy_order(os_page_type p, std::uint16_t o) noexcept { p->order = o; }
  static bool is_buddy_free(os_page_type p) noexcept { return p->is_free; }
  static void set_buddy_free(os_page_type p, bool f) noexcept { p->is_free = f; }
};

struct pfn_free_list {
  std::uint64_t head{~0ull};

  static bpage &d(std::uint64_t p) noexcept { return bos_traits::deref(p); }
  void clear() noexcept { head = ~0ull; }
  [[nodiscard]] bool empty() const noexcept { return head == ~0ull; }
  void push_front(std::uint64_t p) noexcept {
    d(p).prev = ~0ull;
    d(p).next = head;
    if (head != ~0ull) {
      d(head).prev = p;
    }
    head = p;
  }
  void remove(std::uint64_t p) noexcept {
    const std::uint64_t pr = d(p).prev;
    const std::uint64_t nx = d(p).next;
    if (pr != ~0ull) {
      d(pr).next = nx;
    } else {
      head = nx;
    }
    if (nx != ~0ull) {
      d(nx).prev = pr;
    }
  }
  std::uint64_t pop_front() noexcept {
    const std::uint64_t p = head;
    if (p != ~0ull) {
      remove(p);
    }
    return p;
  }
  struct iterator {
    std::uint64_t cur;
    bool operator!=(const iterator &o) const noexcept { return cur != o.cur; }
    std::uint64_t operator*() const noexcept { return cur; }
    iterator &operator++() noexcept {
      cur = d(cur).next;
      return *this;
    }
  };
  [[nodiscard]] iterator begin() const noexcept { return {head}; }
  [[nodiscard]] iterator end() const noexcept { return {~0ull}; }
};

class MemorySegmentMapOsTraits : public ::testing::Test {
protected:
  std::vector<bpage> ram0_ = std::vector<bpage>(256);
  std::vector<bpage> ram1_ = std::vector<bpage>(256);

  void SetUp() override {
    g_bmap = bmap_t{};
    for (auto *r : {&ram0_, &ram1_}) {
      r->assign(256, bpage{});
    }
    for (auto [vec, first] : {std::pair{&ram0_, 0x1000ull}, std::pair{&ram1_, 0x2000ull}}) {
      auto c = g_bmap.check_insert({bmap_t::pfn_type{first}, 256, node_tag{}, reloco::span<bpage>(*vec)});
      ASSERT_TRUE(c.has_value());
      auto n = g_bmap.commit(*c);
      ASSERT_TRUE(n.has_value());
      g_bmap = *n;
    }
  }
};

TEST_F(MemorySegmentMapOsTraits, PageViewMathByPfn) {
  using pv = page_view<page_4k, bos_traits>;
  auto base = bos_traits::from_pfn(0x1000);
  ASSERT_TRUE(base.has_value());
  pv p = pv::from_os_page(*base);
  EXPECT_EQ(p.pfn(), 0x1000u);
  EXPECT_EQ(p.phys().value, 0x1000'000u);
  auto q = p.try_add(10);
  ASSERT_TRUE(q.has_value());
  EXPECT_EQ(q->pfn(), 0x100Au);
  EXPECT_EQ(p.try_add(256).error(), reloco::error::out_of_range); // past the end of the segment: no memory
  auto last = bos_traits::from_pfn(0x10FF);
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(pv::from_os_page(*last).try_add(1).error(), reloco::error::out_of_range); // end of the segment
  EXPECT_EQ(bos_traits::from_pfn(0x5000).error(), reloco::error::out_of_range);
  EXPECT_FALSE(bos_traits::is_same_zone(0x1000, 0x2000)); // different segments
  EXPECT_TRUE(bos_traits::is_same_zone(0x1000, 0x10FF));
}

TEST_F(MemorySegmentMapOsTraits, PointerHandlesRoundTrip) {
  using pv = page_view<page_4k, pos_traits>;
  auto h = pos_traits::from_pfn(0x2005);
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(*h, &ram1_[5]);
  pv p = pv::from_os_page(*h);
  EXPECT_EQ(p.pfn(), 0x2005u);
  auto b = p.try_get_buddy(0); // pfn ^ 1 = 0x2004
  ASSERT_TRUE(b.has_value());
  EXPECT_EQ(b->get_os_page(), &ram1_[4]);
  EXPECT_TRUE(pos_traits::is_null(pos_traits::null_page()));
}

TEST_F(MemorySegmentMapOsTraits, BuddyAllocatorOverSegment) {
  using pv = page_view<page_4k, bos_traits>;
  buddy_allocator<pfn_free_list, pv, 8> buddy;
  auto start = bos_traits::from_pfn(0x1000);
  ASSERT_TRUE(start.has_value());
  buddy.free_n(pv::from_os_page(*start), 256);
  auto a = buddy.allocate(3);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->pfn() % 8, 0u);
  EXPECT_GE(a->pfn(), 0x1000u);
  EXPECT_LT(a->pfn(), 0x1100u);
  buddy.free(*a, 3);
  auto whole = buddy.allocate(8); // everything merged back into one order-8 block
  ASSERT_TRUE(whole.has_value());
  EXPECT_EQ(whole->pfn(), 0x1000u);
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
