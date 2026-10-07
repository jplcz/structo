// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/virtio/packed_ring.hpp>

namespace {

using namespace structo;
using namespace structo::virtio;

struct ring_space {};
struct buf_space {};

using ring_addr = phys_addr<void, ring_space>;
using buf_addr = phys_addr<void, buf_space>;
using ring_mem = direct_virtq_memory<ring_space>;
using buf_mem = direct_virtq_memory<buf_space>;
using driver_t = packed_virtq_driver<ring_space, buf_space, ring_mem>;
using device_t = packed_virtq_device<ring_space, buf_space, ring_mem>;
using sg_t = sg_entry<buf_space, std::uint64_t>;
using rt = virtq_memory_traits<ring_mem, ring_space>;

constexpr std::uint32_t kQ = 8;
constexpr std::uint64_t kRingBase = 0x10000;
constexpr std::uint64_t kBufBase = 0x4000'0000;

template <typename R> auto get(R r) { return r.value(); }

class PackedRingTest : public ::testing::Test {
protected:
  void make(bool event_idx = false) {
    auto layout = try_packed_layout(kQ);
    ASSERT_TRUE(layout.has_value());
    auto a = packed_ring_addrs<ring_space>::try_from_contiguous(ring_addr{kRingBase}, *layout);
    ASSERT_TRUE(a.has_value());
    addrs_ = *a;
    auto dr = driver_t::try_create(ring_mem_, addrs_, kQ, slots_.as_span(), event_idx);
    auto dv = device_t::try_create(ring_mem_, addrs_, kQ, event_idx);
    ASSERT_TRUE(dr.has_value() && dv.has_value());
    driver_.emplace(*dr);
    device_.emplace(*dv);
  }
  void SetUp() override { make(); }

  static sg_t sg(std::uint64_t off, std::uint64_t len) { return sg_t{buf_addr{kBufBase + off}, len}; }
  static reloco::span<const sg_t> none() { return {}; }

  // Builds `n` segments and submits the first `n_out` as device-readable.
  reloco::result<void> add(std::size_t n_out, std::size_t n_in, std::uintptr_t token) {
    reloco::array<sg_t, 16> all{};
    const reloco::span<sg_t> w(all.data(), all.size());
    for (std::size_t i = 0; i < n_out + n_in; ++i)
      w[i] = sg(i * 16, 8);
    const reloco::span<const sg_t> v = w;
    return driver_->try_add(v.subspan(0, n_out), v.subspan(n_out, n_in), token);
  }

  reloco::result<reloco::optional<avail_chain<buf_space>>> pop() { return device_->try_pop(segs_.as_span()); }

  ring_addr field(std::uint32_t pos, std::size_t off) {
    auto at = addrs_.desc_at(pos, kQ);
    auto f = at->try_add(off);
    return *f;
  }
  std::uint16_t flags_of(std::uint32_t pos) { return get(rt::try_load16(ring_mem_, field(pos, 14))); }
  void set_flags(std::uint32_t pos, std::uint16_t f) { ASSERT_TRUE(rt::try_store16(ring_mem_, field(pos, 14), f).has_value()); }

