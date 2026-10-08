// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/lifetime.hpp>
#include <structo/arch/asid_allocator.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

using structo::arch::asid_allocator;
using structo::arch::process_asid_tag;
using structo::arch::tagged_asid;
using structo::arch::vmid_tag;

namespace {

/** @brief Fixture for `asid_allocator` tests; each test builds the allocator with the asid_bits/MaxActive it needs. */
class AsidAllocatorTest : public ::testing::Test {};

using proc4 = asid_allocator<process_asid_tag, 4>;

} // namespace

TEST_F(AsidAllocatorTest, TryCreateRejectsOutOfRangeAsidBits) {
  EXPECT_FALSE(proc4::try_create(0).has_value());
  EXPECT_FALSE(proc4::try_create(proc4::max_asid_bits + 1).has_value());
}

TEST_F(AsidAllocatorTest, TryCreateRejectsAsidSpaceNotExceedingMaxActive) {
  // 1 bit -> 2 asids, which can never outnumber MaxActive == 4.
  EXPECT_FALSE(proc4::try_create(1).has_value());
}

TEST_F(AsidAllocatorTest, TryCreateSucceedsAndReportsProperties) {
  auto made = proc4::try_create(8);
  ASSERT_TRUE(made.has_value());
  auto alloc = std::move(made.value());

  EXPECT_EQ(alloc.asid_bits(), 8u);
  EXPECT_EQ(alloc.num_asids(), 256u);
  EXPECT_EQ(alloc.generation(), 1u);
  EXPECT_EQ(alloc.used_count(), 0u);
}

TEST_F(AsidAllocatorTest, DefaultContextIdIsInvalid) {
  proc4::context_id id;
  EXPECT_FALSE(id.is_valid());
  EXPECT_FALSE(static_cast<bool>(id));
}

TEST_F(AsidAllocatorTest, AllocateProducesValidIdWithoutFlush) {
  auto alloc = proc4::try_create(8).value();

  auto r = alloc.allocate(0);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r.value().id.is_valid());
  EXPECT_FALSE(r.value().flush_required);
  EXPECT_LT(alloc.asid_of(r.value().id), alloc.num_asids());
  EXPECT_EQ(alloc.used_count(), 1u);
}

TEST_F(AsidAllocatorTest, ReallocatingWithStillValidPrevIsFastPathNoOp) {
  auto alloc = proc4::try_create(8).value();

  auto r1 = alloc.allocate(0);
  ASSERT_TRUE(r1.has_value());
  auto id1 = r1.value().id;

  auto r2 = alloc.allocate(0, id1);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2.value().id, id1);
  EXPECT_FALSE(r2.value().flush_required);
  // No extra bit consumed by the fast path.
  EXPECT_EQ(alloc.used_count(), 1u);
}

TEST_F(AsidAllocatorTest, DistinctSlotsGetDistinctAsids) {
  auto alloc = proc4::try_create(8).value();

  auto r1 = alloc.allocate(0);
  auto r2 = alloc.allocate(1);
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  EXPECT_NE(r1.value().id, r2.value().id);
  EXPECT_NE(alloc.asid_of(r1.value().id), alloc.asid_of(r2.value().id));
}

TEST_F(AsidAllocatorTest, AllocateRejectsOutOfRangeSlot) {
  auto alloc = proc4::try_create(8).value();
  auto r = alloc.allocate(proc4::max_active);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);
}

TEST_F(AsidAllocatorTest, ReserveRemovesAsidFromCirculation) {
  auto alloc = proc4::try_create(4).value(); // 16 asids

  ASSERT_TRUE(alloc.reserve(0).has_value());
  EXPECT_EQ(alloc.used_count(), 1u);

  // Allocate every remaining asid; none should ever be asid 0.
  for (std::size_t i = 0; i < alloc.num_asids() - 1; ++i) {
    auto r = alloc.allocate(0);
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(alloc.asid_of(r.value().id), 0u);
    alloc.release(r.value().id);
  }
}

TEST_F(AsidAllocatorTest, ReserveRejectsOutOfRangeAsid) {
  auto alloc = proc4::try_create(4).value();
  EXPECT_FALSE(alloc.reserve(alloc.num_asids()).has_value());
}

