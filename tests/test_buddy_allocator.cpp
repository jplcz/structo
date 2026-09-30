#if !defined(_MSC_VER)
// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/buddy_allocator.hpp>
#include <structo/phys_page.hpp>

namespace {

using namespace structo;

// ============================================================================
// Mock Environment Setup
// ============================================================================

struct mock_page_meta {
  uint32_t next{~0u};
  uint32_t prev{~0u};
  uint16_t buddy_order{0};
  bool is_free{false};
  uint16_t zone_id{0};
};

// 1024 pages of mock physical RAM
static constexpr size_t MOCK_MEM_PAGES = 1024;
static reloco::array<mock_page_meta, MOCK_MEM_PAGES> mock_ram;

// The Intrusive List implementation working on uint32_t indices
struct test_free_list {
  uint32_t head{~0u};

  void clear() noexcept { head = ~0u; }
  bool empty() const noexcept { return head == ~0u; }

  void push_front(uint32_t p) noexcept {
    mock_ram[p].prev = ~0u;
    mock_ram[p].next = head;
    if (head != ~0u)
      mock_ram[head].prev = p;
    head = p;
  }

  void remove(uint32_t p) noexcept {
    uint32_t prev = mock_ram[p].prev;
    uint32_t next = mock_ram[p].next;
    if (prev != ~0u)
      mock_ram[prev].next = next;
    else
      head = next;
    if (next != ~0u)
      mock_ram[next].prev = prev;
  }

  uint32_t pop_front() noexcept {
    uint32_t p = head;
    if (p != ~0u)
      remove(p);
    return p;
  }

  // ========================================================================
  // Forward Iterator for allocate_constrained
  // ========================================================================
  struct iterator {
    uint32_t current;

    bool operator!=(const iterator &other) const noexcept { return current != other.current; }

    uint32_t operator*() const noexcept { return current; }

    iterator &operator++() noexcept {
      current = mock_ram[current].next;
      return *this;
    }
  };

  iterator begin() const noexcept { return {head}; }
  iterator end() const noexcept { return {~0u}; }
};

// OS Traits bridging page_view to mock_ram
struct test_os_traits : os_traits_base<test_os_traits, uint32_t> {
  using os_page_type = uint32_t;

  static os_page_type null_page() noexcept { return ~0u; }
  static bool is_null(os_page_type p) noexcept { return p == ~0u; }
  static uint64_t to_pfn(os_page_type p) noexcept { return p; }

  static result<os_page_type> from_pfn(uint64_t pfn) noexcept {
    if (pfn >= MOCK_MEM_PAGES)
      return unexpected(error::out_of_range);
    return static_cast<uint32_t>(pfn);
  }

  static bool is_same_zone(os_page_type a, os_page_type b) noexcept {
    return mock_ram[a].zone_id == mock_ram[b].zone_id;
  }

  static uint16_t buddy_order(os_page_type p) noexcept { return mock_ram[p].buddy_order; }
  static void set_buddy_order(os_page_type p, uint16_t order) noexcept { mock_ram[p].buddy_order = order; }

  static bool is_buddy_free(os_page_type p) noexcept { return mock_ram[p].is_free; }
  static void set_buddy_free(os_page_type p, bool free) noexcept { mock_ram[p].is_free = free; }
};

using test_page = page_view<page_4k, test_os_traits>;
// Buddy allocator with max order 10 (1024 pages max)
using test_allocator = buddy_allocator<test_free_list, test_page, 10>;

// ============================================================================
// Test Fixture
// ============================================================================

class BuddyAllocatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Zero out memory and put it all in Zone 0
    for (size_t i = 0; i < MOCK_MEM_PAGES; i++) {
      mock_ram[i] = {~0u, ~0u, 0, false, 0};
    }
  }
};

// ============================================================================
// Tests
// ============================================================================

TEST_F(BuddyAllocatorTest, InitCarvingPerfectPowerOfTwo) {
  test_allocator allocator;

  // Initialize with exactly 1024 pages (Order 10)
  auto init_res = allocator.init(test_page::from_os_page(0), 1024);
  ASSERT_TRUE(init_res.has_value());

  EXPECT_TRUE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 10);

  // Allocate that entire block
  auto alloc_res = allocator.allocate(10);
  ASSERT_TRUE(alloc_res.has_value());
  EXPECT_EQ(alloc_res.value().pfn(), 0u);
  EXPECT_FALSE(mock_ram[0].is_free);
}

