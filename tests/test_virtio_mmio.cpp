// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <algorithm>
#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/virtio/virtio_mmio.hpp>

namespace {

using namespace structo;
using namespace structo::virtio;
namespace reg = structo::virtio::mmio_reg;

struct guest_space {};
using gaddr = phys_addr<void, guest_space>;
using gmem = direct_virtq_memory<guest_space>;
using sg_t = sg_entry<guest_space, std::uint64_t>;

constexpr std::uint32_t kQ = 8;
constexpr std::uint64_t kBase = 0x10000;
constexpr std::uint64_t kQueueStride = 0x1000;
constexpr std::uint64_t kBuf = kBase + 0x4000;
constexpr std::uint64_t kTable = kBase + 0x6000;

// Two-queue device: copies each request's readable bytes into its writable buffer.
struct echo_fn {
  static constexpr std::uint32_t device_id = 63;
  static constexpr std::uint32_t queue_count = 2;
  static constexpr std::uint32_t queue_max_size = kQ;

  std::uint64_t device_features() const noexcept { return 0; }
  std::size_t config_size() const noexcept { return 4; }
  reloco::result<void> try_read_config(std::uint64_t, reloco::span<std::byte> dst) noexcept {
    std::fill(dst.begin(), dst.end(), std::byte{0});
    return {};
  }

  template <typename QueueView, typename Mem>
  reloco::result<void> process(Mem &mem, std::uint32_t qidx, QueueView &q) noexcept {
    for (;;) {
      auto popped = q.try_pop(reloco::span<typename QueueView::segment>(segs.data(), segs.size()));
      if (!popped)
        return reloco::unexpected(popped.error());
      if (!popped->has_value())
        return {};
      const auto &c = **popped;
      const std::size_t n = std::min<std::uint64_t>({c.readable_bytes, c.writable_bytes, buf.size()});
      if (n) {
        if (auto r = try_read_chain(mem, c.readable, 0, reloco::span<std::byte>(buf.data(), n)); !r)
          return r;
        if (auto r = try_write_chain(mem, c.writable, 0, reloco::span<const std::byte>(buf.data(), n)); !r)
          return r;
      }
      ++served[qidx];
      if (auto r = q.try_push_used(c, static_cast<std::uint32_t>(n)); !r)
        return r;
    }
  }

  reloco::array<chain_segment<guest_space>, kQ> segs{};
  reloco::array<std::byte, 64> buf{};
  reloco::array<int, 2> served{};
};

using dev_t_ = virtio_mmio_device<guest_space, gmem, echo_fn>;
using split_drv = split_virtq_driver<guest_space, guest_space, gmem>;
using packed_drv = packed_virtq_driver<guest_space, guest_space, gmem>;
using gtraits = virtq_memory_traits<gmem, guest_space>;

class VirtioMmioTest : public ::testing::Test {
protected:
  void SetUp() override { ref_ = hypervisor::mmio_device_ref(dev_); }

  std::uint32_t rd(std::uint64_t off) {
    reloco::array<std::byte, 4> b{};
    EXPECT_TRUE(ref_.try_read(off, b.as_span()).has_value());
    return load_le<std::uint32_t>(b.as_span());
  }
  void wr(std::uint64_t off, std::uint32_t v) {
    reloco::array<std::byte, 4> b{};
    store_le<std::uint32_t>(b.as_span(), v);
    EXPECT_TRUE(ref_.try_write(off, reloco::span<const std::byte>(b.data(), 4)).has_value());
  }

  // Returns whether the device kept FEATURES_OK.
  bool negotiate(std::uint64_t f) {
    wr(reg::status, 0);
    wr(reg::status, reg::status_acknowledge | reg::status_driver);
    wr(reg::driver_features_sel, 0);
    wr(reg::driver_features, static_cast<std::uint32_t>(f));
    wr(reg::driver_features_sel, 1);
    wr(reg::driver_features, static_cast<std::uint32_t>(f >> 32));
    wr(reg::status, reg::status_acknowledge | reg::status_driver | reg::status_features_ok);
    return (rd(reg::status) & reg::status_features_ok) != 0;
  }

  static std::uint64_t bit(unsigned b) { return std::uint64_t{1} << b; }

  void program_queue(std::uint32_t qi, std::uint64_t desc, std::uint64_t drv_area, std::uint64_t dev_area) {
    wr(reg::queue_sel, qi);
    wr(reg::queue_num, kQ);
    wr(reg::queue_desc_low, static_cast<std::uint32_t>(desc));
    wr(reg::queue_desc_high, static_cast<std::uint32_t>(desc >> 32));
    wr(reg::queue_driver_low, static_cast<std::uint32_t>(drv_area));
    wr(reg::queue_driver_high, static_cast<std::uint32_t>(drv_area >> 32));
    wr(reg::queue_device_low, static_cast<std::uint32_t>(dev_area));
    wr(reg::queue_device_high, static_cast<std::uint32_t>(dev_area >> 32));
    wr(reg::queue_ready, 1);
  }