TEST_F(AsidAllocatorTest, ReleaseThenReallocateReusesTheFreedBit) {
  auto alloc = proc4::try_create(4).value(); // 16 asids, small enough to force reuse

  auto r1 = alloc.allocate(0);
  ASSERT_TRUE(r1.has_value());
  auto id1 = r1.value().id;
  alloc.release(id1);
  EXPECT_EQ(alloc.used_count(), 0u);

  auto r2 = alloc.allocate(1);
  ASSERT_TRUE(r2.has_value());
  EXPECT_FALSE(r2.value().flush_required);
}

TEST_F(AsidAllocatorTest, ReleaseIsNoOpForInvalidOrStaleId) {
  auto alloc = proc4::try_create(4).value();

  proc4::context_id invalid;
  alloc.release(invalid); // must not crash or corrupt state
  EXPECT_EQ(alloc.used_count(), 0u);
}

TEST_F(AsidAllocatorTest, RolloverOccursWhenAsidSpaceIsExhausted) {
  // asid_bits = 3 -> 8 asids, MaxActive = 4: easy to exhaust deterministically.
  auto alloc = proc4::try_create(3).value();
  ASSERT_EQ(alloc.num_asids(), 8u);

  // Keep slot 0 permanently "active" so we can verify it survives rollover.
  auto r0 = alloc.allocate(0);
  ASSERT_TRUE(r0.has_value());
  auto id0 = r0.value().id;

  bool saw_flush = false;
  proc4::context_id last_id;
  for (int i = 0; i < 64 && !saw_flush; ++i) {
    // Slot 1 repeatedly asks for a *fresh* id (never pass prev back), to
    // force the bitmap towards exhaustion.
    auto r = alloc.allocate(1);
    ASSERT_TRUE(r.has_value());
    last_id = r.value().id;
    if (r.value().flush_required) {
      saw_flush = true;
    }
  }
  ASSERT_TRUE(saw_flush);
  EXPECT_GT(alloc.generation(), 1u);

  // Slot 0's context survived the rollover: still valid, same hardware
  // asid bits, bumped to the new generation.
  EXPECT_TRUE(alloc.active(0).is_valid());
  EXPECT_EQ(alloc.asid_of(alloc.active(0)), alloc.asid_of(id0));
  EXPECT_EQ(alloc.active(1), last_id);
}

TEST_F(AsidAllocatorTest, TryActiveReportsOutOfRangeSlot) {
  auto alloc = proc4::try_create(4).value();

  auto try_res = alloc.try_active(proc4::max_active);
  ASSERT_FALSE(try_res.has_value());
  EXPECT_EQ(try_res.error(), reloco::error::invalid_argument);
}

// Death Test (run only if debug asserts are enabled) -- see
// reloco/tests/test_packed_bits.cpp for the same guard/convention.
#if !defined(NDEBUG) || defined(RELOCO_ENABLE_ASSERTS)
TEST_F(AsidAllocatorTest, ActiveTrapsOnOutOfRangeSlot) {
  auto alloc = proc4::try_create(4).value();
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ (void)alloc.active(proc4::max_active); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}
#endif

TEST_F(AsidAllocatorTest, GenerationOfReportsAllocationGeneration) {
  auto alloc = proc4::try_create(4).value();
  auto r = alloc.allocate(0);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(alloc.generation_of(r.value().id), alloc.generation());
}

TEST_F(AsidAllocatorTest, MoveConstructionTransfersState) {
  auto alloc = proc4::try_create(8).value();
  auto r = alloc.allocate(0);
  ASSERT_TRUE(r.has_value());

  proc4 moved(std::move(alloc));
  EXPECT_EQ(moved.used_count(), 1u);
  EXPECT_EQ(moved.active(0), r.value().id);
}

TEST_F(AsidAllocatorTest, TaggedAsidTypesAreDistinctAcrossTags) {
  static_assert(
      !std::is_same_v<asid_allocator<process_asid_tag, 4>::context_id, asid_allocator<vmid_tag, 4>::context_id>,
      "process_asid_tag and vmid_tag context_ids must be distinct types");
  static_assert(!std::is_same_v<tagged_asid<process_asid_tag>, tagged_asid<vmid_tag>>,
                "tagged_asid<Tag> must be distinct per Tag");
}

TEST_F(AsidAllocatorTest, VmidAllocatorIsIndependentFromProcessAllocator) {
  using vm_alloc = asid_allocator<vmid_tag, 2>;
  auto made = vm_alloc::try_create(4).value();
  // Unrelated type from the process-ASID instantiation above.
  static_assert(!std::is_same_v<decltype(made), proc4>);

  auto r = made.allocate(0);
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r.value().id.is_valid());
}
