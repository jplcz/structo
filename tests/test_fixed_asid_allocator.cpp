// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/fixed_asid_allocator.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

using structo::arch::fixed_asid;
using structo::arch::fixed_asid_allocator;
using structo::arch::process_asid_tag;
using structo::arch::vmid_tag;

namespace {

/** @brief Fixture for `fixed_asid_allocator` tests; each test builds the allocator with the asid_bits/Capacity it
 * needs. */
class FixedAsidAllocatorTest : public ::testing::Test {};

using proc4 = fixed_asid_allocator<process_asid_tag, 4>;

} // namespace

TEST_F(FixedAsidAllocatorTest, TryCreateRejectsOutOfRangeAsidBits) {
  EXPECT_FALSE(proc4::try_create(0).has_value());
  EXPECT_FALSE(proc4::try_create(proc4::max_asid_bits + 1).has_value());
}

TEST_F(FixedAsidAllocatorTest, TryCreateRejectsAsidSpaceNarrowerThanCapacity) {
  // 1 bit -> 2 asids, narrower than Capacity == 4.
  EXPECT_FALSE(proc4::try_create(1).has_value());
}

TEST_F(FixedAsidAllocatorTest, TryCreateSucceedsAtExactCapacity) {
  // 2 bits -> 4 asids, exactly Capacity == 4.
  auto made = proc4::try_create(2);
  ASSERT_TRUE(made.has_value());
  EXPECT_EQ(made.value().asid_bits(), 2u);
  EXPECT_EQ(made.value().used_count(), 0u);
}

TEST_F(FixedAsidAllocatorTest, DefaultContextIdIsInvalid) {
  proc4::context_id id;
  EXPECT_FALSE(id.is_valid());
  EXPECT_FALSE(static_cast<bool>(id));
}

TEST_F(FixedAsidAllocatorTest, AllocateProducesValidId) {
  auto alloc = proc4::try_create(8).value();

  auto r = alloc.allocate();
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r.value().is_valid());
  EXPECT_LT(alloc.slot_of(r.value()), proc4::capacity);
  EXPECT_EQ(alloc.used_count(), 1u);
}

TEST_F(FixedAsidAllocatorTest, DistinctAllocationsGetDistinctAsids) {
  auto alloc = proc4::try_create(8).value();

  auto r1 = alloc.allocate();
  auto r2 = alloc.allocate();
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());
  EXPECT_NE(r1.value(), r2.value());
  EXPECT_NE(alloc.slot_of(r1.value()), alloc.slot_of(r2.value()));
}

TEST_F(FixedAsidAllocatorTest, AllocateFailsWhenCapacityExhausted) {
  auto alloc = proc4::try_create(8).value();

  for (std::size_t i = 0; i < proc4::capacity; ++i) {
    auto r = alloc.allocate();
    ASSERT_TRUE(r.has_value());
  }
  EXPECT_EQ(alloc.used_count(), proc4::capacity);

  auto r = alloc.allocate();
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::allocation_failed);
}

TEST_F(FixedAsidAllocatorTest, ReserveRemovesAsidFromCirculation) {
  auto alloc = proc4::try_create(8).value();

  ASSERT_TRUE(alloc.reserve(0).has_value());
  EXPECT_EQ(alloc.used_count(), 1u);

  for (std::size_t i = 0; i < proc4::capacity - 1; ++i) {
    auto r = alloc.allocate();
    ASSERT_TRUE(r.has_value());
    EXPECT_NE(alloc.slot_of(r.value()), 0u);
  }
}

TEST_F(FixedAsidAllocatorTest, ReserveRejectsOutOfRangeAsid) {
  auto alloc = proc4::try_create(8).value();
  EXPECT_FALSE(alloc.reserve(proc4::capacity).has_value());
}

TEST_F(FixedAsidAllocatorTest, ReleaseThenReallocateReusesTheFreedSlot) {
  auto alloc = proc4::try_create(8).value();

  auto r1 = alloc.allocate();
  ASSERT_TRUE(r1.has_value());
  auto id1 = r1.value();
  alloc.release(id1);
  EXPECT_EQ(alloc.used_count(), 0u);

  auto r2 = alloc.allocate();
  ASSERT_TRUE(r2.has_value());
  // Same stable slot identity reused...
  EXPECT_EQ(alloc.slot_of(r2.value()), alloc.slot_of(id1));
  // ...but a visibly different raw hardware ASID value, thanks to the
  // bumped cosmetic sequence prefix (asid_bits=8, slot_bits=2: 6 spare bits).
  EXPECT_NE(alloc.asid_of(r2.value()), alloc.asid_of(id1));
}

