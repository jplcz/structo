// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/virtio/split_ring.hpp>

namespace {

using namespace structo;
using namespace structo::virtio;

struct ring_space {};
struct buf_space {};

using ring_addr = phys_addr<void, ring_space>;
using buf_addr = phys_addr<void, buf_space>;
using ring_mem = direct_virtq_memory<ring_space>;
using buf_mem = direct_virtq_memory<buf_space>;
using driver_t = split_virtq_driver<ring_space, buf_space, ring_mem>;
using device_t = split_virtq_device<ring_space, buf_space, ring_mem>;
using sg_t = sg_entry<buf_space, std::uint64_t>;

constexpr std::uint32_t kQ = 8;
constexpr std::uint64_t kRingBase = 0x10000;
constexpr std::uint64_t kBufBase = 0x4000'0000;
constexpr std::uint64_t kTableOff = 1024;

template <typename R> auto get(R r) { return r.value(); }

class SplitRingFeatureTest : public ::testing::Test {
protected:
  void make(bool event_idx) {
    auto layout = try_split_layout(kQ, event_idx);
    ASSERT_TRUE(layout.has_value());
    auto a = split_ring_addrs<ring_space>::try_from_contiguous(ring_addr{kRingBase}, *layout);
    ASSERT_TRUE(a.has_value());
    addrs_ = *a;
    auto dr = driver_t::try_create(ring_mem_, addrs_, kQ, slots_.as_span(), event_idx);
    auto dv = device_t::try_create(ring_mem_, addrs_, kQ, event_idx);
    ASSERT_TRUE(dr.has_value() && dv.has_value());
    driver_.emplace(*dr);
    device_.emplace(*dv);
  }

  static reloco::span<const sg_t> view(const reloco::array<sg_t, 1> &a) { return {a.data(), a.size()}; }
  static reloco::span<const sg_t> view(const reloco::array<sg_t, 2> &a) { return {a.data(), a.size()}; }
  static reloco::span<const sg_t> view(const reloco::array<sg_t, 3> &a) { return {a.data(), a.size()}; }
  static reloco::span<const sg_t> none() { return {}; }
  static sg_t sg(std::uint64_t off, std::uint64_t len) { return sg_t{buf_addr{kBufBase + off}, len}; }

  // Submits one single-segment request and publishes it; returns the kick decision.
  bool submit_and_kick(std::uintptr_t token) {
    reloco::array<sg_t, 1> in{{sg(0, 4)}};
    EXPECT_TRUE(driver_->try_add(none(), view(in), token).has_value());
    EXPECT_TRUE(driver_->try_publish().has_value());
    return get(driver_->needs_notify());
  }

  bool complete_one_and_interrupt() {
    auto p = device_->try_pop(segs_.as_span());
    EXPECT_TRUE(p.has_value() && p->has_value());
    EXPECT_TRUE(device_->try_push_used(**p, 0).has_value());
    return get(device_->should_interrupt());
  }

  void pop_only() {
    auto p = device_->try_pop(segs_.as_span());
    ASSERT_TRUE(p.has_value() && p->has_value());
    ASSERT_TRUE(device_->try_push_used(**p, 0).has_value());
  }

  alignas(4096) reloco::array<std::byte, 4096> ring_bytes_{};
  alignas(16) reloco::array<std::byte, 4096> buf_bytes_{};
  ring_mem ring_mem_{ring_bytes_.data(), ring_bytes_.size(), ring_addr{kRingBase}};
  buf_mem buf_mem_{buf_bytes_.data(), buf_bytes_.size(), buf_addr{kBufBase}};
  split_ring_addrs<ring_space> addrs_;
  reloco::array<split_driver_slot, kQ> slots_{};
  reloco::array<chain_segment<buf_space>, 32> segs_{};
  reloco::optional<driver_t> driver_;
  reloco::optional<device_t> device_;
};

// ----------------------------------------------------------------------------
// EVENT_IDX
// ----------------------------------------------------------------------------

TEST_F(SplitRingFeatureTest, EventIdxSuppressesRedundantKicks) {
  make(true);
  EXPECT_TRUE(submit_and_kick(1));  // avail_event == 0: first request kicks
  EXPECT_FALSE(submit_and_kick(2)); // device has not asked for another yet

  pop_only();
  pop_only();
  ASSERT_TRUE(device_->try_set_notify_enabled(true).has_value()); // avail_event = 2
  EXPECT_TRUE(submit_and_kick(3));

  pop_only();
  ASSERT_TRUE(device_->try_set_notify_enabled(false).has_value()); // avail_event = 2: never again soon
  EXPECT_FALSE(submit_and_kick(4));
}

TEST_F(SplitRingFeatureTest, EventIdxCoalescesInterrupts) {
  make(true);
  for (std::uintptr_t t = 1; t <= 3; ++t)
    submit_and_kick(t);
  ASSERT_TRUE(driver_->try_set_used_event(2).has_value()); // interrupt once used.idx passes 2

  EXPECT_FALSE(complete_one_and_interrupt());
  EXPECT_FALSE(complete_one_and_interrupt());
  EXPECT_TRUE(complete_one_and_interrupt());
}

TEST_F(SplitRingFeatureTest, EventIdxInterruptToggle) {
  make(true);
  submit_and_kick(1);
  submit_and_kick(2);
  ASSERT_TRUE(driver_->try_set_interrupts_enabled(false).has_value());
  EXPECT_FALSE(complete_one_and_interrupt());
  ASSERT_TRUE(get(driver_->try_get_used()).has_value()); // reap, then re-arm at last_used
  ASSERT_TRUE(driver_->try_set_interrupts_enabled(true).has_value());
  EXPECT_TRUE(complete_one_and_interrupt());
}

TEST_F(SplitRingFeatureTest, EventIdxSurvivesIndexWrap) {
  make(true);
  for (std::uintptr_t i = 0; i < 140000; ++i) {
    ASSERT_TRUE(driver_->try_set_interrupts_enabled(true).has_value());
    ASSERT_TRUE(device_->try_set_notify_enabled(true).has_value());
    ASSERT_TRUE(submit_and_kick(i)) << i;
    ASSERT_TRUE(complete_one_and_interrupt()) << i;
    auto u = driver_->try_get_used();
    ASSERT_TRUE(u.has_value() && u->has_value()) << i;
  }
}

TEST_F(SplitRingFeatureTest, UsedEventRequiresEventIdx) {
  make(false);
  auto r = driver_->try_set_used_event(1);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_state);
}

