// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <reloco/optional.hpp>
#include <structo/virtio/split_ring.hpp>
#include <structo/virtio/virtio_mmio.hpp>
#include <structo/virtio/virtio_rpmsg.hpp>

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
constexpr std::uint64_t kRing0 = kBase;
constexpr std::uint64_t kRing1 = kBase + 0x1000;
constexpr std::uint64_t kBuf = kBase + 0x2000;
constexpr std::uint64_t kBufSize = 512;

using rpmsg_t = virtio_rpmsg_function<guest_space, 2, 64, 2>;
using dev_t_ = virtio_mmio_device<guest_space, gmem, rpmsg_t>;

struct seen {
  int calls = 0;
  std::uint32_t src = 0;
  std::uint32_t dst = 0;
  std::size_t len = 0;
  std::byte first{};
};

class VirtioRpmsgTest : public ::testing::Test {
protected:
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

  void setup_queue(std::uint32_t idx, std::uint64_t base) {
    auto layout = try_split_layout(kQ, false);
    ASSERT_TRUE(layout.has_value());
    auto a = split_ring_addrs<guest_space>::try_from_contiguous(gaddr{base}, *layout);
    ASSERT_TRUE(a.has_value());
    wr(reg::queue_sel, idx);
    wr(reg::queue_num, kQ);
    wr(reg::queue_desc_low, static_cast<std::uint32_t>(a->desc.value));
    wr(reg::queue_driver_low, static_cast<std::uint32_t>(a->avail.value));
    wr(reg::queue_device_low, static_cast<std::uint32_t>(a->used.value));
    wr(reg::queue_ready, 1);
    ASSERT_EQ(rd(reg::queue_ready), 1u);
    auto d = driver_t::try_create(mem_, *a, kQ, slots_[idx].as_span());
    ASSERT_TRUE(d.has_value());
    drv_[idx].emplace(*d);
  }

  void bring_up(bool ns = true) {
    wr(reg::status, 0);
    wr(reg::status, reg::status_acknowledge | reg::status_driver);
    wr(reg::driver_features_sel, 0);
    wr(reg::driver_features, ns ? 1u : 0u);
    wr(reg::driver_features_sel, 1);
    wr(reg::driver_features, 1u << (feature_version_1 - 32));
    wr(reg::status, reg::status_acknowledge | reg::status_driver | reg::status_features_ok);
    ASSERT_TRUE(rd(reg::status) & reg::status_features_ok);
    setup_queue(0, kRing0);
    setup_queue(1, kRing1);
    wr(reg::status, rd(reg::status) | reg::status_driver_ok);
  }

  void put(std::uint64_t off, const void *src, std::size_t n) {
    ASSERT_TRUE(gtraits::try_write(mem_, gaddr{kBuf + off},
                                   reloco::span<const std::byte>(static_cast<const std::byte *>(src), n))
                    .has_value());
  }
  void get(std::uint64_t off, void *dst, std::size_t n) {
    ASSERT_TRUE(
        gtraits::try_read(mem_, gaddr{kBuf + off}, reloco::span<std::byte>(static_cast<std::byte *>(dst), n))
            .has_value());
  }

  // Guest sends one rpmsg buffer (queue 1) and kicks. `claimed_len` lets tests lie about the length.
  void guest_send(std::uint32_t src, std::uint32_t dst, std::size_t payload_len, std::uint16_t claimed_len,
                  std::byte fill = std::byte{0x5a}) {
    reloco::array<std::byte, kBufSize> buf{};
    rpmsg::header h;
    h.src = src;
    h.dst = dst;
    h.len = claimed_len;
    rpmsg::encode_header(buf.as_span(), h);
    for (std::size_t i = 0; i < payload_len; ++i)
      buf[rpmsg::header_size + i] = fill;
    const std::uint64_t at = 0x1000 + tx_slot_ * kBufSize;
    tx_slot_ = (tx_slot_ + 1) % 4;
    put(at, buf.data(), rpmsg::header_size + payload_len);
    reloco::array<sg_t, 1> out{sg_t{gaddr{kBuf + at}, rpmsg::header_size + payload_len}};
    ASSERT_TRUE(drv_[1]->try_add(reloco::span<const sg_t>(out.data(), 1), reloco::span<const sg_t>(), 1).has_value());
    ASSERT_TRUE(drv_[1]->try_publish().has_value());
    wr(reg::queue_notify, 1);
    auto done = drv_[1]->try_get_used();
    ASSERT_TRUE(done.has_value() && done->has_value());
  }

