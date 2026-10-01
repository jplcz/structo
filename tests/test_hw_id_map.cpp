// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/hw_id_map.hpp>

#include <cstddef>
#include <cstdint>

using structo::arch::hw_id_lut;

namespace {

/** @brief Fixture for `hw_id_lut` tests; each test constructs its own locally-scoped `lut` with the template parameters it needs. */
class HwIdMapTest : public ::testing::Test {};

} // namespace

TEST_F(HwIdMapTest, StartsEmptyAndFindReturnsInvalidIndex) {
  hw_id_lut<uint32_t, 4, 8> lut;
  EXPECT_EQ(lut.size(), 0u);
  EXPECT_EQ(lut.find(0x1234), lut.invalid_index);

  std::size_t out = 123;
  EXPECT_FALSE(lut.lookup(0x1234, out));
  EXPECT_EQ(out, 123u); // lookup leaves out_idx untouched on failure
}

TEST_F(HwIdMapTest, InsertThenFindRoundTripsThroughL1FastPath) {
  hw_id_lut<uint32_t, 4, 8> lut;

  ASSERT_TRUE(lut.insert(0x100, 0));
  ASSERT_TRUE(lut.insert(0x200, 1));
  ASSERT_TRUE(lut.insert(0x300, 2));

  EXPECT_EQ(lut.size(), 3u);
  EXPECT_EQ(lut.find(0x100), 0u);
  EXPECT_EQ(lut.find(0x200), 1u);
  EXPECT_EQ(lut.find(0x300), 2u);
  EXPECT_EQ(lut.find(0x400), lut.invalid_index);

  std::size_t out = 0;
  ASSERT_TRUE(lut.lookup(0x300, out));
  EXPECT_EQ(out, 2u);
}

TEST_F(HwIdMapTest, FallsBackToL2BinarySearchOnHashCollision) {
  // L1Size = 1 forces every hw_id into the same slot, so the second insert
  // always demotes that slot to a tombstone and every lookup must resolve
  // through the sorted L2 array's binary search instead of the L1 filter.
  hw_id_lut<uint32_t, 4, 1> lut;

  ASSERT_TRUE(lut.insert(0x10, 0));
  ASSERT_TRUE(lut.insert(0x20, 1));
  ASSERT_TRUE(lut.insert(0x05, 2));

  EXPECT_EQ(lut.find(0x10), 0u);
  EXPECT_EQ(lut.find(0x20), 1u);
  EXPECT_EQ(lut.find(0x05), 2u);
  EXPECT_EQ(lut.find(0x99), lut.invalid_index);
}

TEST_F(HwIdMapTest, ReinsertingAnExistingHwIdUpdatesItsCpuIndexInPlace) {
  hw_id_lut<uint32_t, 4, 8> lut;

  ASSERT_TRUE(lut.insert(0x42, 0));
  EXPECT_EQ(lut.size(), 1u);
  EXPECT_EQ(lut.find(0x42), 0u);

  ASSERT_TRUE(lut.insert(0x42, 3));
  EXPECT_EQ(lut.size(), 1u); // re-inserting the same key does not grow L2
  EXPECT_EQ(lut.find(0x42), 3u);
}

TEST_F(HwIdMapTest, InsertRejectsOutOfRangeOrTombstoneCpuIndexAndFullTable) {
  hw_id_lut<uint32_t, 2, 8> lut;

  EXPECT_FALSE(lut.insert(0x1, 2));             // cpu_idx >= max_cpus
  EXPECT_FALSE(lut.insert(0x1, lut.tombstone));  // reserved sentinel value

  ASSERT_TRUE(lut.insert(0x1, 0));
  ASSERT_TRUE(lut.insert(0x2, 1));
  EXPECT_FALSE(lut.insert(0x3, 0)); // table already holds max_cpus entries
  EXPECT_EQ(lut.size(), 2u);
}

TEST_F(HwIdMapTest, ClearResetsSizeAndEverySlotBackToEmpty) {
  hw_id_lut<uint32_t, 4, 8> lut;

  ASSERT_TRUE(lut.insert(0x10, 0));
  ASSERT_TRUE(lut.insert(0x20, 1));
  ASSERT_EQ(lut.size(), 2u);

  lut.clear();

  EXPECT_EQ(lut.size(), 0u);
  EXPECT_EQ(lut.find(0x10), lut.invalid_index);
  EXPECT_EQ(lut.find(0x20), lut.invalid_index);

  // The table is fully reusable after clear().
  ASSERT_TRUE(lut.insert(0x30, 2));
  EXPECT_EQ(lut.find(0x30), 2u);
}

TEST_F(HwIdMapTest, SupportsWideHwIdTypesViaTheSixtyFourBitMixerPath) {
  hw_id_lut<uint64_t, 4, 8> lut;

  ASSERT_TRUE(lut.insert(0x1'0000'0001ULL, 0));
  ASSERT_TRUE(lut.insert(0x2'0000'0002ULL, 1));

  EXPECT_EQ(lut.find(0x1'0000'0001ULL), 0u);
  EXPECT_EQ(lut.find(0x2'0000'0002ULL), 1u);
  EXPECT_EQ(lut.find(0x3'0000'0003ULL), lut.invalid_index);
}