// ----------------------------------------------------------------------------
// Indirect descriptors
// ----------------------------------------------------------------------------

class SplitRingIndirectTest : public SplitRingFeatureTest {
protected:
  void SetUp() override { make(false); }

  reloco::result<void> add_three(std::uintptr_t token = 7) {
    reloco::array<sg_t, 1> out{{sg(0, 8)}};
    reloco::array<sg_t, 2> in{{sg(64, 8), sg(128, 8)}};
    return driver_->try_add_indirect(view(out), view(in), token, buf_mem_, buf_addr{kBufBase + kTableOff},
                                     buf_addr{kBufBase + kTableOff}, 48);
  }

  void patch_table(std::uint32_t i, const virtq_desc &d) {
    ASSERT_TRUE(try_write_object(buf_mem_, buf_addr{kBufBase + kTableOff + i * 16u}, d).has_value());
  }
  virtq_desc read_table(std::uint32_t i) {
    return get(try_read_object<virtq_desc>(buf_mem_, buf_addr{kBufBase + kTableOff + i * 16u}));
  }
  void patch_main(const virtq_desc &d) {
    auto at = addrs_.desc_at(0, kQ);
    ASSERT_TRUE(at.has_value());
    ASSERT_TRUE(try_write_object(ring_mem_, *at, d).has_value());
  }
  virtq_desc read_main() {
    auto at = addrs_.desc_at(0, kQ);
    return get(try_read_object<virtq_desc>(ring_mem_, *at));
  }

  void expect_latched(reloco::error e) {
    auto p = device_->try_pop(segs_.as_span(), buf_mem_);
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error(), e);
    EXPECT_TRUE(device_->is_broken());
  }
};

TEST_F(SplitRingIndirectTest, RoundTripUsesOneRingDescriptor) {
  ASSERT_TRUE(add_three(0xBEEF).has_value());
  EXPECT_EQ(driver_->free_descriptors(), kQ - 1);
  ASSERT_TRUE(driver_->try_publish().has_value());

  auto p = device_->try_pop(segs_.as_span(), buf_mem_);
  ASSERT_TRUE(p.has_value() && p->has_value());
  const auto &c = **p;
  EXPECT_EQ(c.readable.size(), 1u);
  EXPECT_EQ(c.writable.size(), 2u);
  EXPECT_EQ(c.readable_bytes, 8u);
  EXPECT_EQ(c.writable_bytes, 16u);
  ASSERT_TRUE(device_->try_push_used(c, 16).has_value());

  auto u = driver_->try_get_used();
  ASSERT_TRUE(u.has_value() && u->has_value());
  EXPECT_EQ((*u)->token, 0xBEEFu);
  EXPECT_EQ((*u)->len, 16u);
  EXPECT_EQ(driver_->free_descriptors(), kQ);
}