  // Guest posts an empty rx buffer of `size` bytes on queue 0 (without kicking).
  void post_rx(std::uint64_t slot, std::uint64_t size = kBufSize) {
    reloco::array<sg_t, 1> in{sg_t{gaddr{kBuf + slot * kBufSize}, size}};
    ASSERT_TRUE(drv_[0]->try_add(reloco::span<const sg_t>(), reloco::span<const sg_t>(in.data(), 1), slot).has_value());
    ASSERT_TRUE(drv_[0]->try_publish().has_value());
  }

  alignas(16) reloco::array<std::byte, 0x4000> bytes_{};
  gmem mem_{bytes_.data(), bytes_.size(), gaddr{kBase}};
  rpmsg_t rpmsg_;
  dev_t_ dev_{mem_, rpmsg_};
  reloco::array<reloco::array<split_driver_slot, kQ>, 2> slots_{};
  reloco::array<reloco::optional<driver_t>, 2> drv_{};
  std::size_t tx_slot_ = 0;
};

TEST_F(VirtioRpmsgTest, IdentityAndFeatures) {
  EXPECT_EQ(rd(reg::device_id), 7u);
  wr(reg::device_features_sel, 0);
  EXPECT_EQ(rd(reg::device_features) & 1u, 1u);
  EXPECT_EQ(dev_.size(), reg::config);

  rpmsg_t no_ns(false);
  EXPECT_EQ(no_ns.device_features(), 0u);
}

TEST_F(VirtioRpmsgTest, GuestMessageReachesBoundEndpoint) {
  bring_up();
  seen s;
  ASSERT_TRUE(rpmsg_
                  .try_bind(
                      0x400,
                      [](void *ctx, std::uint32_t src, std::uint32_t dst, reloco::span<const std::byte> p) noexcept {
                        auto *r = static_cast<seen *>(ctx);
                        ++r->calls;
                        r->src = src;
                        r->dst = dst;
                        r->len = p.size();
                        r->first = p.empty() ? std::byte{} : p[0];
                      },
                      &s)
                  .has_value());
  guest_send(0x401, 0x400, 50, 50, std::byte{0x7e});
  EXPECT_EQ(s.calls, 1);
  EXPECT_EQ(s.src, 0x401u);
  EXPECT_EQ(s.dst, 0x400u);
  EXPECT_EQ(s.len, 50u);
  EXPECT_EQ(s.first, std::byte{0x7e});
  EXPECT_EQ(rpmsg_.stats().rx_delivered, 1u);

  guest_send(0x401, 0x400, 0, 0); // empty payload is valid
  EXPECT_EQ(s.calls, 2);
  EXPECT_EQ(s.len, 0u);
}

TEST_F(VirtioRpmsgTest, BadGuestMessagesAreCountedAndDropped) {
  bring_up();
  seen s;
  ASSERT_TRUE(rpmsg_
                  .try_bind(
                      0x400, [](void *c, std::uint32_t, std::uint32_t, reloco::span<const std::byte>) noexcept {
                        ++static_cast<seen *>(c)->calls;
                      },
                      &s)
                  .has_value());
  guest_send(1, 0x999, 8, 8); // nobody home
  EXPECT_EQ(rpmsg_.stats().rx_unrouted, 1u);
  guest_send(1, 0x400, 8, 400); // len lies beyond the buffer
  EXPECT_EQ(rpmsg_.stats().rx_malformed, 1u);
  guest_send(1, 0x400, 8, 0xffff); // ditto, maximal
  EXPECT_EQ(rpmsg_.stats().rx_malformed, 2u);
  EXPECT_EQ(s.calls, 0);

  // A buffer shorter than the header.
  reloco::array<std::byte, 8> tiny{};
  put(0x1000 + 3 * kBufSize, tiny.data(), tiny.size());
  reloco::array<sg_t, 1> out{sg_t{gaddr{kBuf + 0x1000 + 3 * kBufSize}, 8}};
  ASSERT_TRUE(drv_[1]->try_add(reloco::span<const sg_t>(out.data(), 1), reloco::span<const sg_t>(), 9).has_value());
  ASSERT_TRUE(drv_[1]->try_publish().has_value());
  wr(reg::queue_notify, 1);
  EXPECT_EQ(rpmsg_.stats().rx_malformed, 3u);
  auto done = drv_[1]->try_get_used();
  EXPECT_TRUE(done.has_value() && done->has_value()); // still completed
}

