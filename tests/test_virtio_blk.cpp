// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <algorithm>
#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/virtio/split_ring.hpp>
#include <structo/virtio/virtio_blk.hpp>
#include <structo/virtio/virtio_mmio.hpp>

namespace {

using namespace structo;
using namespace structo::virtio;
namespace reg = structo::virtio::mmio_reg;

struct guest_space {};
using gaddr = phys_addr<void, guest_space>;
using gmem = direct_virtq_memory<guest_space>;
using driver_t = split_virtq_driver<guest_space, guest_space, gmem>;
using gtraits = virtq_memory_traits<gmem, guest_space>;
using sg_t = sg_entry<guest_space, std::uint64_t>;

constexpr std::uint32_t kQ = 8;
constexpr std::uint64_t kBase = 0x10000;
constexpr std::uint64_t kBuf = kBase + 0x2000;

struct ram_store {
  std::uint64_t capacity_sectors() const noexcept { return 16; }
  bool read_only() const noexcept { return ro; }
  reloco::result<void> try_read(std::uint64_t sector, reloco::span<std::byte> buf) noexcept {
    auto all = data.as_span();
    auto src = all.subspan(static_cast<std::size_t>(sector) * 512, buf.size());
    std::copy(src.begin(), src.end(), buf.begin());
    return {};
  }
  reloco::result<void> try_write(std::uint64_t sector, reloco::span<const std::byte> buf) noexcept {
    auto all = data.as_span();
    auto dst = all.subspan(static_cast<std::size_t>(sector) * 512, buf.size());
    std::copy(buf.begin(), buf.end(), dst.begin());
    return {};
  }
  reloco::result<void> try_flush() noexcept {
    ++flushes;
    return {};
  }
  bool ro = false;
  int flushes = 0;
  reloco::array<std::byte, 16 * 512> data{};
};

using blk_t = virtio_blk_function<guest_space, ram_store>;
using dev_t_ = virtio_mmio_device<guest_space, gmem, blk_t>;

class VirtioBlkTest : public ::testing::Test {
protected:
  void SetUp() override {
    for (std::size_t i = 0; i < store_.data.size(); ++i)
      store_.data[i] = static_cast<std::byte>(i * 7 + 1);
    ref_ = hypervisor::mmio_device_ref(dev_);
  }

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

  void features(std::uint64_t f) {
    wr(reg::driver_features_sel, 0);
    wr(reg::driver_features, static_cast<std::uint32_t>(f));
    wr(reg::driver_features_sel, 1);
    wr(reg::driver_features, static_cast<std::uint32_t>(f >> 32));
  }

  void bring_up() {
    wr(reg::status, 0);
    wr(reg::status, reg::status_acknowledge | reg::status_driver);
    features(std::uint64_t{1} << feature_version_1);
    wr(reg::status, reg::status_acknowledge | reg::status_driver | reg::status_features_ok);
    ASSERT_TRUE(rd(reg::status) & reg::status_features_ok);

    auto layout = try_split_layout(kQ, false);
    ASSERT_TRUE(layout.has_value());
    auto a = split_ring_addrs<guest_space>::try_from_contiguous(gaddr{kBase}, *layout);
    ASSERT_TRUE(a.has_value());
    wr(reg::queue_sel, 0);
    wr(reg::queue_num, kQ);
    wr(reg::queue_desc_low, static_cast<std::uint32_t>(a->desc.value));
    wr(reg::queue_driver_low, static_cast<std::uint32_t>(a->avail.value));
    wr(reg::queue_device_low, static_cast<std::uint32_t>(a->used.value));
    wr(reg::queue_ready, 1);
    ASSERT_EQ(rd(reg::queue_ready), 1u);
    wr(reg::status, rd(reg::status) | reg::status_driver_ok);

    auto d = driver_t::try_create(mem_, *a, kQ, slots_.as_span());
    ASSERT_TRUE(d.has_value());
    drv_.emplace(*d);
  }