  alignas(4096) reloco::array<std::byte, 4096> ring_bytes_{};
  alignas(16) reloco::array<std::byte, 4096> buf_bytes_{};
  ring_mem ring_mem_{ring_bytes_.data(), ring_bytes_.size(), ring_addr{kRingBase}};
  buf_mem buf_mem_{buf_bytes_.data(), buf_bytes_.size(), buf_addr{kBufBase}};
  packed_ring_addrs<ring_space> addrs_;
  reloco::array<packed_driver_slot, kQ> slots_{};
  reloco::array<chain_segment<buf_space>, kQ> segs_{};
  reloco::optional<driver_t> driver_;
  reloco::optional<device_t> device_;
};

TEST_F(PackedRingTest, CreateValidation) {
  EXPECT_FALSE(driver_t::try_create(ring_mem_, addrs_, 0, slots_.as_span()).has_value());
  EXPECT_FALSE(driver_t::try_create(ring_mem_, addrs_, kQ, reloco::span<packed_driver_slot>(slots_.data(), 2)).has_value());
  packed_ring_addrs<ring_space> bad = addrs_;
  bad.desc = ring_addr{kRingBase + 8}; // not 16-aligned
  EXPECT_FALSE(device_t::try_create(ring_mem_, bad, kQ).has_value());
}

TEST_F(PackedRingTest, EmptyQueueYieldsNothing) {
  auto p = pop();
  ASSERT_TRUE(p.has_value());
  EXPECT_FALSE(p->has_value());
  auto u = driver_->try_get_used();
  ASSERT_TRUE(u.has_value());
  EXPECT_FALSE(u->has_value());
}

TEST_F(PackedRingTest, LoopbackRoundTrip) {
  ASSERT_TRUE(add(1, 2, 0xC0FFEE).has_value());
  EXPECT_EQ(driver_->free_descriptors(), kQ - 3);
  EXPECT_TRUE(get(driver_->needs_notify()));

  auto p = pop();
  ASSERT_TRUE(p.has_value() && p->has_value());
  const auto &c = **p;
  EXPECT_EQ(c.readable.size(), 1u);
  EXPECT_EQ(c.writable.size(), 2u);
  EXPECT_EQ(c.readable_bytes, 8u);
  EXPECT_EQ(c.writable_bytes, 16u);
  EXPECT_EQ(c.desc_count, 3u);

  auto none_yet = driver_->try_get_used();
  ASSERT_TRUE(none_yet.has_value());
  EXPECT_FALSE(none_yet->has_value());

  ASSERT_TRUE(device_->try_push_used(c, 12).has_value());
  EXPECT_TRUE(get(device_->should_interrupt()));
  auto u = driver_->try_get_used();
  ASSERT_TRUE(u.has_value() && u->has_value());
  EXPECT_EQ((*u)->token, 0xC0FFEEu);
  EXPECT_EQ((*u)->len, 12u);
  EXPECT_EQ(driver_->free_descriptors(), kQ);
}

TEST_F(PackedRingTest, DataTravelsThroughChain) {
  const char request[] = "ping";
  ASSERT_TRUE(try_write_object(buf_mem_, buf_addr{kBufBase}, request).has_value());
  ASSERT_TRUE(add(1, 1, 1).has_value());
  auto p = pop();
  ASSERT_TRUE(p.has_value() && p->has_value());
  char got[8] = {};
  ASSERT_TRUE(try_read_chain(buf_mem_, (*p)->readable, 0, reloco::span<std::byte>(reinterpret_cast<std::byte *>(got), 5))
                  .has_value());
  EXPECT_STREQ(got, "ping");
}

TEST_F(PackedRingTest, OutOfOrderCompletion) {
  ASSERT_TRUE(add(0, 3, 10).has_value()); // A: 3 descriptors
  ASSERT_TRUE(add(0, 1, 20).has_value()); // B: 1
  ASSERT_TRUE(add(1, 1, 30).has_value()); // C: 2
  EXPECT_EQ(driver_->free_descriptors(), kQ - 6);

  reloco::array<avail_chain<buf_space>, 3> chains{};
  reloco::array<chain_segment<buf_space>, 3 * kQ> store{};
  const reloco::span<chain_segment<buf_space>> st(store.data(), store.size());
  const reloco::span<avail_chain<buf_space>> cs(chains.data(), chains.size());
  for (std::size_t i = 0; i < 3; ++i) {
    auto p = device_->try_pop(st.subspan(i * kQ, kQ));
    ASSERT_TRUE(p.has_value() && p->has_value());
    cs[i] = **p;
  }
  for (std::size_t i : {2u, 0u, 1u})
    ASSERT_TRUE(device_->try_push_used(cs[i], 0).has_value());

  for (std::uintptr_t t : {30u, 10u, 20u}) {
    auto u = driver_->try_get_used();
    ASSERT_TRUE(u.has_value() && u->has_value());
    EXPECT_EQ((*u)->token, t);
  }
  EXPECT_EQ(driver_->free_descriptors(), kQ);
}

TEST_F(PackedRingTest, ExhaustionAndRecovery) {
  ASSERT_TRUE(add(1, 3, 1).has_value());
  ASSERT_TRUE(add(1, 2, 2).has_value());
  EXPECT_EQ(driver_->free_descriptors(), 1u);
  auto full = add(1, 1, 3);
  ASSERT_FALSE(full.has_value());
  EXPECT_EQ(full.error(), reloco::error::capacity_exceeded);
  EXPECT_FALSE(driver_->is_broken());

  auto p = pop();
  ASSERT_TRUE(p.has_value() && p->has_value());
  ASSERT_TRUE(device_->try_push_used(**p, 0).has_value());
  ASSERT_TRUE(get(driver_->try_get_used()).has_value());
  EXPECT_TRUE(add(1, 1, 3).has_value());
}

TEST_F(PackedRingTest, RejectsEmptyAndOversized) {
  auto e = driver_->try_add(none(), none(), 1);
  ASSERT_FALSE(e.has_value());
  EXPECT_EQ(e.error(), reloco::error::invalid_argument);
  reloco::array<sg_t, 1> huge{{sg(0, 0x1'0000'0000ull)}};
  EXPECT_FALSE(driver_->try_add(none(), reloco::span<const sg_t>(huge.data(), 1), 1).has_value());
  EXPECT_EQ(driver_->free_descriptors(), kQ);
}

TEST_F(PackedRingTest, WrapCountersAcrossManyLaps) {
  // Chain sizes 1..3 cycle so the ring position walks every alignment.
  for (std::uintptr_t i = 0; i < 100000; ++i) {
    const std::size_t n_in = 1 + i % 3;
    ASSERT_TRUE(add(i % 2, n_in, i).has_value()) << i;
    auto p = pop();
    ASSERT_TRUE(p.has_value() && p->has_value()) << i;
    ASSERT_TRUE(device_->try_push_used(**p, 8).has_value());
    auto u = driver_->try_get_used();
    ASSERT_TRUE(u.has_value() && u->has_value()) << i;
    ASSERT_EQ((*u)->token, i);
  }
  EXPECT_EQ(driver_->free_descriptors(), kQ);
}

TEST_F(PackedRingTest, BatchedAddsAcrossWrap) {
  for (int round = 0; round < 1000; ++round) {
    for (std::uintptr_t t = 0; t < 3; ++t)
      ASSERT_TRUE(add(1, 1, t).has_value());
    for (std::uintptr_t t = 0; t < 3; ++t) {
      auto p = pop();
      ASSERT_TRUE(p.has_value() && p->has_value());
      ASSERT_TRUE(device_->try_push_used(**p, 0).has_value());
    }
    for (std::uintptr_t t = 0; t < 3; ++t) {
      auto u = driver_->try_get_used();
      ASSERT_TRUE(u.has_value() && u->has_value());
      ASSERT_EQ((*u)->token, t);
    }
  }
}

TEST_F(PackedRingTest, SuppressionFlags) {
  ASSERT_TRUE(device_->try_set_notify_enabled(false).has_value());
  EXPECT_FALSE(get(driver_->needs_notify()));
  ASSERT_TRUE(device_->try_set_notify_enabled(true).has_value());
  EXPECT_TRUE(get(driver_->needs_notify()));
  ASSERT_TRUE(driver_->try_set_interrupts_enabled(false).has_value());
  EXPECT_FALSE(get(device_->should_interrupt()));
  ASSERT_TRUE(driver_->try_set_interrupts_enabled(true).has_value());
  EXPECT_TRUE(get(device_->should_interrupt()));
}

TEST_F(PackedRingTest, EventIdxRequiresNegotiation) {
  auto a = driver_->try_set_interrupt_after(2);
  ASSERT_FALSE(a.has_value());
  EXPECT_EQ(a.error(), reloco::error::invalid_state);
  auto b = device_->try_set_notify_after(2);
  ASSERT_FALSE(b.has_value());
  EXPECT_EQ(b.error(), reloco::error::invalid_state);
}

TEST_F(PackedRingTest, EventIdxCoalescesInterrupts) {
  make(true);
  for (std::uintptr_t t = 0; t < 4; ++t)
    ASSERT_TRUE(add(0, 1, t).has_value());
  ASSERT_TRUE(driver_->try_set_interrupt_after(2).has_value()); // after two more completions

  auto complete = [&]() {
    auto p = pop();
    EXPECT_TRUE(p.has_value() && p->has_value());
    EXPECT_TRUE(device_->try_push_used(**p, 0).has_value());
    return get(device_->should_interrupt());
  };
  EXPECT_FALSE(complete());
  EXPECT_FALSE(complete());
  EXPECT_TRUE(complete());
}

TEST_F(PackedRingTest, EventIdxCoalescesKicks) {
  make(true);
  ASSERT_TRUE(device_->try_set_notify_after(2).has_value()); // kick once 2 more descriptors are available
  ASSERT_TRUE(add(0, 1, 1).has_value());
  EXPECT_FALSE(get(driver_->needs_notify()));
  ASSERT_TRUE(add(0, 1, 2).has_value());
  EXPECT_FALSE(get(driver_->needs_notify()));
  ASSERT_TRUE(add(0, 1, 3).has_value());
  EXPECT_TRUE(get(driver_->needs_notify()));
}

TEST_F(PackedRingTest, EventIdxAcrossWrap) {
  make(true);
  for (std::uintptr_t i = 0; i < 20000; ++i) {
    ASSERT_TRUE(driver_->try_set_interrupt_after(0).has_value());
    ASSERT_TRUE(device_->try_set_notify_after(0).has_value());
    ASSERT_TRUE(add(0, 1 + i % 3, i).has_value());
    ASSERT_TRUE(get(driver_->needs_notify())) << i;
    auto p = pop();
    ASSERT_TRUE(p.has_value() && p->has_value());
    ASSERT_TRUE(device_->try_push_used(**p, 0).has_value());
    ASSERT_TRUE(get(device_->should_interrupt())) << i;
    ASSERT_TRUE(get(driver_->try_get_used()).has_value());
  }
}

// ----------------------------------------------------------------------------
// Hostile driver (device side)
// ----------------------------------------------------------------------------

class PackedHostileGuestTest : public PackedRingTest {
protected:
  void expect_latched(reloco::error e) {
    auto p = pop();
    ASSERT_FALSE(p.has_value());
    EXPECT_EQ(p.error(), e);
    EXPECT_TRUE(device_->is_broken());
    auto again = pop();
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error(), reloco::error::invalid_state);
  }
};