  template <bool Packed, bool EventIdx, bool Indirect> void run_echo() {
    std::uint64_t f = bit(feature_version_1);
    if (EventIdx)
      f |= bit(feature_ring_event_idx);
    if (Packed)
      f |= bit(feature_ring_packed);
    if (Indirect)
      f |= bit(feature_ring_indirect_desc);
    ASSERT_TRUE(negotiate(f));

    using drv_t = std::conditional_t<Packed, packed_drv, split_drv>;
    reloco::array<reloco::optional<drv_t>, 2> drivers{};
    reloco::array<reloco::array<split_driver_slot, kQ>, 2> ssl{};
    reloco::array<reloco::array<packed_driver_slot, kQ>, 2> psl{};

    for (std::uint32_t qi = 0; qi < 2; ++qi) {
      const gaddr base{kBase + qi * kQueueStride};
      if constexpr (Packed) {
        auto layout = try_packed_layout(kQ);
        ASSERT_TRUE(layout.has_value());
        auto a = packed_ring_addrs<guest_space>::try_from_contiguous(base, *layout);
        ASSERT_TRUE(a.has_value());
        program_queue(qi, a->desc.value, a->driver_event.value, a->device_event.value);
        auto d = packed_drv::try_create(mem_, *a, kQ, psl[qi].as_span(), EventIdx);
        ASSERT_TRUE(d.has_value());
        drivers[qi].emplace(*d);
      } else {
        auto layout = try_split_layout(kQ, EventIdx);
        ASSERT_TRUE(layout.has_value());
        auto a = split_ring_addrs<guest_space>::try_from_contiguous(base, *layout);
        ASSERT_TRUE(a.has_value());
        program_queue(qi, a->desc.value, a->avail.value, a->used.value);
        auto d = split_drv::try_create(mem_, *a, kQ, ssl[qi].as_span(), EventIdx);
        ASSERT_TRUE(d.has_value());
        drivers[qi].emplace(*d);
      }
      ASSERT_EQ(rd(reg::queue_ready), 1u);
    }
    wr(reg::status, rd(reg::status) | reg::status_driver_ok);

    // Two rounds per queue: the second proves the device re-armed kicks.
    for (int round = 0; round < 2; ++round) {
      for (std::uint32_t qi = 0; qi < 2; ++qi) {
        const std::uint64_t src = kBuf + qi * 0x200;
        const std::uint64_t dst = src + 0x100;
        reloco::array<std::byte, 16> msg{};
        for (std::size_t i = 0; i < msg.size(); ++i)
          msg[i] = static_cast<std::byte>(qi * 16 + i + static_cast<unsigned>(round));
        ASSERT_TRUE(gtraits::try_write(mem_, gaddr{src}, reloco::span<const std::byte>(msg.data(), msg.size())).has_value());

        const sg_t out{gaddr{src}, 16};
        const sg_t in{gaddr{dst}, 16};
        const reloco::span<const sg_t> outs(&out, 1);
        const reloco::span<const sg_t> ins(&in, 1);
        auto &d = *drivers[qi];
        if constexpr (Packed) {
          ASSERT_TRUE(d.try_add(outs, ins, 7).has_value());
        } else if constexpr (Indirect) {
          ASSERT_TRUE(d.try_add_indirect(outs, ins, 7, mem_, gaddr{kTable + qi * 0x100}, gaddr{kTable + qi * 0x100}, 64)
                          .has_value());
          ASSERT_TRUE(d.try_publish().has_value());
        } else {
          ASSERT_TRUE(d.try_add(outs, ins, 7).has_value());
          ASSERT_TRUE(d.try_publish().has_value());
        }
        auto need = d.needs_notify();
        ASSERT_TRUE(need.has_value());
        ASSERT_TRUE(*need);
        wr(reg::queue_notify, qi);

        auto done = d.try_get_used();
        ASSERT_TRUE(done.has_value() && done->has_value());
        EXPECT_EQ((*done)->token, 7u);
        EXPECT_EQ((*done)->len, 16u);
        reloco::array<std::byte, 16> got{};
        ASSERT_TRUE(gtraits::try_read(mem_, gaddr{dst}, got.as_span()).has_value());
        EXPECT_TRUE(std::equal(got.begin(), got.end(), msg.begin()));
      }
    }
    EXPECT_EQ(fn_.served[0], 2);
    EXPECT_EQ(fn_.served[1], 2);
    EXPECT_TRUE(dev_.irq_asserted());
    wr(reg::interrupt_ack, reg::irq_used_buffer);
    EXPECT_EQ(rd(reg::status) & reg::status_needs_reset, 0u);
  }

