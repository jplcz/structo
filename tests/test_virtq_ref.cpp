// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/virtio/virtq_memory_adapters.hpp>
#include <structo/virtio/virtq_ref.hpp>

namespace {

using namespace structo;
using namespace structo::virtio;

struct ring_space {};  // host window the ring really lives in
struct guest_space {}; // address space the guest (peer) uses for ring + buffers
struct buf_space {};

using ring_addr = phys_addr<void, ring_space>;
using guest_addr = phys_addr<void, guest_space>;
using buf_addr = phys_addr<void, buf_space>;
using ring_mem = direct_virtq_memory<ring_space>;
using buf_mem = direct_virtq_memory<buf_space>;
using sg_t = sg_entry<buf_space, std::uint64_t>;

constexpr std::uint32_t kQ = 8;
constexpr std::uint64_t kHostBase = 0x10000;
constexpr std::uint64_t kGuestBase = 0x8000'0000;
constexpr std::uint64_t kBufBase = 0x4000'0000;

template <typename R> auto get(R r) { return r.value(); }

// ---------------------------------------------------------------------------
// virtq_memory_ref
// ---------------------------------------------------------------------------

TEST(VirtqMemoryRefTest, UnboundFailsEverything) {
  virtq_memory_ref<ring_space> ref;
  EXPECT_FALSE(static_cast<bool>(ref));
  std::byte b[2] = {};
  EXPECT_EQ(ref.try_read(ring_addr{kHostBase}, reloco::span<std::byte>(b, 2)).error(),
            reloco::error::unsupported_operation);
  EXPECT_EQ(ref.try_load16(ring_addr{kHostBase}).error(), reloco::error::unsupported_operation);
  EXPECT_EQ(ref.try_store16(ring_addr{kHostBase}, 1).error(), reloco::error::unsupported_operation);
}

TEST(VirtqMemoryRefTest, ForwardsToBackend) {
  alignas(16) reloco::array<std::byte, 64> bytes{};
  ring_mem mem(bytes.data(), bytes.size(), ring_addr{kHostBase});
  virtq_memory_ref<ring_space> ref(mem);
  ASSERT_TRUE(static_cast<bool>(ref));

  ASSERT_TRUE(ref.try_store16(ring_addr{kHostBase + 4}, 0xBEEF).has_value());
  EXPECT_EQ(get(ref.try_load16(ring_addr{kHostBase + 4})), 0xBEEF);
  EXPECT_EQ(get(ref.try_load16(ring_addr{kHostBase + 4})),
            get(virtq_memory_traits<ring_mem, ring_space>::try_load16(mem, ring_addr{kHostBase + 4})));

  auto oob = ref.try_load16(ring_addr{kHostBase + 4096});
  ASSERT_FALSE(oob.has_value());
  EXPECT_EQ(oob.error(), reloco::error::out_of_range);
}

// ---------------------------------------------------------------------------
// translating_virtq_memory
// ---------------------------------------------------------------------------

struct guest_policy {
  using from_space = guest_space;
  using to_space = ring_space;
  std::uint64_t window = 4096;

