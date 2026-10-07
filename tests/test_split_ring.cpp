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
using ring_traits = virtq_memory_traits<ring_mem, ring_space>;

constexpr std::uint32_t kQ = 8;
constexpr std::uint64_t kRingBase = 0x10000;
constexpr std::uint64_t kBufBase = 0x4000'0000;

template <typename R> auto get(R r) { return r.value(); }

class SplitRingTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto layout = try_split_layout(kQ, false);
    ASSERT_TRUE(layout.has_value());
    auto a = split_ring_addrs<ring_space>::try_from_contiguous(ring_addr{kRingBase}, *layout);
    ASSERT_TRUE(a.has_value());
    addrs_ = *a;
  }

  reloco::result<driver_t> make_driver() { return driver_t::try_create(ring_mem_, addrs_, kQ, slots_.as_span()); }
  reloco::result<device_t> make_device() { return device_t::try_create(ring_mem_, addrs_, kQ); }

  static reloco::span<const sg_t> view(const reloco::array<sg_t, 1> &a) { return {a.data(), a.size()}; }
  static reloco::span<const sg_t> view(const reloco::array<sg_t, 2> &a) { return {a.data(), a.size()}; }
  static reloco::span<const sg_t> none() { return {}; }

  static sg_t sg(std::uint64_t off, std::uint64_t len) { return sg_t{buf_addr{kBufBase + off}, len}; }

  void patch_desc(std::uint32_t id, const virtq_desc &d) {
    auto at = addrs_.desc_at(id, kQ);
    ASSERT_TRUE(at.has_value());
    ASSERT_TRUE(try_write_object(ring_mem_, *at, d).has_value());
  }
  virtq_desc read_desc(std::uint32_t id) {
    auto at = addrs_.desc_at(id, kQ);
    return get(try_read_object<virtq_desc>(ring_mem_, *at));
  }
  void store16(const reloco::result<ring_addr> &at, std::uint16_t v) {
    ASSERT_TRUE(at.has_value());
    ASSERT_TRUE(ring_traits::try_store16(ring_mem_, *at, v).has_value());
  }

  alignas(4096) reloco::array<std::byte, 4096> ring_bytes_{};
  alignas(16) reloco::array<std::byte, 4096> buf_bytes_{};
  ring_mem ring_mem_{ring_bytes_.data(), ring_bytes_.size(), ring_addr{kRingBase}};
  buf_mem buf_mem_{buf_bytes_.data(), buf_bytes_.size(), buf_addr{kBufBase}};
  split_ring_addrs<ring_space> addrs_;
  reloco::array<split_driver_slot, kQ> slots_{};
  reloco::array<chain_segment<buf_space>, kQ> segs_{};
};

TEST_F(SplitRingTest, CreateValidation) {
  EXPECT_FALSE(driver_t::try_create(ring_mem_, addrs_, 6, slots_.as_span()).has_value());
  EXPECT_FALSE(driver_t::try_create(ring_mem_, addrs_, kQ, reloco::span<split_driver_slot>(slots_.data(), 4)).has_value());
  EXPECT_FALSE(device_t::try_create(ring_mem_, addrs_, 0).has_value());

  split_ring_addrs<ring_space> bad = addrs_;
  bad.desc = ring_addr{kRingBase + 8}; // not 16-aligned
  EXPECT_FALSE(device_t::try_create(ring_mem_, bad, kQ).has_value());
}

TEST_F(SplitRingTest, EmptyQueueYieldsNothing) {
  auto dr = make_driver();
  auto dv = make_device();
  ASSERT_TRUE(dr.has_value() && dv.has_value());
  auto used = dr->try_get_used();
  ASSERT_TRUE(used.has_value());
  EXPECT_FALSE(used->has_value());
  auto popped = dv->try_pop(segs_.as_span());
  ASSERT_TRUE(popped.has_value());
  EXPECT_FALSE(popped->has_value());
}