TEST_F(VirtioRpmsgTest, OversizeGuestPayloadIsDropped) {
  bring_up();
  seen s;
  ASSERT_TRUE(rpmsg_
                  .try_bind(
                      0x400, [](void *c, std::uint32_t, std::uint32_t, reloco::span<const std::byte>) noexcept {
                        ++static_cast<seen *>(c)->calls;
                      },
                      &s)
                  .has_value());
  guest_send(1, 0x400, 65, 65); // one byte over MaxPayload
  EXPECT_EQ(rpmsg_.stats().rx_oversize, 1u);
  EXPECT_EQ(s.calls, 0);
  guest_send(1, 0x400, 64, 64); // exactly MaxPayload is fine
  EXPECT_EQ(s.calls, 1);
}

TEST_F(VirtioRpmsgTest, HypervisorMessageIsDeliveredIntoGuestBuffer) {
  bring_up();
  post_rx(0);
  reloco::array<std::byte, 5> payload{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}, std::byte{5}};
  ASSERT_TRUE(rpmsg_.try_send(0x400, 0x401, reloco::span<const std::byte>(payload.data(), payload.size())).has_value());
  EXPECT_EQ(rpmsg_.pending(), 1u);
  ASSERT_TRUE(dev_.try_kick(0).has_value());
  EXPECT_EQ(rpmsg_.pending(), 0u);
  EXPECT_TRUE(dev_.irq_asserted());

  auto done = drv_[0]->try_get_used();
  ASSERT_TRUE(done.has_value() && done->has_value());
  EXPECT_EQ((*done)->len, rpmsg::header_size + 5);
  reloco::array<std::byte, 21> got{};
  get(0, got.data(), got.size());
  const auto h = rpmsg::decode_header(reloco::span<const std::byte>(got.data(), got.size()));
  EXPECT_EQ(h.src, 0x400u);
  EXPECT_EQ(h.dst, 0x401u);
  EXPECT_EQ(h.len, 5u);
  EXPECT_EQ(got[16], std::byte{1});
  EXPECT_EQ(got[20], std::byte{5});
  EXPECT_EQ(rpmsg_.stats().tx_delivered, 1u);
}

TEST_F(VirtioRpmsgTest, MessageWaitsForGuestBuffers) {
  bring_up();
  reloco::array<std::byte, 2> p{std::byte{9}, std::byte{8}};
  ASSERT_TRUE(rpmsg_.try_send(1, 2, reloco::span<const std::byte>(p.data(), p.size())).has_value());
  ASSERT_TRUE(dev_.try_kick(0).has_value());
  EXPECT_EQ(rpmsg_.pending(), 1u); // no rx buffer yet
  post_rx(1);
  wr(reg::queue_notify, 0); // guest kicks after posting
  EXPECT_EQ(rpmsg_.pending(), 0u);
  auto done = drv_[0]->try_get_used();
  ASSERT_TRUE(done.has_value() && done->has_value());
  EXPECT_EQ((*done)->len, rpmsg::header_size + 2);
}

TEST_F(VirtioRpmsgTest, StagingQueueBackpressure) {
  bring_up();
  reloco::array<std::byte, 1> p{};
  const reloco::span<const std::byte> ps(p.data(), 1);
  ASSERT_TRUE(rpmsg_.try_send(1, 2, ps).has_value());
  ASSERT_TRUE(rpmsg_.try_send(1, 2, ps).has_value());
  auto r = rpmsg_.try_send(1, 2, ps);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), reloco::error::try_again);
  EXPECT_EQ(rpmsg_.stats().tx_full, 1u);

  reloco::array<std::byte, 65> big{};
  auto b = rpmsg_.try_send(1, 2, reloco::span<const std::byte>(big.data(), big.size()));
  ASSERT_FALSE(b.has_value());
  EXPECT_EQ(b.error(), reloco::error::invalid_argument);
}

TEST_F(VirtioRpmsgTest, SmallGuestBufferDropsMessage) {
  bring_up();
  post_rx(0, 20); // header + 4 only
  reloco::array<std::byte, 8> p{};
  ASSERT_TRUE(rpmsg_.try_send(1, 2, reloco::span<const std::byte>(p.data(), p.size())).has_value());
  ASSERT_TRUE(dev_.try_kick(0).has_value());
  EXPECT_EQ(rpmsg_.stats().tx_lost, 1u);
  EXPECT_EQ(rpmsg_.pending(), 0u);
  auto done = drv_[0]->try_get_used();
  ASSERT_TRUE(done.has_value() && done->has_value());
  EXPECT_EQ((*done)->len, 0u);
}