  void put(std::uint64_t off, const void *src, std::size_t n) {
    auto r = gtraits::try_write(mem_, gaddr{kBuf + off},
                                reloco::span<const std::byte>(static_cast<const std::byte *>(src), n));
    ASSERT_TRUE(r.has_value());
  }
  void get_bytes(std::uint64_t off, void *dst, std::size_t n) {
    auto r = gtraits::try_read(mem_, gaddr{kBuf + off}, reloco::span<std::byte>(static_cast<std::byte *>(dst), n));
    ASSERT_TRUE(r.has_value());
  }

  // Submits header@0, optional data at 0x100, status at 0x1000; returns status byte.
  std::uint8_t request(std::uint32_t type, std::uint64_t sector, std::uint64_t data_len, bool write_dir,
                       std::uint64_t *used_len = nullptr) {
    reloco::array<std::byte, 16> hdr{};
    auto hs = hdr.as_span();
    store_le<std::uint32_t>(hs, type);
    store_le<std::uint64_t>(hs.subspan(8), sector);
    put(0, hdr.data(), hdr.size());
    const std::byte poison{0x55};
    put(0x1000, &poison, 1);

    reloco::array<sg_t, 2> out{};
    reloco::array<sg_t, 2> in{};
    std::size_t no = 0;
    std::size_t ni = 0;
    out[no++] = sg_t{gaddr{kBuf}, 16};
    if (data_len) {
      if (write_dir)
        out[no++] = sg_t{gaddr{kBuf + 0x100}, data_len};
      else
        in[ni++] = sg_t{gaddr{kBuf + 0x100}, data_len};
    }
    in[ni++] = sg_t{gaddr{kBuf + 0x1000}, 1};
    EXPECT_TRUE(drv_->try_add(reloco::span<const sg_t>(out.data(), no), reloco::span<const sg_t>(in.data(), ni), 1)
                    .has_value());
    EXPECT_TRUE(drv_->try_publish().has_value());
    wr(reg::queue_notify, 0);

    auto done = drv_->try_get_used();
    EXPECT_TRUE(done.has_value() && done->has_value());
    if (used_len && done && done->has_value())
      *used_len = (*done)->len;
    std::uint8_t st = 0xff;
    get_bytes(0x1000, &st, 1);
    return st;
  }