TEST_F(SplitRingTest, LoopbackRoundTrip) {
  auto dr = make_driver();
  auto dv = make_device();
  ASSERT_TRUE(dr.has_value() && dv.has_value());

  const char request[] = "ping-request";
  ASSERT_TRUE(try_write_object(buf_mem_, buf_addr{kBufBase}, request).has_value());

  reloco::array<sg_t, 1> out{{sg(0, sizeof(request))}};
  reloco::array<sg_t, 2> in{{sg(256, 8), sg(512, 8)}};
  ASSERT_TRUE(dr->try_add(view(out), view(in), 0xC0FFEE).has_value());
  EXPECT_EQ(dr->free_descriptors(), kQ - 3);

  // Nothing is visible until published.
  auto early = dv->try_pop(segs_.as_span());
  ASSERT_TRUE(early.has_value());
  EXPECT_FALSE(early->has_value());

  ASSERT_TRUE(dr->try_publish().has_value());
  auto notify = dr->needs_notify();
  ASSERT_TRUE(notify.has_value());
  EXPECT_TRUE(*notify);

  auto popped = dv->try_pop(segs_.as_span());
  ASSERT_TRUE(popped.has_value() && popped->has_value());
  const auto &chain = **popped;
  EXPECT_EQ(chain.readable.size(), 1u);
  EXPECT_EQ(chain.writable.size(), 2u);
  EXPECT_EQ(chain.readable_bytes, sizeof(request));
  EXPECT_EQ(chain.writable_bytes, 16u);

  char got[sizeof(request)] = {};
  ASSERT_TRUE(try_read_chain(buf_mem_, chain.readable, 0,
                             reloco::span<std::byte>(reinterpret_cast<std::byte *>(got), sizeof(got)))
                  .has_value());
  EXPECT_STREQ(got, request);

  // The response spans both writable segments (8 + 8 bytes).
  const reloco::array<char, 16> response{{'0','1','2','3','4','5','6','7','8','9','a','b','c','d','e','\0'}}; // 16 bytes with NUL
  ASSERT_TRUE(try_write_chain(buf_mem_, chain.writable, 0,
                              reloco::span<const std::byte>(reinterpret_cast<const std::byte *>(response.data()), 16))
                  .has_value());
  ASSERT_TRUE(dv->try_push_used(chain, 16).has_value());
  auto irq = dv->should_interrupt();
  ASSERT_TRUE(irq.has_value());
  EXPECT_TRUE(*irq);

  auto used = dr->try_get_used();
  ASSERT_TRUE(used.has_value() && used->has_value());
  EXPECT_EQ((*used)->token, 0xC0FFEEu);
  EXPECT_EQ((*used)->len, 16u);
  EXPECT_EQ(dr->free_descriptors(), kQ);

  reloco::array<std::byte, 16> back{};
  const reloco::span<std::byte> back_span(back.data(), back.size());
  using buf_traits = virtq_memory_traits<buf_mem, buf_space>;
  ASSERT_TRUE(buf_traits::try_read(buf_mem_, buf_addr{kBufBase + 256},
                                   back_span.first(8))
                  .has_value());
  ASSERT_TRUE(buf_traits::try_read(buf_mem_, buf_addr{kBufBase + 512},
                                   back_span.subspan(8, 8))
                  .has_value());
  std::size_t idx = 0;
  for (const char expected : response) {
    const auto one = back_span.subspan(idx, 1);
    EXPECT_EQ(static_cast<char>(one.front()), expected) << idx;
    ++idx;
  }
}

TEST_F(SplitRingTest, BatchPublishPreservesOrder) {
  auto dr = make_driver();
  auto dv = make_device();
  ASSERT_TRUE(dr.has_value() && dv.has_value());
  reloco::array<sg_t, 1> in{{sg(0, 4)}};
  for (std::uintptr_t t = 1; t <= 3; ++t)
    ASSERT_TRUE(dr->try_add(none(), view(in), t).has_value());
  ASSERT_TRUE(dr->try_publish().has_value());

  for (int i = 0; i < 3; ++i) {
    auto p = dv->try_pop(segs_.as_span());
    ASSERT_TRUE(p.has_value() && p->has_value());
    EXPECT_EQ((*p)->head, i);
    ASSERT_TRUE(dv->try_push_used(**p, 4).has_value());
  }
  for (std::uintptr_t t = 1; t <= 3; ++t) {
    auto u = dr->try_get_used();
    ASSERT_TRUE(u.has_value() && u->has_value());
    EXPECT_EQ((*u)->token, t);
  }
}

