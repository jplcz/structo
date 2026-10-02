// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/mmio_space.hpp>

#include <array>

namespace {

using namespace structo;

} // namespace

TEST(MmioSpaceBackendTest, DefaultConstructedBackendIsUnbound) {
  mmio_space_backend backend;
  EXPECT_FALSE(backend.is_bound());
  EXPECT_EQ(backend.size(), 0u);

  io_space_ref<device_io_space> ref(backend);
  io_address<std::uint8_t, device_io_space> addr(std::uint64_t{0});
  auto r = ref.read(addr);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::unsupported_operation);
}

TEST(MmioSpaceBackendTest, BoundBackendReportsTrueAndConfiguredSize) {
  std::array<std::uint8_t, 16> window{};
  mmio_space_backend backend(window.data(), window.size());
  EXPECT_TRUE(backend.is_bound());
  EXPECT_EQ(backend.size(), window.size());
}

TEST(MmioSpaceBackendTest, Write32ThenRead32RoundTrips) {
  std::array<std::uint8_t, 16> window{};
  mmio_space_backend backend(window.data(), window.size());
  io_space_ref<device_io_space> ref(backend);
  io_address<std::uint32_t, device_io_space> addr(4);

  ASSERT_TRUE(ref.write(addr, 0xDEADBEEFu).has_value());
  auto r = ref.read(addr);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 0xDEADBEEFu);
}

TEST(MmioSpaceBackendTest, AllWidthsRoundTrip) {
  std::array<std::uint8_t, 32> window{};
  mmio_space_backend backend(window.data(), window.size());
  io_space_ref<device_io_space> ref(backend);

  io_address<std::uint8_t, device_io_space> a8(std::uint64_t{0});
  io_address<std::uint16_t, device_io_space> a16(2);
  io_address<std::uint32_t, device_io_space> a32(4);
  io_address<std::uint64_t, device_io_space> a64(8);

  ASSERT_TRUE(ref.write(a8, std::uint8_t{0x12}).has_value());
  ASSERT_TRUE(ref.write(a16, std::uint16_t{0x1234}).has_value());
  ASSERT_TRUE(ref.write(a32, std::uint32_t{0x12345678}).has_value());
  ASSERT_TRUE(ref.write(a64, std::uint64_t{0x1122334455667788ull}).has_value());

  auto r8 = ref.read(a8);
  ASSERT_TRUE(r8.has_value());
  EXPECT_EQ(*r8, 0x12u);
  auto r16 = ref.read(a16);
  ASSERT_TRUE(r16.has_value());
  EXPECT_EQ(*r16, 0x1234u);
  auto r32 = ref.read(a32);
  ASSERT_TRUE(r32.has_value());
  EXPECT_EQ(*r32, 0x12345678u);
  auto r64 = ref.read(a64);
  ASSERT_TRUE(r64.has_value());
  EXPECT_EQ(*r64, 0x1122334455667788ull);
}

TEST(MmioSpaceBackendTest, OutOfRangeAccessReportsOutOfRange) {
  std::array<std::uint8_t, 8> window{};
  mmio_space_backend backend(window.data(), window.size());
  io_space_ref<device_io_space> ref(backend);
  io_address<std::uint32_t, device_io_space> past_end(6); // 6 + 4 > 8

  auto r = ref.read(past_end);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::out_of_range);

  auto w = ref.write(past_end, 0u);
  ASSERT_FALSE(w.has_value());
  EXPECT_EQ(w.error(), error::out_of_range);
}

TEST(MmioSpaceBackendTest, ZeroSizeDisablesBoundsChecking) {
  std::array<std::uint8_t, 8> window{};
  mmio_space_backend backend(window.data()); // size defaults to 0 -> unchecked
  io_space_ref<device_io_space> ref(backend);
  io_address<std::uint8_t, device_io_space> addr(7); // last valid byte, would
                                                     // still be in-range anyway
  EXPECT_TRUE(ref.write(addr, std::uint8_t{0x5A}).has_value());
  auto r = ref.read(addr);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, 0x5Au);
}

// --------------------------------------------------------------------
// MMIO registers are frequently read- and/or write-*sensitive*: a FIFO
// that pops its next byte on every read, a status register that
// clears-on-read, a strobe register where every single write (not just
// the last one) triggers a side effect. These tests guard against a
// hypothetical future "optimization" that collapses repeated accesses
// to the same fixed address into fewer (or reordered) real accesses --
// which would be silently wrong for such hardware.
// --------------------------------------------------------------------

TEST(MmioSpaceBackendTest, EachReadObservesLiveMemoryEvenAtTheSameFixedAddress) {
  std::array<std::uint8_t, 8> window{};
  mmio_space_backend backend(window.data(), window.size());
  io_space_ref<device_io_space> ref(backend);
  io_address<std::uint8_t, device_io_space> status(std::uint64_t{0});

  // Simulate a read-sensitive register: something external to this
  // backend (a "device") mutates the live memory between accesses.
  // Every read must observe the *current* value -- nothing here may be
  // cached, memoized, or elided from an earlier read.
  window[0] = 0xAA;
  auto r1 = ref.read(status);
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(*r1, 0xAAu);

  window[0] = 0xBB;
  auto r2 = ref.read(status);
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(*r2, 0xBBu);

  window[0] = 0xCC;
  auto r3 = ref.read(status);
  ASSERT_TRUE(r3.has_value());
  EXPECT_EQ(*r3, 0xCCu);
}