TEST_F(FixedAsidAllocatorTest, SequenceCounterIsCosmeticOnlyWhenNoSpareBits) {
  // asid_bits == slot_bits (2): no spare bits, so the raw value is just
  // the slot index, every time -- the sequence counter has nothing to
  // pack into and degenerates to always-zero.
  auto alloc = proc4::try_create(2).value();

  auto r1 = alloc.allocate();
  ASSERT_TRUE(r1.has_value());
  alloc.release(r1.value());
  auto r2 = alloc.allocate();
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(alloc.asid_of(r2.value()), alloc.asid_of(r1.value()));
}

TEST_F(FixedAsidAllocatorTest, SequenceGroupIsSharedAcrossNeighboringSlots) {
  // SeqGroupSize == 2: slots 0 and 1 share one sequence counter, so
  // allocating slot 0 already advances what slot 1's own raw value will
  // carry, even though slot 1 was never touched before.
  using grouped = fixed_asid_allocator<process_asid_tag, 2, 2>;
  auto alloc = grouped::try_create(8).value();

  auto r0 = alloc.allocate(); // slot 0: bumps the shared group counter to 1
  ASSERT_TRUE(r0.has_value());
  auto r1 = alloc.allocate(); // slot 1: bumps the same shared counter to 2
  ASSERT_TRUE(r1.has_value());

  ASSERT_NE(alloc.slot_of(r0.value()), alloc.slot_of(r1.value()));
  // Both slots' sequence prefixes came from the one shared counter, so
  // strip the (different) slot bits and compare just the prefix.
  const auto prefix0 = alloc.asid_of(r0.value()) >> grouped::slot_bits;
  const auto prefix1 = alloc.asid_of(r1.value()) >> grouped::slot_bits;
  EXPECT_NE(prefix0, prefix1);
  EXPECT_EQ(prefix1, prefix0 + 1);
}

TEST_F(FixedAsidAllocatorTest, ReleaseIsNoOpForInvalidId) {
  auto alloc = proc4::try_create(8).value();

  proc4::context_id invalid;
  alloc.release(invalid); // must not crash or corrupt state
  EXPECT_EQ(alloc.used_count(), 0u);
}

TEST_F(FixedAsidAllocatorTest, MoveConstructionTransfersState) {
  auto alloc = proc4::try_create(8).value();
  auto r = alloc.allocate();
  ASSERT_TRUE(r.has_value());

  proc4 moved(std::move(alloc));
  EXPECT_EQ(moved.used_count(), 1u);
}

TEST_F(FixedAsidAllocatorTest, FixedAsidTypesAreDistinctAcrossTags) {
  static_assert(!std::is_same_v<fixed_asid_allocator<process_asid_tag, 4>::context_id,
                                fixed_asid_allocator<vmid_tag, 4>::context_id>,
                "process_asid_tag and vmid_tag context_ids must be distinct types");
  static_assert(!std::is_same_v<fixed_asid<process_asid_tag>, fixed_asid<vmid_tag>>,
                "fixed_asid<Tag> must be distinct per Tag");
}

TEST_F(FixedAsidAllocatorTest, FixedAsidIsDistinctFromTaggedAsidEvenForSameTag) {
  static_assert(!std::is_same_v<fixed_asid<process_asid_tag>, structo::arch::tagged_asid<process_asid_tag>>,
                "fixed_asid<Tag> must never be the same type as tagged_asid<Tag>");
}

TEST_F(FixedAsidAllocatorTest, VmidAllocatorIsIndependentFromProcessAllocator) {
  using vm_alloc = fixed_asid_allocator<vmid_tag, 2>;
  auto made = vm_alloc::try_create(4).value();
  // Unrelated type from the process-ASID instantiation above.
  static_assert(!std::is_same_v<decltype(made), proc4>);

  auto r = made.allocate();
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r.value().is_valid());
}
