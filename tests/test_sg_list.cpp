// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#if !defined(_MSC_VER) && defined(__LP64__)
#include <gtest/gtest.h>
#include <structo/sg_list.hpp>

using dynamic_sg_list = structo::sg_list<>;

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo;

TEST(SgListTest, PushBackDistinct) {
  dynamic_sg_list sgl;

  structo::phys_addr<void, structo::dma_bus_space> addr1(0x1000);
  structo::phys_addr<void, structo::dma_bus_space> addr2(0x8000); // Non-contiguous

  auto res1 = sgl.try_push_back(addr1, 4096);
  ASSERT_TRUE(res1.has_value());

  auto res2 = sgl.try_push_back(addr2, 8192);
  ASSERT_TRUE(res2.has_value());

  EXPECT_EQ(sgl.size(), 2u);

  auto it = sgl.begin();
  EXPECT_EQ(it->addr.value, 0x1000);
  EXPECT_EQ(it->length, 4096u);

  ++it;
  EXPECT_EQ(it->addr.value, 0x8000);
  EXPECT_EQ(it->length, 8192u);
}

TEST(SgListTest, AutomaticCoalescing) {
  dynamic_sg_list sgl;

  phys_addr<void, dma_bus_space> addr1(0x1000);
  phys_addr<void, dma_bus_space> addr2(0x2000); // Contiguous with addr1 + 4096
  phys_addr<void, dma_bus_space> addr3(0x3000); // Contiguous with addr2 + 4096

  EXPECT_TRUE(sgl.try_push_back(addr1, 4096).has_value());
  EXPECT_EQ(sgl.size(), 1u);

  // Pushing contiguous memory should NOT increase the vector size
  EXPECT_TRUE(sgl.try_push_back(addr2, 4096).has_value());
  EXPECT_EQ(sgl.size(), 1u);

  EXPECT_TRUE(sgl.try_push_back(addr3, 8192).has_value());
  EXPECT_EQ(sgl.size(), 1u);

  auto it = sgl.begin();
  EXPECT_EQ(it->addr.value, 0x1000);
  EXPECT_EQ(it->length, 16384u); // 4096 + 4096 + 8192
}

TEST(SgListTest, RejectNullAndZeroLength) {
  dynamic_sg_list sgl;

  // Zero length should succeed but do nothing
  EXPECT_TRUE(sgl.try_push_back(phys_addr<void, dma_bus_space>(0x1000), 0).has_value());
  EXPECT_TRUE(sgl.empty());

  // Null address should fail
  auto res = sgl.try_push_back(nullptr, 4096);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::invalid_argument);
}

namespace {

class SgListCursorTest : public ::testing::Test {
protected:
  dynamic_sg_list sgl;
};

} // namespace

TEST_F(SgListCursorTest, SplitsOneEntryIntoChunksUpToMaxLength) {
  ASSERT_TRUE(sgl.try_push_back(phys_addr<void, dma_bus_space>(0x1000), 10000).has_value());

  sg_list_cursor cursor(sgl);

  auto c1 = cursor.next_up_to<dma_bus_space>(4096);
  ASSERT_TRUE(c1.has_value());
  EXPECT_EQ(c1->addr.value, 0x1000u);
  EXPECT_EQ(c1->length, 4096u);

  auto c2 = cursor.next_up_to<dma_bus_space>(4096);
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(c2->addr.value, 0x1000u + 4096u);
  EXPECT_EQ(c2->length, 4096u);

  // Remainder of the entry (10000 - 8192 = 1808) is shorter than max_length.
  auto c3 = cursor.next_up_to<dma_bus_space>(4096);
  ASSERT_TRUE(c3.has_value());
  EXPECT_EQ(c3->addr.value, 0x1000u + 8192u);
  EXPECT_EQ(c3->length, 1808u);

  auto c4 = cursor.next_up_to<dma_bus_space>(4096);
  EXPECT_FALSE(c4.has_value());
  EXPECT_EQ(c4.error(), error::out_of_bounds);
}