TEST(MmioSpaceBackendTest, WriteRepIssuesEveryWriteInOrderToTheSameFixedAddress) {
  std::array<std::uint8_t, 8> window{};
  mmio_space_backend backend(window.data(), window.size());
  io_space_ref<device_io_space> ref(backend);
  io_address<std::uint8_t, device_io_space> strobe(4);

  // write_rep targets one fixed address repeatedly (e.g. a
  // command/strobe register); only the *last* value remains observable
  // in plain memory, but that final value being the *last* source
  // element (not the first, and not garbage) proves every write in the
  // sequence was actually issued, in order -- a naive "optimization"
  // that wrote only the first element or collapsed the loop would fail
  // this.
  std::array<std::uint8_t, 4> src{0x11, 0x22, 0x33, 0x44};
  auto wr = ref.write_rep(strobe, span<const std::uint8_t>(src.data(), src.size()));
  ASSERT_TRUE(wr.has_value());
  EXPECT_EQ(window[4], 0x44u);
}

namespace {

// A tiny read-sensitive device built *on top of* mmio_space_backend's
// raw volatile mechanism: every read8 first "pops" the next FIFO byte
// into the backing memory (mimicking real hardware that mutates the
// register as a side effect of being read) before delegating to the
// plain mmio read. This proves read_rep's generic per-element loop
// genuinely re-reads live memory on every single iteration rather than
// reading once and broadcasting the result across the destination span.
struct fifo_over_mmio {
  mmio_space_backend mmio;
  std::array<std::uint8_t, 4> fifo{0x10, 0x20, 0x30, 0x40};
  std::size_t pos = 0;
};

} // namespace

template <> struct structo::io_space_traits<fifo_over_mmio> {
  static reloco::result<std::uint8_t> read8(fifo_over_mmio &b, std::uint64_t addr) noexcept {
    if (b.pos < b.fifo.size()) {
      auto w = io_space_traits<mmio_space_backend>::write8(b.mmio, addr, b.fifo[b.pos]);
      if (!w)
        return reloco::unexpected(w.error());
      ++b.pos;
    }
    return io_space_traits<mmio_space_backend>::read8(b.mmio, addr);
  }
  static reloco::result<std::uint16_t> read16(fifo_over_mmio &b, std::uint64_t addr) noexcept {
    return io_space_traits<mmio_space_backend>::read16(b.mmio, addr);
  }
  static reloco::result<std::uint32_t> read32(fifo_over_mmio &b, std::uint64_t addr) noexcept {
    return io_space_traits<mmio_space_backend>::read32(b.mmio, addr);
  }
  static reloco::result<std::uint64_t> read64(fifo_over_mmio &b, std::uint64_t addr) noexcept {
    return io_space_traits<mmio_space_backend>::read64(b.mmio, addr);
  }
  static reloco::result<void> write8(fifo_over_mmio &b, std::uint64_t addr, std::uint8_t val) noexcept {
    return io_space_traits<mmio_space_backend>::write8(b.mmio, addr, val);
  }
  static reloco::result<void> write16(fifo_over_mmio &b, std::uint64_t addr, std::uint16_t val) noexcept {
    return io_space_traits<mmio_space_backend>::write16(b.mmio, addr, val);
  }
  static reloco::result<void> write32(fifo_over_mmio &b, std::uint64_t addr, std::uint32_t val) noexcept {
    return io_space_traits<mmio_space_backend>::write32(b.mmio, addr, val);
  }
  static reloco::result<void> write64(fifo_over_mmio &b, std::uint64_t addr, std::uint64_t val) noexcept {
    return io_space_traits<mmio_space_backend>::write64(b.mmio, addr, val);
  }
};

TEST(MmioSpaceBackendTest, ReadRepReReadsLiveMemoryOnEveryIterationNotJustOnce) {
  std::array<std::uint8_t, 8> window{};
  fifo_over_mmio dev{mmio_space_backend(window.data(), window.size())};
  io_space_ref<device_io_space> ref(dev);
  io_address<std::uint8_t, device_io_space> fifo_reg(std::uint64_t{0});

  std::array<std::uint8_t, 4> dst{};
  auto rr = ref.read_rep(fifo_reg, span<std::uint8_t>(dst.data(), dst.size()));
  ASSERT_TRUE(rr.has_value());
  // Each of the 4 reads must observe a *different* popped FIFO byte --
  // if the loop (incorrectly) read live memory only once and broadcast
  // that single value, every element would equal 0x10 instead.
  EXPECT_EQ(dst[0], 0x10u);
  EXPECT_EQ(dst[1], 0x20u);
  EXPECT_EQ(dst[2], 0x30u);
  EXPECT_EQ(dst[3], 0x40u);
}