TEST_F(BuddyAllocatorTest, InitCarvingUnalignedSize) {
  test_allocator allocator;

  // Initialize with 19 pages. Should carve:
  // Order 4 (16 pages) at PFN 0
  // Order 1 (2 pages)  at PFN 16
  // Order 0 (1 page)   at PFN 18
  auto init_res = allocator.init(test_page::from_os_page(0), 19);
  ASSERT_TRUE(init_res.has_value());

  EXPECT_TRUE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 4);

  EXPECT_TRUE(mock_ram[16].is_free);
  EXPECT_EQ(mock_ram[16].buddy_order, 1);

  EXPECT_TRUE(mock_ram[18].is_free);
  EXPECT_EQ(mock_ram[18].buddy_order, 0);
}

TEST_F(BuddyAllocatorTest, PowerOfTwoSplitAndCoalesce) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 16)); // 16 pages = Order 4

  // Allocate Order 2 (4 pages).
  // Splits Order 4 -> Order 3 (PFN 8) & Order 3 (PFN 0).
  // Splits Order 3 (PFN 0) -> Order 2 (PFN 4) & Order 2 (PFN 0).
  auto alloc_res = allocator.allocate(2);
  ASSERT_TRUE(alloc_res.has_value());

  test_page p = alloc_res.value();
  EXPECT_EQ(p.pfn(), 0u);

  // Validate remaining free buddies
  EXPECT_FALSE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 2);

  EXPECT_TRUE(mock_ram[4].is_free);
  EXPECT_EQ(mock_ram[4].buddy_order, 2);

  EXPECT_TRUE(mock_ram[8].is_free);
  EXPECT_EQ(mock_ram[8].buddy_order, 3);

  // Free the block. It should instantly coalesce back to Order 4!
  allocator.free(p, 2);

  EXPECT_TRUE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 4);
}

TEST_F(BuddyAllocatorTest, ExactPageAllocation_BinaryDecomposition) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 16)); // Order 4 pool

  // Ask for exactly 5 pages. 5 in binary is 101.
  // Closest bounding order is 3 (8 pages).
  // 8 pages gets popped and split exactly.
  auto alloc_res = allocator.allocate_n(5);
  ASSERT_TRUE(alloc_res.has_value());
  EXPECT_EQ(alloc_res.value().pfn(), 0u);

  // Because of binary decomposition:
  // It consumes PFN 0..3 (Order 2) and PFN 4 (Order 0). Total 5 pages.
  // It MUST have returned the unused tail (PFN 5, 6, 7) back to the free list.

  EXPECT_FALSE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 2); // Consumed 4 pages

  EXPECT_FALSE(mock_ram[4].is_free);
  EXPECT_EQ(mock_ram[4].buddy_order, 0); // Consumed 1 page

  // Checking the perfectly returned tail pieces!
  EXPECT_TRUE(mock_ram[5].is_free);
  EXPECT_EQ(mock_ram[5].buddy_order, 0); // Freed 1 page

  EXPECT_TRUE(mock_ram[6].is_free);
  EXPECT_EQ(mock_ram[6].buddy_order, 1); // Freed 2 pages

  // The original unused half from the bounding order split (PFN 8)
  EXPECT_TRUE(mock_ram[8].is_free);
  EXPECT_EQ(mock_ram[8].buddy_order, 3); // Freed 8 pages
}

TEST_F(BuddyAllocatorTest, ExactPageFree_BinaryDecomposition) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 16));

  auto p = allocator.allocate_n(5).value();

  // Ensure free_n traverses the exact same MSB-to-LSB logic
  // and coalesces everything flawlessly back into Order 4 (16 pages).
  allocator.free_n(p, 5);

  EXPECT_TRUE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 4);
}

TEST_F(BuddyAllocatorTest, OutOfMemoryConditions) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 4)); // Only 4 pages (Order 2)

  // Try to allocate more than total capacity
  auto oom_res1 = allocator.allocate(3); // Order 3 (8 pages)
  EXPECT_FALSE(oom_res1.has_value());
  EXPECT_EQ(oom_res1.error(), error::allocation_failed);

  // Try to allocate an order higher than MaxOrder (10)
  auto oom_res2 = allocator.allocate(11);
  EXPECT_FALSE(oom_res2.has_value());
  EXPECT_EQ(oom_res2.error(), error::invalid_argument);

  // Exhaustion
  EXPECT_TRUE(allocator.allocate(2).has_value()); // Take all 4 pages
  auto oom_res3 = allocator.allocate(0);          // Take 1 more page
  EXPECT_FALSE(oom_res3.has_value());
  EXPECT_EQ(oom_res3.error(), error::allocation_failed);
}

