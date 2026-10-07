// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/virtio/virtq_types.hpp>

namespace {

using namespace structo::virtio;

class VirtqTypesTest : public ::testing::Test {};

TEST_F(VirtqTypesTest, FlagValuesMatchSpec) {
  EXPECT_EQ(desc_f_next, 1u);
  EXPECT_EQ(desc_f_write, 2u);
  EXPECT_EQ(desc_f_indirect, 4u);
  EXPECT_EQ(desc_f_avail, 0x80u);
  EXPECT_EQ(desc_f_used, 0x8000u);
}

TEST_F(VirtqTypesTest, FeatureBitHelper) {
  const std::uint64_t f = (std::uint64_t{1} << feature_version_1) | (std::uint64_t{1} << feature_ring_event_idx);
  EXPECT_TRUE(has_feature(f, feature_version_1));
  EXPECT_TRUE(has_feature(f, feature_ring_event_idx));
  EXPECT_FALSE(has_feature(f, feature_ring_packed));
  EXPECT_FALSE(has_feature(f, 64));
}

TEST_F(VirtqTypesTest, NeedEventBasic) {
  // Event idx 5: crossing from 4 to 6 (publishing entries 4 and 5) must notify.
  EXPECT_TRUE(need_event(5, 6, 4));
  // Not yet reached.
  EXPECT_FALSE(need_event(10, 6, 4));
  // Already passed before this batch.
  EXPECT_FALSE(need_event(2, 6, 4));
}

TEST_F(VirtqTypesTest, NeedEventWrapsModulo16Bits) {
  // old = 0xFFFE, new = 0x0001 (3 entries), event idx 0 sits inside the batch.
  EXPECT_TRUE(need_event(0x0000, 0x0001, 0xFFFE));
  EXPECT_TRUE(need_event(0xFFFF, 0x0001, 0xFFFE));
  EXPECT_FALSE(need_event(0x0005, 0x0001, 0xFFFE));
}

} // namespace
