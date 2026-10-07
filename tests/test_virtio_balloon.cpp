// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "virtio_mmio_fixture.hpp"

#include <structo/virtio/virtio_balloon.hpp>

namespace {

using namespace virtio_test;

struct recording_host {
  struct call {
    std::uint64_t addr;
    std::uint32_t count;
    bool inflate;
  };
  reloco::result<void> try_reclaim(gaddr first, std::uint32_t count) noexcept { return record(first, count, true); }
  reloco::result<void> try_restore(gaddr first, std::uint32_t count) noexcept { return record(first, count, false); }
  reloco::result<void> record(gaddr first, std::uint32_t count, bool inflate) noexcept {
    if (fail)
      return reloco::unexpected(reloco::error::io_error);
    if (n < calls.size())
      calls[n++] = call{first.value, count, inflate};
    return {};
  }
  reloco::array<call, 16> calls{};
  std::size_t n = 0;
  bool fail = false;
};

using balloon_t = virtio_balloon_function<guest_space, recording_host>;

struct host_holder {
  recording_host host;
};

class VirtioBalloonTest : private host_holder, public mmio_fixture<balloon_t, 2> {
protected:
  VirtioBalloonTest() : mmio_fixture<balloon_t, 2>(host, std::uint64_t{1000}) {}
  recording_host &hv() { return host; }

  void send_pfns(std::uint32_t q, std::initializer_list<std::uint32_t> pfns, std::uint64_t off = 0) {
    std::size_t i = 0;
    for (auto p : pfns) {
      reloco::array<std::byte, 4> b{};
      store_le<std::uint32_t>(b.as_span(), p);
      put(off + 4 * i++, b.data(), 4);
    }
    post_out(q, off, 4 * pfns.size());
    kick(q);
    EXPECT_EQ(reap(q), 0);
  }
};

TEST_F(VirtioBalloonTest, IdentityAndConfig) {
  EXPECT_EQ(rd(reg::device_id), 5u);
  EXPECT_EQ(dev_.size(), reg::config + balloon::config_size);
  fn_.set_target(256);
  EXPECT_EQ(rd(reg::config), 256u);
  EXPECT_EQ(rd(reg::config + 4), 0u);
}

TEST_F(VirtioBalloonTest, InflateCoalescesRunsAndReports) {
  bring_up();
  send_pfns(0, {10, 11, 12, 20});
  ASSERT_EQ(hv().n, 2u);
  EXPECT_EQ(hv().calls[0].addr, std::uint64_t{10} << 12);
  EXPECT_EQ(hv().calls[0].count, 3u);
  EXPECT_TRUE(hv().calls[0].inflate);
  EXPECT_EQ(hv().calls[1].addr, std::uint64_t{20} << 12);
  EXPECT_EQ(hv().calls[1].count, 1u);
  EXPECT_EQ(fn_.inflated(), 4u);
  EXPECT_EQ(rd(reg::config + 4), 4u); // actual
}

TEST_F(VirtioBalloonTest, DeflateRestoresPages) {
  bring_up();
  send_pfns(0, {10, 11, 12});
  send_pfns(1, {11, 12}, 0x100);
  ASSERT_EQ(hv().n, 2u);
  EXPECT_FALSE(hv().calls[1].inflate);
  EXPECT_EQ(hv().calls[1].addr, std::uint64_t{11} << 12);
  EXPECT_EQ(hv().calls[1].count, 2u);
  EXPECT_EQ(fn_.inflated(), 1u);
}

TEST_F(VirtioBalloonTest, OutOfRangeFramesAreDropped) {
  bring_up();
  send_pfns(0, {5, 1000, 6, 0xffffffffu});
  EXPECT_EQ(fn_.stats().bad_messages, 2u);
  EXPECT_EQ(fn_.inflated(), 2u); // 5 and 6 are separate runs, both valid
  ASSERT_EQ(hv().n, 2u);
  EXPECT_EQ(hv().calls[0].count, 1u);
  EXPECT_EQ(hv().calls[1].count, 1u);
}

TEST_F(VirtioBalloonTest, MalformedMessagesAreCounted) {
  bring_up();
  post_out(0, 0, 6); // not a multiple of 4
  kick(0);
  EXPECT_EQ(reap(0), 0);
  EXPECT_EQ(fn_.stats().bad_messages, 1u);
  EXPECT_EQ(hv().n, 0u);
  post_out(0, 0, balloon::max_pfns_per_message * 4 + 4); // too long
  kick(0);
  EXPECT_EQ(reap(0), 0);
  EXPECT_EQ(fn_.stats().bad_messages, 2u);
}

TEST_F(VirtioBalloonTest, DeflatingMoreThanInflatedIsClamped) {
  bring_up();
  send_pfns(0, {1, 2});
  send_pfns(1, {1, 2, 3, 4}, 0x100);
  EXPECT_EQ(fn_.inflated(), 0u);
  EXPECT_EQ(fn_.stats().bad_messages, 1u);
  EXPECT_EQ(hv().calls[1].count, 2u); // only what was inflated is restored
}

TEST_F(VirtioBalloonTest, HostFailureDoesNotCountPages) {
  bring_up();
  hv().fail = true;
  send_pfns(0, {7, 8});
  EXPECT_EQ(fn_.inflated(), 0u);
  EXPECT_EQ(fn_.stats().host_errors, 1u);
}

} // namespace
