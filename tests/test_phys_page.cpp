#if !defined(_MSC_VER)
#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/phys_page.hpp>

namespace {

using namespace structo;

// ============================================================================
// Mock Environment Setup
// ============================================================================

// A minimal metadata struct to mock physical RAM backing
struct mock_meta_t {
  uint16_t buddy_order{0};
  bool is_free{false};
  uint16_t zone_id{0};
};

// Mock physical memory map (1024 pages)
static constexpr size_t MOCK_MEM_PAGES = 1024;
static array<mock_meta_t, MOCK_MEM_PAGES> mock_mem_map;

// CRTP OS Traits using a compressed 32-bit integer as the page handle
struct test_os_traits : os_traits_base<test_os_traits, uint32_t> {
  using os_page_type = uint32_t;

  static os_page_type null_page() noexcept { return ~uint32_t(0); }
  static bool is_null(os_page_type p) noexcept { return p == null_page(); }

  // Handles 0xF0000000..0xF0000009 (outside the mock RAM) are dummy marker descriptors (queue cursors), not physical pages.
  static constexpr bool is_marker(os_page_type p) noexcept { return p >= 0xF0000000u && p < 0xF000000Au; }

  static uint64_t to_pfn(os_page_type p) noexcept { return p; }

  static result<os_page_type> from_pfn(uint64_t pfn) noexcept {
    if (pfn >= MOCK_MEM_PAGES) {
      return unexpected(error::out_of_range);
    }
    return static_cast<uint32_t>(pfn);
  }

  static bool is_same_zone(os_page_type a, os_page_type b) noexcept {
    return mock_mem_map[a].zone_id == mock_mem_map[b].zone_id;
  }

  static uint16_t buddy_order(os_page_type p) noexcept { return mock_mem_map[p].buddy_order; }
  static void set_buddy_order(os_page_type p, uint16_t order) noexcept { mock_mem_map[p].buddy_order = order; }

  static bool is_buddy_free(os_page_type p) noexcept { return mock_mem_map[p].is_free; }
  static void set_buddy_free(os_page_type p, bool free) noexcept { mock_mem_map[p].is_free = free; }
};

using test_page = page_view<page_4k, test_os_traits>;

// ============================================================================
// Test Fixture
// ============================================================================

class PageViewTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Reset the memory map before each test
    for (size_t i = 0; i < MOCK_MEM_PAGES; i++) {
      mock_mem_map[i].buddy_order = 0;
      mock_mem_map[i].is_free = false;
      // Zone 0: PFNs 0-511
      // Zone 1: PFNs 512-1023
      mock_mem_map[i].zone_id = (i < 512) ? 0 : 1;
    }
  }
};

// ============================================================================
// Tests
// ============================================================================

TEST_F(PageViewTest, TraitsConstants) {
  EXPECT_EQ(page_4k::page_size, 4096u);
  EXPECT_EQ(page_4k::page_shift, 12u);
  EXPECT_EQ(page_4k::alignment_mask, 4095u);

  EXPECT_EQ(page_2m::page_size, 2097152u);
  EXPECT_EQ(page_2m::page_shift, 21u);
}

TEST_F(PageViewTest, ConstructionAndNullity) {
  test_page p_null;
  EXPECT_TRUE(p_null.is_null());
  EXPECT_FALSE(static_cast<bool>(p_null));
  EXPECT_EQ(p_null.pfn(), 0u);
  EXPECT_TRUE(p_null.phys().is_null());

  test_page p_nullptr = nullptr;
  EXPECT_TRUE(p_nullptr.is_null());

  test_page p_valid = test_page::from_os_page(42);
  EXPECT_FALSE(p_valid.is_null());
  EXPECT_TRUE(static_cast<bool>(p_valid));
  EXPECT_EQ(p_valid.get_os_page(), 42u);
  EXPECT_EQ(p_valid.pfn(), 42u);
}

TEST_F(PageViewTest, ValidArithmeticWithinZone) {
  auto p = test_page::from_os_page(10); // Zone 0

  auto p_add = p.try_add(50);
  ASSERT_TRUE(p_add.has_value());
  EXPECT_EQ(p_add.value().pfn(), 60u); // Still in Zone 0

  auto p_sub = p_add.value().try_sub(20);
  ASSERT_TRUE(p_sub.has_value());
  EXPECT_EQ(p_sub.value().pfn(), 40u); // Still in Zone 0
}

TEST_F(PageViewTest, ArithmeticOutOfBounds) {
  auto p = test_page::from_os_page(1000); // Zone 1

  // 1. Math exceeding mock OS RAM layout
  auto add_oob = p.try_add(50);
  EXPECT_FALSE(add_oob.has_value());
  EXPECT_EQ(add_oob.error(), error::out_of_range);

  // 2. Math resulting in underflow
  auto p_low = test_page::from_os_page(5);
  auto sub_underflow = p_low.try_sub(10);
  EXPECT_FALSE(sub_underflow.has_value());
  EXPECT_EQ(sub_underflow.error(), error::out_of_range);

  // 3. Math resulting in absolute uint64_t overflow
  auto massive_count = ~uint64_t(0);
  auto add_overflow = p.try_add(massive_count);
  EXPECT_FALSE(add_overflow.has_value());
  EXPECT_EQ(add_overflow.error(), error::out_of_range);
}

