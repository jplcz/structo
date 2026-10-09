// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

// Test fixture: a virtio_mmio_device wrapping a Function, driven by the library's own
// split_virtq_driver, one ring per queue. Buffers live at kBuf.

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <reloco/optional.hpp>
#include <structo/virtio/split_ring.hpp>
#include <structo/virtio/virtio_mmio.hpp>

namespace virtio_test {

using namespace structo;
using namespace structo::virtio;
namespace reg = structo::virtio::mmio_reg;

struct guest_space {};
using gaddr = phys_addr<void, guest_space>;
using gmem = direct_virtq_memory<guest_space>;
using driver_t = split_virtq_driver<guest_space, guest_space, gmem>;
using gtraits = virtq_memory_traits<gmem, guest_space>;
using sg_t = sg_entry<guest_space, std::uint64_t>;

inline constexpr std::uint32_t kQ = 8;
inline constexpr std::uint64_t kBase = 0x10000;
inline constexpr std::uint64_t kBuf = kBase + 0x4000;

template <typename Function, std::size_t Queues> class mmio_fixture : public ::testing::Test {
protected:
  template <typename... Args> explicit mmio_fixture(Args &&...args) : fn_(static_cast<Args &&>(args)...) {}

  std::uint32_t rd(std::uint64_t off) {
    reloco::array<std::byte, 4> b{};
    EXPECT_TRUE(dev_.try_read(off, b.as_span()).has_value());
    return load_le<std::uint32_t>(b.as_span());
  }
  void wr(std::uint64_t off, std::uint32_t v) {
    reloco::array<std::byte, 4> b{};
    store_le<std::uint32_t>(b.as_span(), v);
    EXPECT_TRUE(dev_.try_write(off, reloco::span<const std::byte>(b.data(), 4)).has_value());
  }

  void bring_up(std::uint64_t features = 0) {
    wr(reg::status, 0);
    wr(reg::status, reg::status_acknowledge | reg::status_driver);
    features |= std::uint64_t{1} << feature_version_1;
    wr(reg::driver_features_sel, 0);
    wr(reg::driver_features, static_cast<std::uint32_t>(features));
    wr(reg::driver_features_sel, 1);
    wr(reg::driver_features, static_cast<std::uint32_t>(features >> 32));
    wr(reg::status, reg::status_acknowledge | reg::status_driver | reg::status_features_ok);
    ASSERT_TRUE(rd(reg::status) & reg::status_features_ok);
    for (std::uint32_t q = 0; q < Queues; ++q) {
      auto layout = try_split_layout(kQ, false);
      ASSERT_TRUE(layout.has_value());
      auto a = split_ring_addrs<guest_space>::try_from_contiguous(gaddr{kBase + 0x800 * q}, *layout);
      ASSERT_TRUE(a.has_value());
      wr(reg::queue_sel, q);
      wr(reg::queue_num, kQ);
      wr(reg::queue_desc_low, static_cast<std::uint32_t>(a->desc.value));
      wr(reg::queue_driver_low, static_cast<std::uint32_t>(a->avail.value));
      wr(reg::queue_device_low, static_cast<std::uint32_t>(a->used.value));
      wr(reg::queue_ready, 1);
      ASSERT_EQ(rd(reg::queue_ready), 1u);
      auto d = driver_t::try_create(mem_, *a, kQ, slots_[q].as_span());
      ASSERT_TRUE(d.has_value());
      drv_[q].emplace(*d);
    }
    wr(reg::status, rd(reg::status) | reg::status_driver_ok);
  }

  void put(std::uint64_t off, const void *src, std::size_t n) {
    ASSERT_TRUE(gtraits::try_write(mem_, gaddr{kBuf + off},
                                   reloco::span<const std::byte>(static_cast<const std::byte *>(src), n))
                    .has_value());
  }
  void get(std::uint64_t off, void *dst, std::size_t n) {
    ASSERT_TRUE(gtraits::try_read(mem_, gaddr{kBuf + off}, reloco::span<std::byte>(static_cast<std::byte *>(dst), n))
                    .has_value());
  }

  // Posts a device-readable buffer (guest output) / a device-writable buffer (guest input) without kicking.
  void post_out(std::uint32_t q, std::uint64_t off, std::uint64_t len, std::uintptr_t token = 1) {
    sg_t out[1] = {{gaddr{kBuf + off}, len}};
    ASSERT_TRUE(drv_[q]->try_add(reloco::span<const sg_t>(out, 1), reloco::span<const sg_t>(), token).has_value());
    ASSERT_TRUE(drv_[q]->try_publish().has_value());
  }
  void post_in(std::uint32_t q, std::uint64_t off, std::uint64_t len, std::uintptr_t token = 1) {
    sg_t in[1] = {{gaddr{kBuf + off}, len}};
    ASSERT_TRUE(drv_[q]->try_add(reloco::span<const sg_t>(), reloco::span<const sg_t>(in, 1), token).has_value());
    ASSERT_TRUE(drv_[q]->try_publish().has_value());
  }
  void kick(std::uint32_t q) { wr(reg::queue_notify, q); }

  // Returns the used length of the next completion, or -1 if none.
  std::int64_t reap(std::uint32_t q) {
    auto done = drv_[q]->try_get_used();
    if (!done || !done->has_value())
      return -1;
    return static_cast<std::int64_t>((*done)->len);
  }

  alignas(16) reloco::array<std::byte, 0x8000> bytes_{};
  gmem mem_{bytes_.data(), bytes_.size(), gaddr{kBase}};
  Function fn_;
  virtio_mmio_device<guest_space, gmem, Function> dev_{mem_, fn_};
  reloco::array<reloco::array<split_driver_slot, kQ>, Queues> slots_{};
  reloco::array<reloco::optional<driver_t>, Queues> drv_{};
};

} // namespace virtio_test