TEST_F(SplitRingTest, ExhaustionAndRecovery) {
  auto dr = make_driver();
  auto dv = make_device();
  ASSERT_TRUE(dr.has_value() && dv.has_value());
  reloco::array<sg_t, 2> two{{sg(0, 4), sg(8, 4)}};
  reloco::array<sg_t, 1> one{{sg(0, 4)}};
  for (int i = 0; i < 4; ++i)
    ASSERT_TRUE(dr->try_add(view(one), view(one), 1).has_value());
  EXPECT_EQ(dr->free_descriptors(), 0u);
  auto full = dr->try_add(view(one), none(), 2);
  ASSERT_FALSE(full.has_value());
  EXPECT_EQ(full.error(), reloco::error::capacity_exceeded);
  EXPECT_FALSE(dr->is_broken());

  ASSERT_TRUE(dr->try_publish().has_value());
  auto p = dv->try_pop(segs_.as_span());
  ASSERT_TRUE(p.has_value() && p->has_value());
  ASSERT_TRUE(dv->try_push_used(**p, 0).has_value());
  ASSERT_TRUE(get(dr->try_get_used()).has_value());
  EXPECT_EQ(dr->free_descriptors(), 2u);
  EXPECT_TRUE(dr->try_add(view(two), none(), 3).has_value());
  EXPECT_EQ(dr->free_descriptors(), 0u);
}

TEST_F(SplitRingTest, RejectsEmptyAndOversizedRequests) {
  auto dr = make_driver();
  ASSERT_TRUE(dr.has_value());
  auto empty = dr->try_add(none(), none(), 1);
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error(), reloco::error::invalid_argument);

  reloco::array<sg_t, 1> huge{{sg(0, 0x1'0000'0000ull)}};
  EXPECT_FALSE(dr->try_add(view(huge), none(), 1).has_value());
  EXPECT_EQ(dr->free_descriptors(), kQ);
}

TEST_F(SplitRingTest, IndicesWrapAround) {
  auto dr = make_driver();
  auto dv = make_device();
  ASSERT_TRUE(dr.has_value() && dv.has_value());
  reloco::array<sg_t, 1> out{{sg(0, 8)}};
  reloco::array<sg_t, 1> in{{sg(64, 8)}};

  // > 2 * 65536 chain submissions: avail.idx/used.idx wrap several times.
  for (std::uintptr_t i = 0; i < 140000; ++i) {
    ASSERT_TRUE(dr->try_add(view(out), view(in), i).has_value());
    ASSERT_TRUE(dr->try_publish().has_value());
    auto p = dv->try_pop(segs_.as_span());
    ASSERT_TRUE(p.has_value() && p->has_value()) << i;
    ASSERT_TRUE(dv->try_push_used(**p, 8).has_value());
    auto u = dr->try_get_used();
    ASSERT_TRUE(u.has_value() && u->has_value()) << i;
    ASSERT_EQ((*u)->token, i);
  }
  EXPECT_EQ(dr->free_descriptors(), kQ);
}

TEST_F(SplitRingTest, NotificationSuppressionFlags) {
  auto dr = make_driver();
  auto dv = make_device();
  ASSERT_TRUE(dr.has_value() && dv.has_value());

  ASSERT_TRUE(dv->try_set_notify_enabled(false).has_value());
  EXPECT_FALSE(get(dr->needs_notify()));
  ASSERT_TRUE(dv->try_set_notify_enabled(true).has_value());
  EXPECT_TRUE(get(dr->needs_notify()));

  ASSERT_TRUE(dr->try_set_interrupts_enabled(false).has_value());
  EXPECT_FALSE(get(dv->should_interrupt()));
  ASSERT_TRUE(dr->try_set_interrupts_enabled(true).has_value());
  EXPECT_TRUE(get(dv->should_interrupt()));
}