  alignas(16) reloco::array<std::byte, 0x8000> bytes_{};
  gmem mem_{bytes_.data(), bytes_.size(), gaddr{kBase}};
  echo_fn fn_;
  dev_t_ dev_{mem_, fn_};
  hypervisor::mmio_device_ref ref_;
};

TEST_F(VirtioMmioTest, SplitEcho) { run_echo<false, false, false>(); }
TEST_F(VirtioMmioTest, SplitEventIdxEcho) { run_echo<false, true, false>(); }
TEST_F(VirtioMmioTest, SplitIndirectEcho) { run_echo<false, false, true>(); }
TEST_F(VirtioMmioTest, SplitEventIdxIndirectEcho) { run_echo<false, true, true>(); }
TEST_F(VirtioMmioTest, PackedEcho) { run_echo<true, false, false>(); }
TEST_F(VirtioMmioTest, PackedEventIdxEcho) { run_echo<true, true, false>(); }

TEST_F(VirtioMmioTest, PackedWithIndirectIsRefused) {
  EXPECT_FALSE(negotiate(bit(feature_version_1) | bit(feature_ring_packed) | bit(feature_ring_indirect_desc)));
}

TEST_F(VirtioMmioTest, OffersTransportFeatures) {
  wr(reg::device_features_sel, 0);
  EXPECT_NE(rd(reg::device_features) & (1u << feature_ring_event_idx), 0u);
  EXPECT_NE(rd(reg::device_features) & (1u << feature_ring_indirect_desc), 0u);
  wr(reg::device_features_sel, 1);
  const std::uint32_t hi = rd(reg::device_features);
  EXPECT_NE(hi & (1u << (feature_version_1 - 32)), 0u);
  EXPECT_NE(hi & (1u << (feature_ring_packed - 32)), 0u);
  wr(reg::device_features_sel, 2);
  EXPECT_EQ(rd(reg::device_features), 0u);
}

TEST_F(VirtioMmioTest, QueueSelection) {
  wr(reg::queue_sel, 1);
  EXPECT_EQ(rd(reg::queue_num_max), kQ);
  wr(reg::queue_sel, 2);
  EXPECT_EQ(rd(reg::queue_num_max), 0u);
  EXPECT_EQ(rd(reg::queue_ready), 0u);
}

TEST_F(VirtioMmioTest, FeaturesFrozenAfterFeaturesOk) {
  ASSERT_TRUE(negotiate(bit(feature_version_1)));
  wr(reg::driver_features_sel, 0);
  wr(reg::driver_features, 1u << feature_ring_event_idx);
  EXPECT_EQ(dev_.driver_features(), bit(feature_version_1));
}

TEST_F(VirtioMmioTest, ConfigIsReadOnly) {
  reloco::array<std::byte, 4> b{};
  EXPECT_FALSE(ref_.try_write(reg::config, reloco::span<const std::byte>(b.data(), 4)).has_value());
}

TEST_F(VirtioMmioTest, DeviceResetViaHandle) {
  ASSERT_TRUE(negotiate(bit(feature_version_1)));
  EXPECT_TRUE(ref_.try_reset().has_value());
  EXPECT_EQ(rd(reg::status), 0u);
}

TEST_F(VirtioMmioTest, NeedsResetIsStickyUntilReset) {
  ASSERT_TRUE(negotiate(bit(feature_version_1)));
  auto layout = try_split_layout(kQ, false);
  auto a = split_ring_addrs<guest_space>::try_from_contiguous(gaddr{kBase}, *layout);
  program_queue(0, a->desc.value, a->avail.value, a->used.value);
  wr(reg::status, rd(reg::status) | reg::status_driver_ok);
  auto idx = a->avail_idx_addr();
  auto slot = a->avail_ring_at(0, kQ);
  ASSERT_TRUE(idx.has_value() && slot.has_value());
  ASSERT_TRUE(gtraits::try_store16(mem_, *slot, 0xffff).has_value());
  ASSERT_TRUE(gtraits::try_store16(mem_, *idx, 1).has_value());
  wr(reg::queue_notify, 0);
  EXPECT_NE(rd(reg::status) & reg::status_needs_reset, 0u);
  wr(reg::status, reg::status_acknowledge); // cannot clear NEEDS_RESET
  EXPECT_NE(rd(reg::status) & reg::status_needs_reset, 0u);
  wr(reg::status, 0);
  EXPECT_EQ(rd(reg::status), 0u);
}

} // namespace