TEST_F(VirtioRpmsgTest, AnnounceBuildsNameServiceMessage) {
  bring_up();
  post_rx(0);
  ASSERT_TRUE(rpmsg_.try_announce("structo-echo", 0x400).has_value());
  ASSERT_TRUE(dev_.try_kick(0).has_value());
  auto done = drv_[0]->try_get_used();
  ASSERT_TRUE(done.has_value() && done->has_value());
  EXPECT_EQ((*done)->len, rpmsg::header_size + rpmsg::ns_msg_size);
  reloco::array<std::byte, rpmsg::header_size + rpmsg::ns_msg_size> got{};
  get(0, got.data(), got.size());
  const auto h = rpmsg::decode_header(reloco::span<const std::byte>(got.data(), got.size()));
  EXPECT_EQ(h.dst, rpmsg::ns_addr);
  EXPECT_EQ(h.src, 0x400u);
  EXPECT_EQ(h.len, rpmsg::ns_msg_size);
  auto ns = rpmsg::try_decode_ns(reloco::span<const std::byte>(got.data() + rpmsg::header_size, rpmsg::ns_msg_size));
  ASSERT_TRUE(ns.has_value());
  EXPECT_STREQ(ns->name.data(), "structo-echo");
  EXPECT_EQ(ns->addr, 0x400u);
  EXPECT_EQ(ns->flags, rpmsg::ns_create);

  reloco::array<char, 40> longname{};
  for (auto &c : longname)
    c = 'x';
  auto bad = rpmsg_.try_announce(reloco::string_view(longname.data(), 32), 1);
  EXPECT_FALSE(bad.has_value());
  EXPECT_FALSE(rpmsg::try_decode_ns(reloco::span<const std::byte>(got.data(), 10)).has_value());
}

TEST_F(VirtioRpmsgTest, HandlerReplyGoesOutOnNextQueue0Kick) {
  bring_up();
  post_rx(0);
  ASSERT_TRUE(rpmsg_
                  .try_bind(
                      0x400,
                      [](void *ctx, std::uint32_t src, std::uint32_t dst, reloco::span<const std::byte> p) noexcept {
                        (void)static_cast<rpmsg_t *>(ctx)->try_send(dst, src, p); // echo
                      },
                      &rpmsg_)
                  .has_value());
  guest_send(0x401, 0x400, 6, 6, std::byte{0x33});
  EXPECT_EQ(rpmsg_.pending(), 1u);
  ASSERT_TRUE(dev_.try_kick(0).has_value());
  auto done = drv_[0]->try_get_used();
  ASSERT_TRUE(done.has_value() && done->has_value());
  EXPECT_EQ((*done)->len, rpmsg::header_size + 6);
  reloco::array<std::byte, 22> got{};
  get(0, got.data(), got.size());
  const auto h = rpmsg::decode_header(reloco::span<const std::byte>(got.data(), got.size()));
  EXPECT_EQ(h.src, 0x400u);
  EXPECT_EQ(h.dst, 0x401u);
  EXPECT_EQ(got[21], std::byte{0x33});
}

TEST_F(VirtioRpmsgTest, EndpointTableRules) {
  auto fn = [](void *, std::uint32_t, std::uint32_t, reloco::span<const std::byte>) noexcept {};
  EXPECT_EQ(rpmsg_.try_bind(rpmsg::addr_any, fn).error(), reloco::error::invalid_argument);
  EXPECT_EQ(rpmsg_.try_bind(1, nullptr).error(), reloco::error::invalid_argument);
  ASSERT_TRUE(rpmsg_.try_bind(1, fn).has_value());
  EXPECT_EQ(rpmsg_.try_bind(1, fn).error(), reloco::error::already_exists);
  ASSERT_TRUE(rpmsg_.try_bind(2, fn).has_value());
  EXPECT_EQ(rpmsg_.try_bind(3, fn).error(), reloco::error::capacity_exceeded);
  ASSERT_TRUE(rpmsg_.try_unbind(1).has_value());
  EXPECT_EQ(rpmsg_.try_unbind(1).error(), reloco::error::not_found);
  EXPECT_TRUE(rpmsg_.try_bind(3, fn).has_value());
}

} // namespace
