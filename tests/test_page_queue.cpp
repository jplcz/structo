// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <structo/page_queue.hpp>
#include <structo/page_queue_scan.hpp>
#include <structo/phys_page.hpp>

#include <reloco/array.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace structo;

constexpr std::uint32_t NIL = ~0u;

struct idx_node {
  std::uint32_t prev{NIL};
  std::uint32_t next{NIL};
};
reloco::array<idx_node, 128> g_nodes;

// Handles >= 100 are marker descriptors.
struct idx_traits : os_traits_base<idx_traits, std::uint32_t> {
  using os_page_type = std::uint32_t;
  static os_page_type null_page() noexcept { return NIL; }
  static bool is_null(os_page_type p) noexcept { return p == NIL; }
  static constexpr bool is_marker(os_page_type p) noexcept { return p >= 100 && p != NIL; }
  static std::uint64_t to_pfn(os_page_type p) noexcept { return p; }
  static reloco::result<os_page_type> from_pfn(std::uint64_t pfn) noexcept { return static_cast<os_page_type>(pfn); }
  static bool is_same_zone(os_page_type, os_page_type) noexcept { return true; }
};
using idx_page = page_view<page_4k, idx_traits>;

// Plain doubly linked list over g_nodes without native splice (exercises the node-by-node path).
struct idx_list {
  std::uint32_t head{NIL};
  std::uint32_t tail{NIL};

  void clear() noexcept { head = tail = NIL; }
  reloco::optional<std::uint32_t> front() const noexcept {
    if (head == NIL)
      return reloco::nullopt;
    return head;
  }
  reloco::optional<std::uint32_t> next(std::uint32_t p) const noexcept {
    if (g_nodes[p].next == NIL)
      return reloco::nullopt;
    return g_nodes[p].next;
  }
  void push_back(std::uint32_t p) noexcept {
    g_nodes[p].prev = tail;
    g_nodes[p].next = NIL;
    if (tail != NIL)
      g_nodes[tail].next = p;
    else
      head = p;
    tail = p;
  }
  void push_front(std::uint32_t p) noexcept {
    g_nodes[p].next = head;
    g_nodes[p].prev = NIL;
    if (head != NIL)
      g_nodes[head].prev = p;
    else
      tail = p;
    head = p;
  }
  void insert_after(std::uint32_t pos, std::uint32_t p) noexcept {
    g_nodes[p].prev = pos;
    g_nodes[p].next = g_nodes[pos].next;
    if (g_nodes[pos].next != NIL)
      g_nodes[g_nodes[pos].next].prev = p;
    else
      tail = p;
    g_nodes[pos].next = p;
  }
  void insert_before(std::uint32_t pos, std::uint32_t p) noexcept {
    if (g_nodes[pos].prev == NIL) {
      push_front(p);
      return;
    }
    insert_after(g_nodes[pos].prev, p);
  }
  void remove(std::uint32_t p) noexcept {
    const auto pr = g_nodes[p].prev, nx = g_nodes[p].next;
    if (pr != NIL)
      g_nodes[pr].next = nx;
    else
      head = nx;
    if (nx != NIL)
      g_nodes[nx].prev = pr;
    else
      tail = pr;
    g_nodes[p] = {};
  }
  reloco::optional<std::uint32_t> pop_front() noexcept {
    if (head == NIL)
      return reloco::nullopt;
    const auto h = head;
    remove(h);
    return h;
  }
};

// Same list with O(1) splice, to exercise the native path.
struct fast_idx_list : idx_list {
  static inline int splices = 0;
  void splice_back(fast_idx_list &o) noexcept {
    ++splices;
    if (o.head == NIL)
      return;
    if (tail == NIL) {
      head = o.head;
    } else {
      g_nodes[tail].next = o.head;
      g_nodes[o.head].prev = tail;
    }
    tail = o.tail;
    o.clear();
  }
  void splice_front(fast_idx_list &o) noexcept {
    ++splices;
    if (o.head == NIL)
      return;
    if (head == NIL) {
      tail = o.tail;
    } else {
      g_nodes[o.tail].next = head;
      g_nodes[head].prev = o.tail;
    }
    head = o.head;
    o.clear();
  }
};

template <typename Q> std::vector<std::uint32_t> contents(const Q &q) {
  std::vector<std::uint32_t> v;
  for (auto h : q)
    v.push_back(h);
  return v;
}

template <typename List> class PageQueueTyped : public ::testing::Test {
protected:
  void SetUp() override {
    for (auto &n : g_nodes)
      n = {};
  }
  using queue = page_queue<List, idx_page>;
};
using Lists = ::testing::Types<idx_list, fast_idx_list>;
TYPED_TEST_SUITE(PageQueueTyped, Lists, );