TEST_F(SgListCursorTest, NeverCrossesAnOriginalEntryBoundary) {
  ASSERT_TRUE(sgl.try_push_back(phys_addr<void, dma_bus_space>(0x1000), 4096).has_value());
  ASSERT_TRUE(sgl.try_push_back(phys_addr<void, dma_bus_space>(0x8000), 4096).has_value()); // non-contiguous

  sg_list_cursor cursor(sgl);

  // A chunk request large enough to span both entries still stops at the first entry's end.
  auto c1 = cursor.next_up_to<dma_bus_space>(1'000'000);
  ASSERT_TRUE(c1.has_value());
  EXPECT_EQ(c1->addr.value, 0x1000u);
  EXPECT_EQ(c1->length, 4096u);

  auto c2 = cursor.next_up_to<dma_bus_space>(1'000'000);
  ASSERT_TRUE(c2.has_value());
  EXPECT_EQ(c2->addr.value, 0x8000u);
  EXPECT_EQ(c2->length, 4096u);

  auto c3 = cursor.next_up_to<dma_bus_space>(1'000'000);
  EXPECT_FALSE(c3.has_value());
  EXPECT_EQ(c3.error(), error::out_of_bounds);
}

TEST_F(SgListCursorTest, RejectsZeroMaxLength) {
  ASSERT_TRUE(sgl.try_push_back(phys_addr<void, dma_bus_space>(0x1000), 4096).has_value());

  sg_list_cursor cursor(sgl);
  auto res = cursor.next_up_to<dma_bus_space>(0);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::invalid_argument);
}

TEST_F(SgListCursorTest, OutOfBoundsOnEmptyList) {
  sg_list_cursor cursor(sgl);
  auto res = cursor.next_up_to<dma_bus_space>(4096);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::out_of_bounds);
}

TEST_F(SgListCursorTest, CopyingCursorAllowsIndependentMultiplePasses) {
  ASSERT_TRUE(sgl.try_push_back(phys_addr<void, dma_bus_space>(0x1000), 8192).has_value());

  sg_list_cursor cursor(sgl);
  auto first_chunk = cursor.next_up_to<dma_bus_space>(4096);
  ASSERT_TRUE(first_chunk.has_value());

  // Copy after partial consumption: the copy continues independently from
  // where the original left off, and advancing one does not affect the other.
  sg_list_cursor cursor_copy = cursor;

  auto orig_next = cursor.next_up_to<dma_bus_space>(4096);
  ASSERT_TRUE(orig_next.has_value());
  EXPECT_EQ(orig_next->addr.value, 0x1000u + 4096u);

  auto copy_next = cursor_copy.next_up_to<dma_bus_space>(2048);
  ASSERT_TRUE(copy_next.has_value());
  EXPECT_EQ(copy_next->addr.value, 0x1000u + 4096u);
  EXPECT_EQ(copy_next->length, 2048u);
}

namespace {
/** @brief A distinct address-space tag, unrelated to `dma_bus_space`, used only to exercise type mismatches. */
struct unrelated_space_tag {};
} // namespace

TEST_F(SgListCursorTest, HoldsReportsTheErasedEntryTypeSpaceTag) {
  ASSERT_TRUE(sgl.try_push_back(phys_addr<void, dma_bus_space>(0x1000), 4096).has_value());

  sg_list_cursor cursor(sgl);
  EXPECT_TRUE(cursor.holds<dma_bus_space>());
  EXPECT_FALSE(cursor.holds<unrelated_space_tag>());
  EXPECT_EQ(cursor.entry_type_id(), type_id::of<sg_entry<dma_bus_space>>());
}

TEST_F(SgListCursorTest, NextUpToRejectsMismatchedSpaceTag) {
  ASSERT_TRUE(sgl.try_push_back(phys_addr<void, dma_bus_space>(0x1000), 4096).has_value());

  sg_list_cursor cursor(sgl);
  auto res = cursor.next_up_to<unrelated_space_tag>(4096);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::invalid_argument);
}

RELOCO_END_UNSAFE_BUFFER_USAGE
#endif
