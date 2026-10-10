// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <structo/page_queue_scan.hpp>

#include <reloco/array.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

// Doubly linked queue over indices; index 0 is the list head sentinel. Markers are flagged nodes.
struct node {
  std::uint32_t prev{0};
  std::uint32_t next{0};
  bool marker{false};
  bool queued{false};
};

struct test_lock {
  int depth{0};
  void lock() noexcept { ++depth; }
  void unlock() noexcept { --depth; }
};

struct test_queue {
  reloco::array<node, 32> n{};
  test_lock *lock{nullptr};
  using handle_type = std::uint32_t;

  test_queue() noexcept { n[0].prev = n[0].next = 0; }

  void link_after(std::uint32_t pos, std::uint32_t x) noexcept {
    n[x].prev = pos;
    n[x].next = n[pos].next;
    n[n[pos].next].prev = x;
    n[pos].next = x;
    n[x].queued = true;
  }
  void unlink(std::uint32_t x) noexcept {
    n[n[x].prev].next = n[x].next;
    n[n[x].next].prev = n[x].prev;
    n[x].queued = false;
  }
  void push_back(std::uint32_t x) noexcept { link_after(n[0].prev, x); }

  reloco::optional<std::uint32_t> skip_markers(std::uint32_t x) noexcept {
    while (x != 0 && n[x].marker)
      x = n[x].next;
    if (x == 0)
      return reloco::nullopt;
    return x;
  }
  reloco::optional<std::uint32_t> first() noexcept {
    EXPECT_GT(lock->depth, 0);
    return skip_markers(n[0].next);
  }
  reloco::optional<std::uint32_t> next_after(std::uint32_t pos) noexcept {
    EXPECT_GT(lock->depth, 0);
    return skip_markers(n[pos].next);
  }
  void insert_after(std::uint32_t pos, std::uint32_t m) noexcept {
    EXPECT_GT(lock->depth, 0);
    link_after(pos, m);
  }
  void remove(std::uint32_t m) noexcept {
    EXPECT_GT(lock->depth, 0);
    unlink(m);
  }

  std::vector<std::uint32_t> pages() noexcept {
    std::vector<std::uint32_t> v;
    for (std::uint32_t i = n[0].next; i != 0; i = n[i].next)
      if (!n[i].marker)
        v.push_back(i);
    return v;
  }
  bool any_marker_linked() noexcept {
    for (std::uint32_t i = n[0].next; i != 0; i = n[i].next)
      if (n[i].marker)
        return true;
    return false;
  }
};

class PageQueueScan : public ::testing::Test {
protected:
  void SetUp() override {
    q.lock = &lock;
    for (std::uint32_t i = 1; i <= 6; ++i)
      q.push_back(i);
    q.n[30].marker = true;
    q.n[31].marker = true;
  }
  test_queue q;
  test_lock lock;
};

TEST_F(PageQueueScan, VisitsAllPagesWithLockDroppedInBody) {
  structo::page_queue_scan scan(q, lock, 30u);
  std::vector<std::uint32_t> seen;
  for (auto p : scan) {
    EXPECT_EQ(lock.depth, 0);
    seen.push_back(p);
  }
  EXPECT_EQ(seen, (std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_FALSE(q.any_marker_linked());
}

TEST_F(PageQueueScan, SurvivesRemovalOfCurrentAndNextPageWhileUnlocked) {
  structo::page_queue_scan scan(q, lock, 30u);
  std::vector<std::uint32_t> seen;
  for (auto p : scan) {
    seen.push_back(p);
    if (p == 2) {
      scan.locked([&] {
        q.unlink(2); // the page just returned leaves the queue
        q.unlink(3); // and so does the one that would come next
      });
    }
  }
  EXPECT_EQ(seen, (std::vector<std::uint32_t>{1, 2, 4, 5, 6}));
}

TEST_F(PageQueueScan, PagesRequeuedAtTailAreSeenAgainButBoundedByMaxVisits) {
  structo::page_queue_scan scan(q, lock, 30u, 8);
  std::size_t count = 0;
  for (auto p : scan) {
    ++count;
    scan.locked([&] {
      q.unlink(p);
      q.push_back(p); // activated: moved to the tail, so the scan would never end without a limit
    });
  }
  EXPECT_EQ(count, 8u);
  EXPECT_FALSE(q.any_marker_linked());
}

TEST_F(PageQueueScan, BreakUnlinksMarkerAndTwoScansSkipEachOthersMarkers) {
  {
    structo::page_queue_scan a(q, lock, 30u);
    auto first = a.next();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(*first, 1u);
    EXPECT_TRUE(q.any_marker_linked());

    structo::page_queue_scan b(q, lock, 31u);
    std::vector<std::uint32_t> seen_b;
    for (auto p : b)
      seen_b.push_back(p); // never returns marker 30
    EXPECT_EQ(seen_b, (std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6}));

    auto second = a.next(); // a resumes after its own marker
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(*second, 2u);
  } // a's destructor unlinks its marker
  EXPECT_FALSE(q.any_marker_linked());
  EXPECT_EQ(q.pages().size(), 6u);
}

TEST_F(PageQueueScan, InterleavedScansEachSeeEveryPageDespiteRemovalsAndEarlyExit) {
  structo::page_queue_scan a(q, lock, 30u);
  structo::page_queue_scan b(q, lock, 31u);
  std::vector<std::uint32_t> seen_a, seen_b;

  seen_a.push_back(*a.next()); // 1
  seen_b.push_back(*b.next()); // 1 (both markers now sit after page 1)
  seen_b.push_back(*b.next()); // 2
  b.locked([&] { q.unlink(2); }); // b's current page leaves while a's marker is behind it
  seen_a.push_back(*a.next()); // 3 (2 is gone)
  seen_b.push_back(*b.next()); // 3
  a.finish();                  // a leaves early; b must be unaffected
  EXPECT_FALSE(a.next().has_value());
  for (auto p : b)
    seen_b.push_back(p);

  EXPECT_EQ(seen_a, (std::vector<std::uint32_t>{1, 3}));
  EXPECT_EQ(seen_b, (std::vector<std::uint32_t>{1, 2, 3, 4, 5, 6}));
  EXPECT_FALSE(q.any_marker_linked());
}

TEST_F(PageQueueScan, EmptyQueueYieldsNothing) {
  for (std::uint32_t i = 1; i <= 6; ++i)
    q.unlink(i);
  structo::page_queue_scan scan(q, lock, 30u);
  EXPECT_FALSE(scan.next().has_value());
  EXPECT_FALSE(q.any_marker_linked());
}

} // namespace