  alignas(16) reloco::array<std::byte, 0x4000> bytes_{};
  gmem mem_{bytes_.data(), bytes_.size(), gaddr{kBase}};
  ram_store store_;
  blk_t blk_{store_};
  dev_t_ dev_{mem_, blk_};
  hypervisor::mmio_device_ref ref_;
  reloco::array<split_driver_slot, kQ> slots_{};
  reloco::optional<driver_t> drv_;
};

TEST_F(VirtioBlkTest, IdentityRegisters) {
  EXPECT_EQ(rd(reg::magic), reg::magic_value);
  EXPECT_EQ(rd(reg::version), 2u);
  EXPECT_EQ(rd(reg::device_id), 2u);
  EXPECT_EQ(ref_.size(), reg::config + blk::config_size);
  EXPECT_EQ(rd(reg::config), 16u); // capacity low
  EXPECT_EQ(rd(reg::config + 4), 0u);
  wr(reg::device_features_sel, 1);
  EXPECT_NE(rd(reg::device_features) & 1u, 0u); // VERSION_1 in the high bank
  wr(reg::device_features_sel, 0);
  EXPECT_NE(rd(reg::device_features) & (1u << blk::feature_flush), 0u);
}

TEST_F(VirtioBlkTest, RejectsUnalignedAndWrongSizedAccess) {
  reloco::array<std::byte, 2> b2{};
  EXPECT_FALSE(ref_.try_read(reg::magic, b2.as_span()).has_value());
  reloco::array<std::byte, 4> b4{};
  EXPECT_FALSE(ref_.try_read(reg::magic + 1, b4.as_span()).has_value());
}

TEST_F(VirtioBlkTest, UnknownFeatureIsRefused) {
  wr(reg::status, reg::status_acknowledge | reg::status_driver);
  features((std::uint64_t{1} << feature_version_1) | (std::uint64_t{1} << 20));
  wr(reg::status, reg::status_acknowledge | reg::status_driver | reg::status_features_ok);
  EXPECT_EQ(rd(reg::status) & reg::status_features_ok, 0u);
}

TEST_F(VirtioBlkTest, MissingVersion1IsRefused) {
  wr(reg::status, reg::status_acknowledge | reg::status_driver);
  features(1u << blk::feature_flush);
  wr(reg::status, reg::status_acknowledge | reg::status_driver | reg::status_features_ok);
  EXPECT_EQ(rd(reg::status) & reg::status_features_ok, 0u);
}

TEST_F(VirtioBlkTest, QueueReadyRequiresFeaturesOk) {
  wr(reg::queue_sel, 0);
  wr(reg::queue_num, kQ);
  wr(reg::queue_ready, 1);
  EXPECT_EQ(rd(reg::queue_ready), 0u);
}

TEST_F(VirtioBlkTest, BadQueueAddressStaysNotReady) {
  wr(reg::status, reg::status_acknowledge | reg::status_driver);
  features(std::uint64_t{1} << feature_version_1);
  wr(reg::status, reg::status_acknowledge | reg::status_driver | reg::status_features_ok);
  wr(reg::queue_sel, 0);
  wr(reg::queue_num, kQ);
  wr(reg::queue_desc_low, 0x7000'0000);
  wr(reg::queue_driver_low, 0x7000'1000);
  wr(reg::queue_device_low, 0x7000'2000);
  wr(reg::queue_ready, 1);
  EXPECT_EQ(rd(reg::queue_ready), 0u);
}

TEST_F(VirtioBlkTest, ReadRequest) {
  bring_up();
  std::uint64_t len = 0;
  EXPECT_EQ(request(blk::req_in, 2, 1024, false, &len), blk::status_ok);
  EXPECT_EQ(len, 1025u);
  reloco::array<std::byte, 1024> got{};
  get_bytes(0x100, got.data(), got.size());
  EXPECT_TRUE(std::equal(got.begin(), got.end(), store_.data.begin() + 2 * 512));
  EXPECT_TRUE(dev_.irq_asserted());
  EXPECT_EQ(rd(reg::interrupt_status), reg::irq_used_buffer);
  wr(reg::interrupt_ack, reg::irq_used_buffer);
  EXPECT_FALSE(dev_.irq_asserted());
}

TEST_F(VirtioBlkTest, WriteRequest) {
  bring_up();
  reloco::array<std::byte, 512> payload{};
  for (auto &b : payload)
    b = std::byte{0xa5};
  put(0x100, payload.data(), payload.size());
  std::uint64_t len = 0;
  EXPECT_EQ(request(blk::req_out, 5, 512, true, &len), blk::status_ok);
  EXPECT_EQ(len, 1u);
  EXPECT_TRUE(std::equal(payload.begin(), payload.end(), store_.data.begin() + 5 * 512));
}

TEST_F(VirtioBlkTest, WriteRequestPreservesPayload) {
  bring_up();
  // request() rewrites only header + status; the payload placed by the test must survive.
  reloco::array<std::byte, 512> payload{};
  payload[3] = std::byte{9};
  put(0x100, payload.data(), payload.size());
  ASSERT_EQ(request(blk::req_out, 0, 512, true), blk::status_ok);
  EXPECT_EQ(store_.data[3], std::byte{9});
}

TEST_F(VirtioBlkTest, FlushRequest) {
  bring_up();
  EXPECT_EQ(request(blk::req_flush, 0, 0, false), blk::status_ok);
  EXPECT_EQ(store_.flushes, 1);
}

TEST_F(VirtioBlkTest, ConfigSpaceFields) {
  EXPECT_EQ(rd(reg::config + 12), 126u); // seg_max
  EXPECT_EQ(rd(reg::config + 20), 512u); // blk_size
  wr(reg::device_features_sel, 0);
  EXPECT_NE(rd(reg::device_features) & (1u << blk::feature_seg_max), 0u);
  EXPECT_NE(rd(reg::device_features) & (1u << blk::feature_blk_size), 0u);
}

TEST_F(VirtioBlkTest, GetId) {
  bring_up();
  std::uint64_t len = 0;
  EXPECT_EQ(request(blk::req_get_id, 0, blk::id_size, false, &len), blk::status_ok);
  EXPECT_EQ(len, blk::id_size + 1);
  reloco::array<std::byte, 4> id{};
  get_bytes(0x100, id.data(), id.size());
  EXPECT_EQ(id[0], std::byte{'s'});
  EXPECT_EQ(request(blk::req_get_id, 0, 8, false), blk::status_ioerr); // wrong buffer size
}

TEST_F(VirtioBlkTest, UnsupportedType) {
  bring_up();
  EXPECT_EQ(request(99, 0, 0, false), blk::status_unsupp);
}

TEST_F(VirtioBlkTest, OutOfRangeSector) {
  bring_up();
  EXPECT_EQ(request(blk::req_in, 15, 1024, false), blk::status_ioerr);
  EXPECT_EQ(request(blk::req_in, ~std::uint64_t{0}, 512, false), blk::status_ioerr);
  EXPECT_EQ(request(blk::req_in, 16, 512, false), blk::status_ioerr);
}

TEST_F(VirtioBlkTest, UnalignedLengthIsIoErr) {
  bring_up();
  EXPECT_EQ(request(blk::req_in, 0, 100, false), blk::status_ioerr);
}

TEST_F(VirtioBlkTest, ReadOnlyStoreRejectsWrites) {
  store_.ro = true;
  bring_up();
  EXPECT_EQ(request(blk::req_out, 0, 512, true), blk::status_ioerr);
  wr(reg::device_features_sel, 0);
  EXPECT_NE(rd(reg::device_features) & (1u << blk::feature_ro), 0u);
}

TEST_F(VirtioBlkTest, ResetDropsQueue) {
  bring_up();
  wr(reg::status, 0);
  EXPECT_EQ(rd(reg::status), 0u);
  wr(reg::queue_sel, 0);
  EXPECT_EQ(rd(reg::queue_ready), 0u);
  EXPECT_FALSE(dev_.try_kick(0).has_value());
}

TEST_F(VirtioBlkTest, HostileDescriptorLatchesNeedsReset) {
  bring_up();
  // Forge an avail entry whose head index is outside the ring.
  auto layout = try_split_layout(kQ, false);
  auto a = split_ring_addrs<guest_space>::try_from_contiguous(gaddr{kBase}, *layout);
  auto idx = a->avail_idx_addr();
  auto slot = a->avail_ring_at(0, kQ);
  ASSERT_TRUE(idx.has_value() && slot.has_value());
  using tr = virtq_memory_traits<gmem, guest_space>;
  ASSERT_TRUE(tr::try_store16(mem_, *slot, 0xffff).has_value());
  ASSERT_TRUE(tr::try_store16(mem_, *idx, 1).has_value());
  wr(reg::queue_notify, 0);
  EXPECT_NE(rd(reg::status) & reg::status_needs_reset, 0u);
}

TEST_F(VirtioBlkTest, SpuriousNotifyIsIgnored) {
  wr(reg::queue_notify, 0);
  wr(reg::queue_notify, 99);
  EXPECT_EQ(rd(reg::status), 0u);
}

TEST_F(VirtioBlkTest, ConfigGenerationBump) {
  EXPECT_EQ(rd(reg::config_generation), 0u);
  dev_.notify_config_changed();
  EXPECT_EQ(rd(reg::config_generation), 1u);
  EXPECT_EQ(rd(reg::interrupt_status), reg::irq_config_change);
}

} // namespace