TEST_F(BuddyAllocatorTest, ZoneBoundaryProtection) {
  test_allocator allocator;

  // Set up a zone boundary exactly in the middle of our 16 pages
  for (size_t i = 8; i < 16; i++) {
    mock_ram[i].zone_id = 1;
  }

  // Passing 16 pages to Init.
  // Init will try to group all 16 pages into Order 4.
  // The try_add(16) check inside OS traits MUST intercept this because
  // the block crosses from Zone 0 to Zone 1!
  auto init_res = allocator.init(test_page::from_os_page(0), 16);
  EXPECT_FALSE(init_res.has_value());
  EXPECT_EQ(init_res.error(), error::security_violation);
}

// ============================================================================
// Constrained Allocation (DMA / Hardware strict requirements)
// ============================================================================

TEST_F(BuddyAllocatorTest, Constrained_AlignmentAndLowPfn) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 1024));

  // Constraint: Must be at least PFN 5, but aligned to an 8-page boundary.
  // Expectation: The math should push the start_pfn from 5 to 8.
  test_allocator::physical_constraint c;
  c.low_pfn = 5;
  c.high_pfn = 1023;
  c.alignment_pages = 8;
  c.boundary_pages = 0;

  // Allocate 3 pages.
  // It should take the 1024-page Order 10 block.
  // Front padding = 8 pages (PFN 0 to 7).
  // Allocation = 3 pages (PFN 8, 9, 10).
  // Back padding = 1013 pages (PFN 11 to 1023).
  auto alloc_res = allocator.allocate_constrained(3, c);
  ASSERT_TRUE(alloc_res.has_value());

  test_page p = alloc_res.value();
  EXPECT_EQ(p.pfn(), 8u);

  // Verify front padding was correctly shredded back into free lists!
  // Front padding is 8 pages. It should form an Order 3 block at PFN 0.
  EXPECT_TRUE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 3);

  // Verify the allocation itself is marked as consumed
  EXPECT_FALSE(mock_ram[8].is_free);

  // Freeing the 3 pages should heal the entire 1024-page block
  allocator.free_n(p, 3);
  EXPECT_TRUE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 10);
}

TEST_F(BuddyAllocatorTest, Constrained_BoundaryCrossingPrevention) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 1024));

  // Constraint: Start at least at PFN 14. Do NOT cross a 16-page boundary.
  test_allocator::physical_constraint c;
  c.low_pfn = 14;
  c.high_pfn = 1023;
  c.alignment_pages = 1;
  c.boundary_pages = 16;

  // Request 4 pages.
  // If it starts at PFN 14, the block is [14, 15, 16, 17].
  // This crosses the 16-page boundary (PFN 15 to PFN 16).
  // The allocator should detect this and push the start to PFN 16!
  auto alloc_res = allocator.allocate_constrained(4, c);
  ASSERT_TRUE(alloc_res.has_value());

  test_page p = alloc_res.value();
  EXPECT_EQ(p.pfn(), 16u);

  // The front padding (16 pages: PFN 0..15) should be free as an Order 4 block
  EXPECT_TRUE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 4);

  // The allocation (PFN 16..19) is consumed
  EXPECT_FALSE(mock_ram[16].is_free);

  allocator.free_n(p, 4);
}

TEST_F(BuddyAllocatorTest, Constrained_HighPfnRejection) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 1024));

  // First, artificially consume the first 1000 pages so the free list only has high memory
  auto block_res = allocator.allocate_n(1000);
  ASSERT_TRUE(block_res.has_value());

  // Constraint: We need 10 pages, but they MUST be below PFN 1005 (legacy hardware limit).
  // We only have PFN 1000..1023 available.
  // PFN 1000 + 10 pages = ends at PFN 1009.
  // 1009 > 1005. It should reject this!
  test_allocator::physical_constraint c;
  c.low_pfn = 0;
  c.high_pfn = 1005;
  c.alignment_pages = 1;
  c.boundary_pages = 0;

  auto alloc_res = allocator.allocate_constrained(10, c);
  EXPECT_FALSE(alloc_res.has_value());
  EXPECT_EQ(alloc_res.error(), error::allocation_failed);

  // If we relax the constraint to 1017, it should succeed, because the
  // allocation will occupy PFN 1008 through PFN 1017 exactly.
  c.high_pfn = 1017;
  auto alloc_ok = allocator.allocate_constrained(10, c);
  ASSERT_TRUE(alloc_ok.has_value());
  EXPECT_EQ(alloc_ok.value().pfn(), 1008u);
}