TEST_F(SplitRingIndirectTest, ManySegmentsBeyondQueueSize) {
  // 20 segments > queue size 8: only possible with an indirect table.
  reloco::array<sg_t, 20> out{};
  const reloco::span<sg_t> out_w(out.data(), out.size());
  for (std::uint64_t i = 0; i < out_w.size(); ++i)
    out_w[i] = sg(i * 8, 8);
  const reloco::span<const sg_t> out_view = out_w;
  ASSERT_TRUE(driver_
                  ->try_add_indirect(out_view, none(), 1, buf_mem_, buf_addr{kBufBase + kTableOff},
                                     buf_addr{kBufBase + kTableOff}, 20 * 16)
                  .has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  auto p = device_->try_pop(segs_.as_span(), buf_mem_);
  ASSERT_TRUE(p.has_value() && p->has_value());
  EXPECT_EQ((*p)->readable.size(), 20u);
  EXPECT_EQ((*p)->readable_bytes, 160u);
}

TEST_F(SplitRingIndirectTest, DriverRejectsSmallTableAndEmpty) {
  reloco::array<sg_t, 2> in{{sg(0, 8), sg(64, 8)}};
  auto small = driver_->try_add_indirect(none(), view(in), 1, buf_mem_, buf_addr{kBufBase + kTableOff},
                                         buf_addr{kBufBase + kTableOff}, 16);
  ASSERT_FALSE(small.has_value());
  EXPECT_EQ(small.error(), reloco::error::invalid_argument);
  auto empty = driver_->try_add_indirect(none(), none(), 1, buf_mem_, buf_addr{kBufBase + kTableOff},
                                         buf_addr{kBufBase + kTableOff}, 64);
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(driver_->free_descriptors(), kQ);
}

TEST_F(SplitRingIndirectTest, ExhaustsRingDescriptors) {
  for (std::uintptr_t t = 0; t < kQ; ++t)
    ASSERT_TRUE(add_three(t).has_value());
  auto full = add_three(99);
  ASSERT_FALSE(full.has_value());
  EXPECT_EQ(full.error(), reloco::error::capacity_exceeded);
}

TEST_F(SplitRingIndirectTest, PlainPopRejectsIndirect) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  auto p = device_->try_pop(segs_.as_span());
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), reloco::error::security_violation);
}

TEST_F(SplitRingIndirectTest, HostileLengthNotMultipleOf16) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  virtq_desc m = read_main();
  m.len = 40;
  patch_main(m);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingIndirectTest, HostileZeroLength) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  virtq_desc m = read_main();
  m.len = 0;
  patch_main(m);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingIndirectTest, HostileIndirectWithNext) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  virtq_desc m = read_main();
  m.flags = static_cast<std::uint16_t>(m.flags | desc_f_next);
  patch_main(m);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingIndirectTest, HostileNestedIndirect) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  virtq_desc t = read_table(1);
  t.flags = static_cast<std::uint16_t>(t.flags | desc_f_indirect);
  patch_table(1, t);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingIndirectTest, HostileTableLoop) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  virtq_desc t = read_table(1);
  t.flags = static_cast<std::uint16_t>(t.flags | desc_f_next);
  t.next = 0;
  patch_table(1, t);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingIndirectTest, HostileTableNextOutOfRange) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  virtq_desc t = read_table(0);
  t.next = 3; // table holds 3 entries: valid indices 0..2
  patch_table(0, t);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingIndirectTest, HostileReadableAfterWritableInTable) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  virtq_desc t = read_table(2);
  t.flags = static_cast<std::uint16_t>(t.flags & ~desc_f_write);
  patch_table(2, t);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingIndirectTest, HostileTableOutsideMemory) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  virtq_desc m = read_main();
  m.addr = kBufBase + 0x10'0000;
  patch_main(m);
  auto p = device_->try_pop(segs_.as_span(), buf_mem_);
  ASSERT_FALSE(p.has_value());
  EXPECT_TRUE(device_->is_broken());
}

TEST_F(SplitRingIndirectTest, HostileTableTooLongForStorage) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  auto p = device_->try_pop(reloco::span<chain_segment<buf_space>>(segs_.data(), 2), buf_mem_);
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), reloco::error::capacity_exceeded);
}

TEST_F(SplitRingIndirectTest, DirectChainsStillWorkViaIndirectPop) {
  reloco::array<sg_t, 1> out{{sg(0, 4)}};
  reloco::array<sg_t, 1> in{{sg(8, 4)}};
  ASSERT_TRUE(driver_->try_add(view(out), view(in), 1).has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  auto p = device_->try_pop(segs_.as_span(), buf_mem_);
  ASSERT_TRUE(p.has_value() && p->has_value());
  EXPECT_EQ((*p)->readable.size(), 1u);
  EXPECT_EQ((*p)->writable.size(), 1u);
}

TEST_F(SplitRingIndirectTest, HostileDeviceLengthBeyondIndirectCapacity) {
  ASSERT_TRUE(add_three().has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  auto p = device_->try_pop(segs_.as_span(), buf_mem_);
  ASSERT_TRUE(p.has_value() && p->has_value());
  // Forge a used element claiming 17 bytes written into 16 writable bytes.
  auto elem_at = addrs_.used_elem_at(0, kQ);
  auto idx_at = addrs_.used_idx_addr();
  ASSERT_TRUE(elem_at.has_value() && idx_at.has_value());
  ASSERT_TRUE(try_write_object(ring_mem_, *elem_at, virtq_used_elem{0, 17}).has_value());
  using rt = virtq_memory_traits<ring_mem, ring_space>;
  ASSERT_TRUE(rt::try_store16(ring_mem_, *idx_at, 1).has_value());
  auto u = driver_->try_get_used();
  ASSERT_FALSE(u.has_value());
  EXPECT_EQ(u.error(), reloco::error::security_violation);
}

} // namespace