  reloco::result<std::uint64_t> translate(std::uint64_t gpa, std::uint64_t len) const noexcept {
    if (gpa < kGuestBase)
      return reloco::unexpected(reloco::error::out_of_range);
    const std::uint64_t off = gpa - kGuestBase;
    if (off > window || len > window - off)
      return reloco::unexpected(reloco::error::out_of_range);
    return kHostBase + off;
  }
};
using guest_translator = phys_translator<guest_policy>;
using guest_mem = translating_virtq_memory<guest_translator, ring_mem>;

class VirtqTranslatedTest : public ::testing::Test {
protected:
  alignas(4096) reloco::array<std::byte, 4096> ring_bytes_{};
  ring_mem host_{ring_bytes_.data(), ring_bytes_.size(), ring_addr{kHostBase}};
  guest_mem mem_{guest_translator(guest_policy{}), host_};
  using traits = virtq_memory_traits<guest_mem, guest_space>;
};

TEST_F(VirtqTranslatedTest, AccessesLandAtTranslatedAddress) {
  ASSERT_TRUE(traits::try_store16(mem_, guest_addr{kGuestBase + 16}, 0x1234).has_value());
  EXPECT_EQ(get(virtq_memory_traits<ring_mem, ring_space>::try_load16(host_, ring_addr{kHostBase + 16})), 0x1234);

  const std::uint32_t v = 0xAABBCCDD;
  ASSERT_TRUE(try_write_object(mem_, guest_addr{kGuestBase + 32}, v).has_value());
  EXPECT_EQ(get(try_read_object<std::uint32_t>(host_, ring_addr{kHostBase + 32})), v);
}

TEST_F(VirtqTranslatedTest, TranslationFailureNeverReachesInner) {
  EXPECT_EQ(traits::try_load16(mem_, guest_addr{kGuestBase - 2}).error(), reloco::error::out_of_range);
  EXPECT_EQ(traits::try_load16(mem_, guest_addr{kGuestBase + 4095}).error(),
            reloco::error::out_of_range); // 2-byte access straddles the end
  std::byte big[16] = {};
  EXPECT_EQ(traits::try_read(mem_, guest_addr{kGuestBase + 4090}, reloco::span<std::byte>(big, 16)).error(),
            reloco::error::out_of_range);
  EXPECT_EQ(traits::try_store16(mem_, guest_addr{~std::uint64_t{0} - 8}, 1).error(), reloco::error::out_of_range);
}

TEST_F(VirtqTranslatedTest, ComposesWithTypeErasedRef) {
  virtq_memory_ref<guest_space> ref(mem_);
  ASSERT_TRUE(ref.try_store16(guest_addr{kGuestBase + 8}, 7).has_value());
  EXPECT_EQ(get(ref.try_load16(guest_addr{kGuestBase + 8})), 7);
  EXPECT_FALSE(ref.try_load16(guest_addr{0x1000}).has_value());
}

// ---------------------------------------------------------------------------
// Ring over translated + type-erased memory (guest-address ring, host window)
// ---------------------------------------------------------------------------

TEST_F(VirtqTranslatedTest, SplitRingOverTranslatedMemory) {
  using mem_ref = virtq_memory_ref<guest_space>;
  using drv_t = split_virtq_driver<guest_space, buf_space, mem_ref>;
  using dev_t = split_virtq_device<guest_space, buf_space, mem_ref>;
  mem_ref ref(mem_);

  auto layout = try_split_layout(kQ, false);
  ASSERT_TRUE(layout.has_value());
  auto addrs = split_ring_addrs<guest_space>::try_from_contiguous(guest_addr{kGuestBase}, *layout);
  ASSERT_TRUE(addrs.has_value());
  reloco::array<split_driver_slot, kQ> slots{};
  auto dr = drv_t::try_create(ref, *addrs, kQ, slots.as_span());
  auto dv = dev_t::try_create(ref, *addrs, kQ);
  ASSERT_TRUE(dr.has_value() && dv.has_value());

  reloco::array<sg_t, 1> in{{sg_t{buf_addr{kBufBase}, 8}}};
  ASSERT_TRUE(dr->try_add({}, reloco::span<const sg_t>(in.data(), 1), 42).has_value());
  ASSERT_TRUE(dr->try_publish().has_value());
  reloco::array<chain_segment<buf_space>, kQ> segs{};
  auto p = dv->try_pop(segs.as_span());
  ASSERT_TRUE(p.has_value() && p->has_value());
  ASSERT_TRUE(dv->try_push_used(**p, 8).has_value());
  auto u = dr->try_get_used();
  ASSERT_TRUE(u.has_value() && u->has_value());
  EXPECT_EQ((*u)->token, 42u);
}

TEST_F(VirtqTranslatedTest, RingBeyondMappedWindowIsRejectedAtUse) {
  using mem_ref = virtq_memory_ref<guest_space>;
  using dev_t = split_virtq_device<guest_space, buf_space, mem_ref>;
  mem_ref ref(mem_);
  auto layout = try_split_layout(kQ, false);
  // Ring placed so that its used area falls past the 4 KiB window.
  auto addrs = split_ring_addrs<guest_space>::try_from_contiguous(guest_addr{kGuestBase + 3800}, *layout);
  ASSERT_TRUE(addrs.has_value());
  auto dv = dev_t::try_create(ref, *addrs, kQ);
  ASSERT_FALSE(dv.has_value());
  EXPECT_EQ(dv.error(), reloco::error::invalid_argument);
}

// ---------------------------------------------------------------------------
// virtq_driver_ref / virtq_device_ref over split and packed rings
// ---------------------------------------------------------------------------

class VirtqRefTest : public ::testing::Test {
protected:
  alignas(4096) reloco::array<std::byte, 4096> ring_bytes_{};
  ring_mem mem_{ring_bytes_.data(), ring_bytes_.size(), ring_addr{kHostBase}};
  reloco::array<chain_segment<buf_space>, kQ> segs_{};
};

// Same code path for any ring layout: this is the point of the handles.
void round_trip(virtq_driver_ref<buf_space> drv, virtq_device_ref<buf_space> dev,
                reloco::span<chain_segment<buf_space>> segs) {
  ASSERT_TRUE(static_cast<bool>(drv) && static_cast<bool>(dev));
  EXPECT_EQ(drv.queue_size(), kQ);
  EXPECT_EQ(dev.queue_size(), kQ);

  reloco::array<sg_t, 1> out{{sg_t{buf_addr{kBufBase}, 8}}};
  reloco::array<sg_t, 2> in{{sg_t{buf_addr{kBufBase + 64}, 8}, sg_t{buf_addr{kBufBase + 128}, 8}}};
  for (std::uintptr_t i = 0; i < 1000; ++i) {
    ASSERT_TRUE(
        drv.try_add(reloco::span<const sg_t>(out.data(), 1), reloco::span<const sg_t>(in.data(), 2), i).has_value());
    ASSERT_TRUE(drv.try_publish().has_value());
    ASSERT_TRUE(get(drv.needs_notify()));

    auto p = dev.try_pop(segs);
    ASSERT_TRUE(p.has_value() && p->has_value());
    EXPECT_EQ((*p)->readable.size(), 1u);
    EXPECT_EQ((*p)->writable.size(), 2u);
    ASSERT_TRUE(dev.try_push_used(**p, 16).has_value());
    ASSERT_TRUE(get(dev.should_interrupt()));

    auto u = drv.try_get_used();
    ASSERT_TRUE(u.has_value() && u->has_value());
    ASSERT_EQ((*u)->token, i);
    ASSERT_EQ((*u)->len, 16u);
  }
  EXPECT_EQ(drv.free_descriptors(), kQ);
  EXPECT_FALSE(drv.is_broken());
  EXPECT_FALSE(dev.is_broken());

  ASSERT_TRUE(dev.try_set_notify_enabled(false).has_value());
  EXPECT_FALSE(get(drv.needs_notify()));
  ASSERT_TRUE(drv.try_set_interrupts_enabled(false).has_value());
  EXPECT_FALSE(get(dev.should_interrupt()));
}

TEST_F(VirtqRefTest, SplitRing) {
  using drv_t = split_virtq_driver<ring_space, buf_space, ring_mem>;
  using dev_t = split_virtq_device<ring_space, buf_space, ring_mem>;
  auto layout = try_split_layout(kQ, false);
  auto addrs = split_ring_addrs<ring_space>::try_from_contiguous(ring_addr{kHostBase}, *layout);
  reloco::array<split_driver_slot, kQ> slots{};
  auto dr = drv_t::try_create(mem_, *addrs, kQ, slots.as_span());
  auto dv = dev_t::try_create(mem_, *addrs, kQ);
  ASSERT_TRUE(dr.has_value() && dv.has_value());
  round_trip(virtq_driver_ref<buf_space>(*dr), virtq_device_ref<buf_space>(*dv), segs_.as_span());
}

TEST_F(VirtqRefTest, PackedRing) {
  using drv_t = packed_virtq_driver<ring_space, buf_space, ring_mem>;
  using dev_t = packed_virtq_device<ring_space, buf_space, ring_mem>;
  auto layout = try_packed_layout(kQ);
  auto addrs = packed_ring_addrs<ring_space>::try_from_contiguous(ring_addr{kHostBase}, *layout);
  reloco::array<packed_driver_slot, kQ> slots{};
  auto dr = drv_t::try_create(mem_, *addrs, kQ, slots.as_span());
  auto dv = dev_t::try_create(mem_, *addrs, kQ);
  ASSERT_TRUE(dr.has_value() && dv.has_value());
  round_trip(virtq_driver_ref<buf_space>(*dr), virtq_device_ref<buf_space>(*dv), segs_.as_span());
}

TEST_F(VirtqRefTest, UnboundHandles) {
  virtq_driver_ref<buf_space> drv;
  virtq_device_ref<buf_space> dev;
  EXPECT_FALSE(static_cast<bool>(drv));
  EXPECT_EQ(drv.try_add({}, {}, 1).error(), reloco::error::unsupported_operation);
  EXPECT_EQ(drv.try_publish().error(), reloco::error::unsupported_operation);
  EXPECT_EQ(drv.needs_notify().error(), reloco::error::unsupported_operation);
  EXPECT_EQ(drv.try_get_used().error(), reloco::error::unsupported_operation);
  EXPECT_EQ(drv.queue_size(), 0u);
  EXPECT_TRUE(drv.is_broken());
  EXPECT_EQ(dev.try_pop(segs_.as_span()).error(), reloco::error::unsupported_operation);
  EXPECT_EQ(dev.should_interrupt().error(), reloco::error::unsupported_operation);
  EXPECT_TRUE(dev.is_broken());
}

// A copy of a non-const handle must copy the binding, not bind to the handle.
TEST_F(VirtqRefTest, CopyingNonConstHandleKeepsBinding) {
  using drv_t = split_virtq_driver<ring_space, buf_space, ring_mem>;
  using dev_t = split_virtq_device<ring_space, buf_space, ring_mem>;
  auto layout = try_split_layout(kQ, false);
  auto addrs = split_ring_addrs<ring_space>::try_from_contiguous(ring_addr{kHostBase}, *layout);
  reloco::array<split_driver_slot, kQ> slots{};
  auto dr = drv_t::try_create(mem_, *addrs, kQ, slots.as_span());
  auto dv = dev_t::try_create(mem_, *addrs, kQ);
  ASSERT_TRUE(dr.has_value() && dv.has_value());

  virtq_driver_ref<buf_space> copy_d;
  virtq_device_ref<buf_space> copy_v;
  virtq_memory_ref<ring_space> copy_m;
  {
    virtq_driver_ref<buf_space> d(*dr);
    virtq_device_ref<buf_space> v(*dv);
    virtq_memory_ref<ring_space> m(mem_);
    copy_d = virtq_driver_ref<buf_space>(d);
    copy_v = virtq_device_ref<buf_space>(v);
    copy_m = virtq_memory_ref<ring_space>(m);
  }
  EXPECT_EQ(copy_d.queue_size(), kQ);
  EXPECT_EQ(copy_v.queue_size(), kQ);
  EXPECT_TRUE(copy_m.try_load16(ring_addr{kHostBase}).has_value());
}

TEST_F(VirtqRefTest, HandleReportsBrokenQueue) {
  using drv_t = split_virtq_driver<ring_space, buf_space, ring_mem>;
  using dev_t = split_virtq_device<ring_space, buf_space, ring_mem>;
  auto layout = try_split_layout(kQ, false);
  auto addrs = split_ring_addrs<ring_space>::try_from_contiguous(ring_addr{kHostBase}, *layout);
  reloco::array<split_driver_slot, kQ> slots{};
  auto dr = drv_t::try_create(mem_, *addrs, kQ, slots.as_span());
  auto dv = dev_t::try_create(mem_, *addrs, kQ);
  ASSERT_TRUE(dr.has_value() && dv.has_value());
  virtq_device_ref<buf_space> dev(*dv);

  // Hostile avail index (more pending than the ring holds).
  auto idx_at = addrs->avail_idx_addr();
  using ring_traits = virtq_memory_traits<ring_mem, ring_space>;
  ASSERT_TRUE(ring_traits::try_store16(mem_, *idx_at, 100).has_value());
  auto p = dev.try_pop(segs_.as_span());
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), reloco::error::security_violation);
  EXPECT_TRUE(dev.is_broken());
}

} // namespace