TYPED_TEST(PageQueueTyped, PushRemovePopAndCount) {
  typename TestFixture::queue q;
  EXPECT_TRUE(q.empty());
  q.push_back(1);
  q.push_back(2);
  q.push_front(0);
  q.insert_after(1, 3);
  q.insert_before(0, 4);
  EXPECT_EQ(q.size(), 5u);
  EXPECT_EQ(contents(q), (std::vector<std::uint32_t>{4, 0, 1, 3, 2}));
  q.remove(1);
  auto f = q.pop_front();
  ASSERT_TRUE(f.has_value());
  EXPECT_EQ(*f, 4u);
  EXPECT_EQ(q.size(), 3u);
  EXPECT_EQ(contents(q), (std::vector<std::uint32_t>{0, 3, 2}));
  EXPECT_EQ(*q.front(), 0u);
  EXPECT_EQ(*q.next(0), 3u);
  EXPECT_FALSE(q.next(2).has_value());
}

TYPED_TEST(PageQueueTyped, MoveToBackAndFrontKeepSize) {
  typename TestFixture::queue q;
  for (std::uint32_t i = 0; i < 4; ++i)
    q.push_back(i);
  q.move_to_back(0);
  EXPECT_EQ(contents(q), (std::vector<std::uint32_t>{1, 2, 3, 0}));
  q.move_to_front(3);
  EXPECT_EQ(contents(q), (std::vector<std::uint32_t>{3, 1, 2, 0}));
  EXPECT_EQ(q.size(), 4u);
}

TYPED_TEST(PageQueueTyped, SplicePreservesOrderAndCounts) {
  typename TestFixture::queue a, b;
  a.push_back(1);
  a.push_back(2);
  b.push_back(10);
  b.push_back(11);
  a.splice_back(b);
  EXPECT_EQ(contents(a), (std::vector<std::uint32_t>{1, 2, 10, 11}));
  EXPECT_EQ(a.size(), 4u);
  EXPECT_TRUE(b.empty());

  typename TestFixture::queue c;
  c.push_back(20);
  c.push_back(21);
  a.splice_front(c);
  EXPECT_EQ(contents(a), (std::vector<std::uint32_t>{20, 21, 1, 2, 10, 11}));
  EXPECT_EQ(a.size(), 6u);

  typename TestFixture::queue empty_q;
  empty_q.splice_front(a); // into an empty queue
  EXPECT_EQ(contents(empty_q), (std::vector<std::uint32_t>{20, 21, 1, 2, 10, 11}));
  EXPECT_TRUE(a.empty());
  empty_q.splice_back(empty_q); // self splice is a no-op
  EXPECT_EQ(empty_q.size(), 6u);
}

TYPED_TEST(PageQueueTyped, PagesAdaptorComposes) {
  typename TestFixture::queue q;
  for (std::uint32_t i = 1; i <= 5; ++i)
    q.push_back(i);
  std::uint32_t sum = 0;
  auto walk = q.pages();
  for (auto h : walk)
    sum += h;
  EXPECT_EQ(sum, 15u);
}

TEST(PageQueueNative, FastListUsesNativeSplice) {
  for (auto &n : g_nodes)
    n = {};
  page_queue<fast_idx_list, idx_page> a, b;
  a.push_back(1);
  b.push_back(2);
  fast_idx_list::splices = 0;
  a.splice_back(b);
  a.splice_front(b); // b is empty now: nothing to splice
  EXPECT_EQ(fast_idx_list::splices, 1);
}

TEST(PageQueueMarkers, SkippedByIterationAndNotCounted) {
  for (auto &n : g_nodes)
    n = {};
  page_queue<idx_list, idx_page> q;
  for (std::uint32_t i = 1; i <= 4; ++i)
    q.push_back(i);

  auto ops = q.scan_ops();
  ops.insert_after(2, 100); // marker between 2 and 3
  ops.insert_after(4, 101); // marker at the tail
  EXPECT_EQ(q.size(), 4u);
  EXPECT_EQ(contents(q), (std::vector<std::uint32_t>{1, 2, 3, 4}));
  EXPECT_EQ(*q.next(2), 3u);
  EXPECT_FALSE(q.next(4).has_value());
  EXPECT_EQ(*ops.next_after(100), 3u); // a marker can be the position
  EXPECT_FALSE(ops.next_after(101).has_value());

  auto f = q.pop_front();
  EXPECT_EQ(*f, 1u);
  ops.remove(100);
  ops.remove(101);
  EXPECT_EQ(q.size(), 3u);
  EXPECT_EQ(contents(q), (std::vector<std::uint32_t>{2, 3, 4}));
}

TEST(PageQueueMarkers, DrivesPageQueueScanAndSurvivesRequeue) {
  for (auto &n : g_nodes)
    n = {};
  struct lock_t {
    void lock() noexcept {}
    void unlock() noexcept {}
  } lock;
  page_queue<idx_list, idx_page> q;
  for (std::uint32_t i = 1; i <= 5; ++i)
    q.push_back(i);

  auto ops = q.scan_ops();
  page_queue_scan scan(ops, lock, 100u);
  std::vector<std::uint32_t> seen;
  for (auto p : scan) {
    seen.push_back(p);
    if (p == 2)
      scan.locked([&] { q.remove(3); }); // page leaves while the lock is dropped
    if (p == 4)
      scan.locked([&] { q.move_to_front(5); }); // jumps behind the cursor: not visited this pass
  }
  EXPECT_EQ(seen, (std::vector<std::uint32_t>{1, 2, 4}));
  EXPECT_EQ(q.size(), 4u);
}

} // namespace