TEST_F(PackedHostileGuestTest, ChainNotFullyPublished) {
  ASSERT_TRUE(add(1, 1, 1).has_value());
  set_flags(1, static_cast<std::uint16_t>(flags_of(1) | desc_f_next)); // points at slot 2, never written
  expect_latched(reloco::error::security_violation);
}

TEST_F(PackedHostileGuestTest, ChainLongerThanRing) {
  ASSERT_TRUE(add(0, kQ, 1).has_value());
  set_flags(kQ - 1, static_cast<std::uint16_t>(flags_of(kQ - 1) | desc_f_next));
  expect_latched(reloco::error::security_violation);
}

TEST_F(PackedHostileGuestTest, IndirectRejected) {
  ASSERT_TRUE(add(0, 1, 1).has_value());
  set_flags(0, static_cast<std::uint16_t>(flags_of(0) | desc_f_indirect));
  expect_latched(reloco::error::security_violation);
}

TEST_F(PackedHostileGuestTest, ReadableAfterWritable) {
  ASSERT_TRUE(add(1, 1, 1).has_value());
  set_flags(0, static_cast<std::uint16_t>(flags_of(0) | desc_f_write));
  set_flags(1, static_cast<std::uint16_t>(flags_of(1) & ~desc_f_write));
  expect_latched(reloco::error::security_violation);
}