TEST_F(SplitRingTest, ChainHelpersCrossSegmentBoundaries) {
  chain_segment<buf_space> segs[2] = {{buf_addr{kBufBase + 0}, 4}, {buf_addr{kBufBase + 100}, 4}};
  reloco::span<const chain_segment<buf_space>> view_segs(segs, 2);

  const char data[] = "ABCDEFG";
  ASSERT_TRUE(try_write_chain(buf_mem_, view_segs, 1, reloco::span<const std::byte>(
                                                          reinterpret_cast<const std::byte *>(data), 6))
                  .has_value());
  char out[7] = {};
  ASSERT_TRUE(try_read_chain(buf_mem_, view_segs, 1, reloco::span<std::byte>(reinterpret_cast<std::byte *>(out), 6))
                  .has_value());
  EXPECT_STREQ(out, "ABCDEF");

  // Beyond the chain's 8 bytes.
  auto r = try_read_chain(buf_mem_, view_segs, 4, reloco::span<std::byte>(reinterpret_cast<std::byte *>(out), 5));
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::out_of_range);
}

// ----------------------------------------------------------------------------
// Hostile guest (device side): the driver end of the ring is corrupted by hand.
// ----------------------------------------------------------------------------

class SplitRingHostileGuestTest : public SplitRingTest {
protected:
  void SetUp() override {
    SplitRingTest::SetUp();
    auto dr = make_driver();
    auto dv = make_device();
    ASSERT_TRUE(dr.has_value() && dv.has_value());
    driver_.emplace(*dr);
    device_.emplace(*dv);
  }

  void submit_two_segment_chain() {
    reloco::array<sg_t, 1> out{{sg(0, 8)}};
    reloco::array<sg_t, 1> in{{sg(64, 8)}};
    ASSERT_TRUE(driver_->try_add(view(out), view(in), 1).has_value());
    ASSERT_TRUE(driver_->try_publish().has_value());
  }

  void expect_latched(reloco::error expected) {
    auto p = device_->try_pop(segs_.as_span());
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error(), expected);
    EXPECT_TRUE(device_->is_broken());
    auto again = device_->try_pop(segs_.as_span());
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), reloco::error::invalid_state);
  }

  reloco::optional<driver_t> driver_;
  reloco::optional<device_t> device_;
};

TEST_F(SplitRingHostileGuestTest, NextIndexOutOfRange) {
  submit_two_segment_chain();
  virtq_desc d = read_desc(0);
  d.next = 4000;
  patch_desc(0, d);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingHostileGuestTest, DescriptorLoop) {
  submit_two_segment_chain();
  virtq_desc d = read_desc(1);
  d.flags = static_cast<std::uint16_t>(d.flags | desc_f_next);
  d.next = 0; // 0 -> 1 -> 0 -> ...
  patch_desc(1, d);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingHostileGuestTest, SelfLoop) {
  submit_two_segment_chain();
  virtq_desc d = read_desc(0);
  d.next = 0;
  patch_desc(0, d);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingHostileGuestTest, HeadIndexOutOfRange) {
  submit_two_segment_chain();
  store16(addrs_.avail_ring_at(0, kQ), 0xFFFF);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingHostileGuestTest, ReadableAfterWritable) {
  submit_two_segment_chain();
  virtq_desc d0 = read_desc(0);
  d0.flags = static_cast<std::uint16_t>(d0.flags | desc_f_write);
  patch_desc(0, d0);
  virtq_desc d1 = read_desc(1);
  d1.flags = static_cast<std::uint16_t>(d1.flags & ~desc_f_write);
  patch_desc(1, d1);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingHostileGuestTest, IndirectNotNegotiated) {
  submit_two_segment_chain();
  virtq_desc d = read_desc(0);
  d.flags = static_cast<std::uint16_t>(d.flags | desc_f_indirect);
  patch_desc(0, d);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingHostileGuestTest, AvailIndexJumpsPastQueueSize) {
  store16(addrs_.avail_idx_addr(), 100);
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingHostileGuestTest, AvailIndexGoingBackwardsLooksLikeHugeBacklog) {
  submit_two_segment_chain();
  ASSERT_TRUE(get(device_->try_pop(segs_.as_span())).has_value());
  store16(addrs_.avail_idx_addr(), 0); // 0 - 1 == 65535 pending
  expect_latched(reloco::error::security_violation);
}