TEST_F(BuddyAllocatorTest, Constrained_ExactFitNoPadding) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 16));

  // Constraint: We want 16 pages, aligned to 1 page, starting at PFN 0.
  // This exactly matches the root buddy block. Front padding = 0. Back padding = 0.
  test_allocator::physical_constraint c;
  c.low_pfn = 0;
  c.high_pfn = 100;
  c.alignment_pages = 1;
  c.boundary_pages = 0;

  auto alloc_res = allocator.allocate_constrained(16, c);
  ASSERT_TRUE(alloc_res.has_value());
  EXPECT_EQ(alloc_res.value().pfn(), 0u);

  // The block should be fully consumed, no rogue chunks freed.
  EXPECT_FALSE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 0); // Exact allocations are stamped with 0

  allocator.free_n(alloc_res.value(), 16);
  EXPECT_TRUE(mock_ram[0].is_free);
  EXPECT_EQ(mock_ram[0].buddy_order, 4); // Heals back to Order 4 (16 pages)
}

// ============================================================================
// Greedy / Opportunistic Allocation (allocate_up_to)
// ============================================================================

TEST_F(BuddyAllocatorTest, AllocateUpTo_ExactFit) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 1024));

  // We have plenty of memory (1024 pages).
  // Requesting 13 pages should succeed exactly via the allocate_n fast path.
  auto alloc_res = allocator.allocate_up_to(13);
  ASSERT_TRUE(alloc_res.has_value());

  EXPECT_EQ(alloc_res.value().count, 13u);
  EXPECT_EQ(alloc_res.value().page.pfn(), 0u);

  // Clean up
  allocator.free_n(alloc_res.value().page, 13);
}

TEST_F(BuddyAllocatorTest, AllocateUpTo_FragmentedFallback) {
  test_allocator allocator;

  // Initialize with exactly 15 pages.
  // This physically cannot form an Order 4 block (16 pages).
  // It will be carved into: Order 3 (8 pages), Order 2 (4 pages), Order 1 (2 pages), Order 0 (1 page).
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 15));

  // Request 10 pages.
  // allocate_n(10) bounds to Order 4, which is empty, so it fails.
  // allocate_up_to should catch this, calculate that the largest power-of-two <= 10 is Order 3 (8 pages),
  // and successfully return the 8-page block!
  auto res1 = allocator.allocate_up_to(10);
  ASSERT_TRUE(res1.has_value());
  EXPECT_EQ(res1.value().count, 8u);
  EXPECT_EQ(res1.value().page.pfn(), 0u); // The 8-page block is at PFN 0

  // Request 5 pages.
  // Remaining memory: Order 2 (4 pages), Order 1 (2 pages), Order 0 (1 page).
  // allocate_n(5) bounds to Order 3, which is now empty, so it fails.
  // allocate_up_to should fall back to Order 2 (4 pages).
  auto res2 = allocator.allocate_up_to(5);
  ASSERT_TRUE(res2.has_value());
  EXPECT_EQ(res2.value().count, 4u);
  EXPECT_EQ(res2.value().page.pfn(), 8u); // The 4-page block is at PFN 8

  // Request 1 page.
  // Remaining memory: Order 1 (2 pages), Order 0 (1 page).
  // allocate_n(1) bounds to Order 0, which exists! It should do an exact allocation.
  auto res3 = allocator.allocate_up_to(1);
  ASSERT_TRUE(res3.has_value());
  EXPECT_EQ(res3.value().count, 1u);
  // It pulls from the Order 0 list, which is the last page (PFN 14).
  EXPECT_EQ(res3.value().page.pfn(), 14u);
}

TEST_F(BuddyAllocatorTest, AllocateUpTo_OutOfMemory) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 4)); // Only 4 pages available

  // Consume all 4 pages
  auto consume = allocator.allocate_up_to(4);
  ASSERT_TRUE(consume.has_value());
  EXPECT_EQ(consume.value().count, 4u);

  // Now the allocator is entirely empty.
  // Asking for even 1 page should correctly return OOM.
  auto oom_res = allocator.allocate_up_to(1);
  EXPECT_FALSE(oom_res.has_value());
  EXPECT_EQ(oom_res.error(), error::allocation_failed);
}

TEST_F(BuddyAllocatorTest, AllocateUpTo_InvalidArguments) {
  test_allocator allocator;
  ASSERT_TRUE(allocator.init(test_page::from_os_page(0), 1024));

  // Asking for 0 pages is logically invalid
  auto zero_res = allocator.allocate_up_to(0);
  EXPECT_FALSE(zero_res.has_value());
  EXPECT_EQ(zero_res.error(), error::invalid_argument);
}

} // namespace

#endif