TEST_F(PackedHostileGuestTest, StorageTooSmall) {
  ASSERT_TRUE(add(0, 3, 1).has_value());
  auto p = device_->try_pop(reloco::span<chain_segment<buf_space>>(segs_.data(), 2));
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), reloco::error::capacity_exceeded);
  EXPECT_TRUE(device_->is_broken());
}

TEST_F(PackedHostileGuestTest, WrongWrapCounterIsNotAvailable) {
  ASSERT_TRUE(add(0, 1, 1).has_value());
  set_flags(0, desc_f_used); // owned by "previous lap": not available, not an error
  auto p = pop();
  ASSERT_TRUE(p.has_value());
  EXPECT_FALSE(p->has_value());
  EXPECT_FALSE(device_->is_broken());
}

TEST_F(PackedHostileGuestTest, OversizedPushUsedIsRejectedWithoutLatch) {
  ASSERT_TRUE(add(0, 1, 1).has_value());
  auto p = pop();
  ASSERT_TRUE(p.has_value() && p->has_value());
  auto r = device_->try_push_used(**p, 9);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);
  EXPECT_FALSE(device_->is_broken());
}

// ----------------------------------------------------------------------------
// Hostile device (driver side)
// ----------------------------------------------------------------------------

class PackedHostileDeviceTest : public PackedRingTest {
protected:
  // Forges a used descriptor at ring position 0, first lap.
  void forge_used(std::uint16_t id, std::uint32_t len, std::uint16_t flags = desc_f_avail | desc_f_used) {
    ASSERT_TRUE(try_write_object(ring_mem_, field(0, 8), len).has_value());
    ASSERT_TRUE(rt::try_store16(ring_mem_, field(0, 12), id).has_value());
    set_flags(0, flags);
  }
  void expect_latched() {
    auto u = driver_->try_get_used();
    ASSERT_FALSE(u.has_value());
    EXPECT_EQ(u.error(), reloco::error::security_violation);
    EXPECT_TRUE(driver_->is_broken());
    EXPECT_EQ(driver_->try_get_used().error(), reloco::error::invalid_state);
  }
};

