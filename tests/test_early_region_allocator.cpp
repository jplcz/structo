// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/early_region_allocator.hpp>

using structo::early_region_allocator;
using structo::error;
using structo::region_set;

TEST(EarlyRegionAllocatorTest, AllocatesFromTheOnlyRegionAndShrinksFreeBytes) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1000, 0x1000));

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);
  auto &alloc = *made;
  EXPECT_EQ(alloc.free_bytes(), 0x1000u);

  auto result = alloc.try_alloc(0x100);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->base, 0x1000u);
  EXPECT_EQ(result->size, 0x100u);
  EXPECT_EQ(alloc.free_bytes(), 0x1000u - 0x100u);

  // The seed region_set passed into try_create() is never mutated.
  ASSERT_EQ(seed.size(), 1u);
  EXPECT_EQ(seed[0].base, 0x1000u);
  EXPECT_EQ(seed[0].size, 0x1000u);
}

TEST(EarlyRegionAllocatorTest, AlignsTheReturnedBaseAndAccountsForPadding) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1010, 0x1000)); // base not aligned to 0x100

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);
  auto &alloc = *made;

  auto result = alloc.try_alloc(0x100, 0x100);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->base, 0x1100u);
  EXPECT_EQ(result->size, 0x100u);

  // The allocation splits the region: the small alignment padding
  // (0x1010..0x1100) stays free, as does everything past the allocation.
  ASSERT_EQ(alloc.regions().size(), 2u);
  EXPECT_EQ(alloc.regions()[0].base, 0x1010u);
  EXPECT_EQ(alloc.regions()[0].size, 0xf0u);
  EXPECT_EQ(alloc.regions()[1].base, 0x1200u);
  EXPECT_EQ(alloc.regions()[1].size, 0xe10u);
}

TEST(EarlyRegionAllocatorTest, PicksTheBestFitRegionToMinimizeWaste) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1000, 0x1000));   // 4 KiB region: exact fit
  ASSERT_TRUE(seed.try_add(0x10000, 0x10000)); // 64 KiB region: would waste far more

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);
  auto &alloc = *made;

  auto result = alloc.try_alloc(0x1000);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->base, 0x1000u);
  EXPECT_EQ(result->size, 0x1000u);

  // The smaller, exact-fit region is gone; the larger one is untouched.
  ASSERT_EQ(alloc.regions().size(), 1u);
  EXPECT_EQ(alloc.regions()[0].base, 0x10000u);
  EXPECT_EQ(alloc.regions()[0].size, 0x10000u);
}

TEST(EarlyRegionAllocatorTest, FailsWithAllocationFailedWhenNothingFits) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1000, 0x100));

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);

  auto result = made->try_alloc(0x1000);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), error::allocation_failed);
}

TEST(EarlyRegionAllocatorTest, FailsWithInvalidArgumentOnZeroSizeOrBadAlignment) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1000, 0x1000));
  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);

  auto zero_size = made->try_alloc(0);
  ASSERT_FALSE(zero_size);
  EXPECT_EQ(zero_size.error(), error::invalid_argument);

  auto bad_alignment = made->try_alloc(0x100, 3);
  ASSERT_FALSE(bad_alignment);
  EXPECT_EQ(bad_alignment.error(), error::invalid_argument);
}

TEST(EarlyRegionAllocatorTest, ReservesAnExactKnownRangeInsideAFreeRegion) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x0, 0x10000));

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);
  auto &alloc = *made;
  ASSERT_TRUE(alloc.try_reserve(0x2000, 0x1000));

  ASSERT_EQ(alloc.regions().size(), 2u);
  EXPECT_EQ(alloc.regions()[0].base, 0x0u);
  EXPECT_EQ(alloc.regions()[0].size, 0x2000u);
  EXPECT_EQ(alloc.regions()[1].base, 0x3000u);
  EXPECT_EQ(alloc.regions()[1].size, 0xd000u);

  // The seed is still exactly what it was.
  ASSERT_EQ(seed.size(), 1u);
  EXPECT_EQ(seed[0].base, 0x0u);
  EXPECT_EQ(seed[0].size, 0x10000u);
}

TEST(EarlyRegionAllocatorTest, ReserveFailsWhenRangeIsOnlyPartiallyFree) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1000, 0x1000));

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);
  auto &alloc = *made;

  // Overlaps the free region but extends past its end.
  auto result = alloc.try_reserve(0x1800, 0x1000);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), error::invalid_state);

  // The region set is untouched by the rejected reservation.
  ASSERT_EQ(alloc.regions().size(), 1u);
  EXPECT_EQ(alloc.regions()[0].base, 0x1000u);
  EXPECT_EQ(alloc.regions()[0].size, 0x1000u);
}

TEST(EarlyRegionAllocatorTest, ReserveFailsWhenRangeIsAlreadyReserved) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1000, 0x1000));

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);
  auto &alloc = *made;
  ASSERT_TRUE(alloc.try_reserve(0x1000, 0x1000));

  auto result = alloc.try_reserve(0x1000, 0x1000);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), error::invalid_state);
}

TEST(EarlyRegionAllocatorTest, ReserveFailsOnZeroSize) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1000, 0x1000));

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);

  auto result = made->try_reserve(0x1000, 0);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), error::invalid_argument);
}

TEST(EarlyRegionAllocatorTest, FreeMergesBackIntoAnAdjacentRegion) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1000, 0x1000));

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);
  auto &alloc = *made;
  ASSERT_TRUE(alloc.try_reserve(0x1000, 0x1000));
  ASSERT_TRUE(alloc.empty());

  ASSERT_TRUE(alloc.free(0x1000, 0x1000));
  ASSERT_FALSE(alloc.empty());
  ASSERT_EQ(alloc.regions().size(), 1u);
  EXPECT_EQ(alloc.regions()[0].base, 0x1000u);
  EXPECT_EQ(alloc.regions()[0].size, 0x1000u);
}

TEST(EarlyRegionAllocatorTest, ReportsEmptyFreeBytesAndLargestRegionPassthroughs) {
  region_set<4> seed;
  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);
  auto &alloc = *made;
  EXPECT_TRUE(alloc.empty());
  EXPECT_EQ(alloc.free_bytes(), 0u);
  EXPECT_EQ(alloc.largest_region().size, 0u);

  ASSERT_TRUE(seed.try_add(0x1000, 0x1000));
  ASSERT_TRUE(seed.try_add(0x10000, 0x4000));

  // The allocator was already created before these `try_add` calls, so
  // its own snapshot still reflects the empty seed it started from.
  EXPECT_TRUE(alloc.empty());

  auto made_after = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made_after);
  EXPECT_FALSE(made_after->empty());
  EXPECT_EQ(made_after->free_bytes(), 0x5000u);
  EXPECT_EQ(made_after->largest_region().base, 0x10000u);
  EXPECT_EQ(made_after->largest_region().size, 0x4000u);
}

TEST(EarlyRegionAllocatorTest, TryCreateNeverMutatesTheSeedRegionSet) {
  region_set<4> seed;
  ASSERT_TRUE(seed.try_add(0x1000, 0x1000));

  auto made = early_region_allocator<4>::try_create(seed);
  ASSERT_TRUE(made);
  ASSERT_TRUE(made->try_alloc(0x100));

  // The caller's own `seed` is completely unaffected by the allocation.
  ASSERT_EQ(seed.size(), 1u);
  EXPECT_EQ(seed[0].base, 0x1000u);
  EXPECT_EQ(seed[0].size, 0x1000u);
}
