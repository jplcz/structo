// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/virtio/virtq_layout.hpp>

namespace {

using namespace structo;
using namespace structo::virtio;

struct ring_space_a {};
struct ring_space_b {};

template <typename R> std::uint64_t val(R r) { return r.value().value; }

class VirtqLayoutTest : public ::testing::Test {};

TEST_F(VirtqLayoutTest, QueueSizeValidation) {
  EXPECT_FALSE(is_valid_split_queue_size(0));
  EXPECT_TRUE(is_valid_split_queue_size(1));
  EXPECT_TRUE(is_valid_split_queue_size(256));
  EXPECT_FALSE(is_valid_split_queue_size(100));
  EXPECT_TRUE(is_valid_split_queue_size(32768));
  EXPECT_FALSE(is_valid_split_queue_size(65536));

  EXPECT_TRUE(is_valid_packed_queue_size(100));
  EXPECT_FALSE(is_valid_packed_queue_size(0));
  EXPECT_FALSE(is_valid_packed_queue_size(32769));
}

TEST_F(VirtqLayoutTest, SplitSizesMatchSpec) {
  EXPECT_EQ(split_desc_bytes(256), 4096u);
  EXPECT_EQ(split_avail_bytes(256, false), 4u + 512u);
  EXPECT_EQ(split_avail_bytes(256, true), 4u + 512u + 2u);
  EXPECT_EQ(split_used_bytes(256, false), 4u + 2048u);
  EXPECT_EQ(split_used_bytes(256, true), 4u + 2048u + 2u);
}

TEST_F(VirtqLayoutTest, SplitContiguousLayout) {
  auto l = try_split_layout(256, false);
  ASSERT_TRUE(l.has_value());
  EXPECT_EQ(l->desc_offset, 0u);
  EXPECT_EQ(l->avail_offset, 4096u);
  EXPECT_EQ(l->used_offset, 4096u + 516u); // 4612, already 4-aligned
  EXPECT_EQ(l->total_size, 4612u + 2052u);

  auto e = try_split_layout(256, true);
  ASSERT_TRUE(e.has_value());
  EXPECT_EQ(e->avail_offset, 4096u);
  EXPECT_EQ(e->used_offset, 4096u + 520u); // 4616: 4098+... rounded up to 4
  EXPECT_EQ(e->used_offset % split_used_align, 0u);
}

TEST_F(VirtqLayoutTest, SplitRejectsBadQueueSize) {
  auto l = try_split_layout(100, false);
  ASSERT_FALSE(l.has_value());
  EXPECT_EQ(l.error(), reloco::error::invalid_argument);
}

TEST_F(VirtqLayoutTest, PackedLayout) {
  auto l = try_packed_layout(100);
  ASSERT_TRUE(l.has_value());
  EXPECT_EQ(l->desc_size, 1600u);
  EXPECT_EQ(l->driver_event_offset, 1600u);
  EXPECT_EQ(l->device_event_offset, 1604u);
  EXPECT_EQ(l->total_size, 1608u);

  EXPECT_FALSE(try_packed_layout(0).has_value());
}

TEST_F(VirtqLayoutTest, CheckedIndex) {
  auto ok = try_checked_index(7, 8);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(*ok, 7u);

  auto bad = try_checked_index(8, 8);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), reloco::error::security_violation);
  EXPECT_FALSE(try_checked_index(0xFFFFFFFFu, 8).has_value());
}

TEST_F(VirtqLayoutTest, SplitAddrsFromContiguousAndValidate) {
  auto l = try_split_layout(8, true);
  ASSERT_TRUE(l.has_value());
  auto a = split_ring_addrs<ring_space_a>::try_from_contiguous(phys_addr<void, ring_space_a>{0x10000}, *l);
  ASSERT_TRUE(a.has_value());
  EXPECT_EQ(a->desc.value, 0x10000u);
  EXPECT_EQ(a->avail.value, 0x10000u + l->avail_offset);
  EXPECT_EQ(a->used.value, 0x10000u + l->used_offset);
  EXPECT_TRUE(a->try_validate(*l).has_value());
}

TEST_F(VirtqLayoutTest, ValidateRejectsMisalignedAndNull) {
  auto l = try_split_layout(8, false);
  ASSERT_TRUE(l.has_value());
  split_ring_addrs<ring_space_a> a{phys_addr<void, ring_space_a>{0x10008}, // not 16-aligned
                                   phys_addr<void, ring_space_a>{0x20000}, phys_addr<void, ring_space_a>{0x30000}};
  EXPECT_FALSE(a.try_validate(*l).has_value());

  a.desc = phys_addr<void, ring_space_a>{0x10000};
  EXPECT_TRUE(a.try_validate(*l).has_value());

  a.used = phys_addr<void, ring_space_a>{};
  EXPECT_FALSE(a.try_validate(*l).has_value());
}

TEST_F(VirtqLayoutTest, ValidateRejectsAddressSpaceWrap) {
  auto l = try_split_layout(8, false);
  ASSERT_TRUE(l.has_value());
  split_ring_addrs<ring_space_a> a{phys_addr<void, ring_space_a>{0xFFFFFFFFFFFFFFF0ull},
                                   phys_addr<void, ring_space_a>{0x20000}, phys_addr<void, ring_space_a>{0x30000}};
  auto r = a.try_validate(*l);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::integer_overflow);
}

TEST_F(VirtqLayoutTest, SplitElementAddresses) {
  split_ring_addrs<ring_space_a> a{phys_addr<void, ring_space_a>{0x1000}, phys_addr<void, ring_space_a>{0x2000},
                                   phys_addr<void, ring_space_a>{0x3000}};
  EXPECT_EQ(val(a.desc_at(3, 8)), 0x1000u + 3 * 16);
  EXPECT_EQ(val(a.avail_ring_at(3, 8)), 0x2000u + 4 + 3 * 2);
  EXPECT_EQ(val(a.used_elem_at(3, 8)), 0x3000u + 4 + 3 * 8);
  EXPECT_EQ(val(a.avail_idx_addr()), 0x2002u);
  EXPECT_EQ(val(a.used_idx_addr()), 0x3002u);
  EXPECT_EQ(val(a.used_event_addr(8)), 0x2000u + 4 + 16);
  EXPECT_EQ(val(a.avail_event_addr(8)), 0x3000u + 4 + 64);

  auto bad = a.desc_at(8, 8);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), reloco::error::security_violation);
  EXPECT_FALSE(a.avail_ring_at(0x8000, 8).has_value());
  EXPECT_FALSE(a.used_elem_at(8, 8).has_value());
}

TEST_F(VirtqLayoutTest, PackedAddrs) {
  auto l = try_packed_layout(16);
  ASSERT_TRUE(l.has_value());
  auto a = packed_ring_addrs<ring_space_b>::try_from_contiguous(phys_addr<void, ring_space_b>{0x4000}, *l);
  ASSERT_TRUE(a.has_value());
  EXPECT_TRUE(a->try_validate(*l).has_value());
  EXPECT_EQ(val(a->desc_at(15, 16)), 0x4000u + 15 * 16);
  EXPECT_FALSE(a->desc_at(16, 16).has_value());
}

// Addresses in different spaces are distinct, non-interchangeable types.
static_assert(!std::is_convertible_v<phys_addr<void, ring_space_a>, phys_addr<void, ring_space_b>>);
static_assert(!std::is_same_v<split_ring_addrs<ring_space_a>, split_ring_addrs<ring_space_b>>);

} // namespace