TEST_F(PackedHostileDeviceTest, IdOutOfRange) {
  ASSERT_TRUE(add(1, 1, 1).has_value());
  forge_used(0x7000, 0);
  expect_latched();
}

TEST_F(PackedHostileDeviceTest, IdNotInFlight) {
  ASSERT_TRUE(add(1, 1, 1).has_value());
  forge_used(5, 0);
  expect_latched();
}

TEST_F(PackedHostileDeviceTest, LengthBeyondCapacity) {
  ASSERT_TRUE(add(1, 1, 1).has_value());
  forge_used(0, 9);
  expect_latched();
}

TEST_F(PackedHostileDeviceTest, AvailOnlyMarkerIsNotACompletion) {
  ASSERT_TRUE(add(1, 1, 1).has_value());
  auto u = driver_->try_get_used();
  ASSERT_TRUE(u.has_value());
  EXPECT_FALSE(u->has_value());
  EXPECT_FALSE(driver_->is_broken());
}

TEST_F(PackedHostileDeviceTest, WellFormedForgeryStillWorks) {
  ASSERT_TRUE(add(1, 1, 1).has_value());
  forge_used(0, 8);
  auto u = driver_->try_get_used();
  ASSERT_TRUE(u.has_value() && u->has_value());
  EXPECT_EQ((*u)->len, 8u);
}

TEST_F(PackedHostileDeviceTest, DoubleCompletionIsRejected) {
  ASSERT_TRUE(add(0, 1, 1).has_value());
  auto p = pop();
  ASSERT_TRUE(p.has_value() && p->has_value());
  ASSERT_TRUE(device_->try_push_used(**p, 0).has_value());
  ASSERT_TRUE(get(driver_->try_get_used()).has_value());
  // Device replays the same used descriptor at the next position.
  ASSERT_TRUE(try_write_object(ring_mem_, field(1, 8), std::uint32_t{0}).has_value());
  ASSERT_TRUE(rt::try_store16(ring_mem_, field(1, 12), 0).has_value());
  set_flags(1, desc_f_avail | desc_f_used);
  expect_latched();
}

} // namespace