TEST_F(PageViewTest, ArithmeticZoneCrossings) {
  auto p_end_zone0 = test_page::from_os_page(510);

  // Adding 1 is fine (PFN 511 is Zone 0)
  auto add_ok = p_end_zone0.try_add(1);
  ASSERT_TRUE(add_ok.has_value());

  // Adding 2 crosses into Zone 1 (PFN 512)
  auto add_cross = p_end_zone0.try_add(2);
  EXPECT_FALSE(add_cross.has_value());
  EXPECT_EQ(add_cross.error(), error::security_violation);

  auto p_start_zone1 = test_page::from_os_page(513);

  // Subtracting 1 is fine (PFN 512 is Zone 1)
  auto sub_ok = p_start_zone1.try_sub(1);
  ASSERT_TRUE(sub_ok.has_value());

  // Subtracting 2 crosses back into Zone 0 (PFN 511)
  auto sub_cross = p_start_zone1.try_sub(2);
  EXPECT_FALSE(sub_cross.has_value());
  EXPECT_EQ(sub_cross.error(), error::security_violation);
}

TEST_F(PageViewTest, BuddyOperationsAndMetadata) {
  auto p = test_page::from_os_page(8);

  p.set_buddy_order(3);
  p.set_buddy_free(true);

  EXPECT_EQ(p.buddy_order(), 3);
  EXPECT_TRUE(p.is_buddy_free());

  EXPECT_EQ(mock_mem_map[8].buddy_order, 3);
  EXPECT_TRUE(mock_mem_map[8].is_free);

  // Buddy of PFN 8 at order 3 (1<<3 = 8) is PFN 0
  auto buddy_res = p.try_get_buddy(3);
  ASSERT_TRUE(buddy_res.has_value());
  EXPECT_EQ(buddy_res.value().pfn(), 0u);

  // Buddy of PFN 8 at order 2 (1<<2 = 4) is PFN 12
  auto buddy_res2 = p.try_get_buddy(2);
  ASSERT_TRUE(buddy_res2.has_value());
  EXPECT_EQ(buddy_res2.value().pfn(), 12u);
}

TEST_F(PageViewTest, BuddyZoneCrossings) {
  // PFN 0 (Zone 0) at Order 9 (512 pages). The buddy is PFN 512.
  // But PFN 512 is the start of Zone 1!
  auto p = test_page::from_os_page(0);

  auto buddy_res = p.try_get_buddy(9);
  EXPECT_FALSE(buddy_res.has_value());

  // The mathematical calculation succeeded (512 is within 1024 RAM bound),
  // but it was rejected by the zone continuity check.
  EXPECT_EQ(buddy_res.error(), error::security_violation);
}

TEST_F(PageViewTest, PhysicalAddressTranslations) {
  auto p = test_page::from_os_page(5); // PFN 5

  // For 4K pages (Shift 12), PFN 5 -> 0x5000
  phys_addr<void, host_phys_space> paddr = p.phys();
  EXPECT_FALSE(paddr.is_null());
  EXPECT_EQ(paddr.value, 0x5000u);

  test_page p_null;
  EXPECT_TRUE(p_null.phys().is_null());
}

TEST_F(PageViewTest, EqualityComparisons) {
  auto p1 = test_page::from_os_page(42);
  auto p2 = test_page::from_os_page(42);
  auto p3 = test_page::from_os_page(99);

  EXPECT_TRUE(p1 == p2);
  EXPECT_FALSE(p1 != p2);

  EXPECT_TRUE(p1 != p3);
  EXPECT_FALSE(p1 == p3);
}

TEST_F(PageViewTest, MarkerCanOnlyBeProbed) {
  // A marker is only ever probed: is_marker() is true for it, and it is a valid (non-null) handle.
  auto m = test_page::from_os_page(0xF0000000u);
  EXPECT_TRUE(m.is_marker());
  EXPECT_FALSE(m.is_null());
  EXPECT_TRUE(test_page::from_os_page(0xF0000009u).is_marker());

  // Real pages, the null page and the handles just outside the marker range are not markers.
  EXPECT_FALSE(test_page::from_os_page(0xEFFFFFFFu).is_marker());
  EXPECT_FALSE(test_page::from_os_page(0xF000000Au).is_marker());
  EXPECT_FALSE(test_page::from_os_page(5).is_marker());
  EXPECT_FALSE(test_page{}.is_marker());

  // Probing leaves the metadata alone and normal pages keep working.
  EXPECT_FALSE(mock_mem_map[5].is_free);
  EXPECT_TRUE(test_page::from_os_page(5).try_add(1).has_value());
}

TEST_F(PageViewTest, DefaultTraitsHaveNoMarkers) {
  using base = os_traits_base<test_os_traits, uint32_t>;
  const bool marker = base::is_marker(0);
  EXPECT_FALSE(marker);
}

} // namespace
#endif