TEST_F(SplitRingHostileGuestTest, ChainLongerThanDeviceStorage) {
  reloco::array<sg_t, 2> two{{sg(0, 4), sg(8, 4)}};
  ASSERT_TRUE(driver_->try_add(view(two), none(), 1).has_value());
  ASSERT_TRUE(driver_->try_publish().has_value());
  auto p = device_->try_pop(reloco::span<chain_segment<buf_space>>(segs_.data(), 1));
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), reloco::error::capacity_exceeded);
  EXPECT_TRUE(device_->is_broken());
}

TEST_F(SplitRingHostileGuestTest, CompletingMoreThanWritableIsADeviceBugNotLatched) {
  submit_two_segment_chain();
  auto p = device_->try_pop(segs_.as_span());
  ASSERT_TRUE(p.has_value() && p->has_value());
  auto r = device_->try_push_used(**p, 9); // only 8 writable bytes
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);
  EXPECT_FALSE(device_->is_broken());
  EXPECT_TRUE(device_->try_push_used(**p, 8).has_value());
}

TEST_F(SplitRingHostileGuestTest, RingMemoryUnmappedLatches) {
  // Descriptor table claims an address range the memory backend cannot reach.
  ring_mem tiny(ring_bytes_.data(), 8, ring_addr{kRingBase});
  auto dv = device_t::try_create(tiny, addrs_, kQ);
  EXPECT_FALSE(dv.has_value());
}

// ----------------------------------------------------------------------------
// Hostile device (driver side): the used ring is corrupted by hand.
// ----------------------------------------------------------------------------

class SplitRingHostileDeviceTest : public SplitRingHostileGuestTest {
protected:
  void expect_driver_latched() {
    auto u = driver_->try_get_used();
    ASSERT_FALSE(u.has_value());
    EXPECT_EQ(u.error(), reloco::error::security_violation);
    EXPECT_TRUE(driver_->is_broken());
    auto again = driver_->try_get_used();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), reloco::error::invalid_state);
    EXPECT_FALSE(driver_->try_publish().has_value());
  }

  void post_used(std::uint32_t id, std::uint32_t len, std::uint16_t new_idx) {
    auto at = addrs_.used_elem_at(0, kQ);
    ASSERT_TRUE(at.has_value());
    ASSERT_TRUE(try_write_object(ring_mem_, *at, virtq_used_elem{id, len}).has_value());
    store16(addrs_.used_idx_addr(), new_idx);
  }
};

TEST_F(SplitRingHostileDeviceTest, CompletionWithNothingOutstanding) {
  post_used(0, 0, 1);
  expect_driver_latched();
}

TEST_F(SplitRingHostileDeviceTest, MoreCompletionsThanOutstanding) {
  submit_two_segment_chain();
  post_used(0, 0, 2);
  expect_driver_latched();
}

TEST_F(SplitRingHostileDeviceTest, IdOutOfRange) {
  submit_two_segment_chain();
  post_used(0x10000, 0, 1);
  expect_driver_latched();
}

TEST_F(SplitRingHostileDeviceTest, IdNotAChainHead) {
  submit_two_segment_chain();
  post_used(5, 0, 1); // id 5 was never submitted
  expect_driver_latched();
}

TEST_F(SplitRingHostileDeviceTest, LengthBeyondWritableCapacity) {
  submit_two_segment_chain();
  post_used(0, 9, 1); // only 8 writable bytes
  expect_driver_latched();
}

TEST_F(SplitRingHostileDeviceTest, WellBehavedCompletionStillWorks) {
  submit_two_segment_chain();
  post_used(0, 8, 1);
  auto u = driver_->try_get_used();
  ASSERT_TRUE(u.has_value() && u->has_value());
  EXPECT_EQ((*u)->len, 8u);
  EXPECT_FALSE(driver_->is_broken());
}

} // namespace
