// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/io_space_ref.hpp>

#include <array>
#include <cstring>

namespace {

using namespace structo;

// --------------------------------------------------------------------
// A fake MMIO backend: a flat in-memory byte array standing in for a
// real volatile register window. Implements only the 8 mandatory
// fixed-width functions, so its "rep" string I/O is always serviced by
// io_space_ref's generic single-access loop.
// --------------------------------------------------------------------
struct fake_mmio {
  std::array<std::uint8_t, 256> mem{};
};

} // namespace

template <> struct structo::io_space_traits<fake_mmio> {
  static reloco::result<std::uint8_t> read8(fake_mmio &b, std::uint64_t addr) noexcept {
    if (addr + 1 > b.mem.size())
      return reloco::unexpected(reloco::error::out_of_range);
    return b.mem[addr];
  }
  static reloco::result<std::uint16_t> read16(fake_mmio &b, std::uint64_t addr) noexcept {
    if (addr + 2 > b.mem.size())
      return reloco::unexpected(reloco::error::out_of_range);
    std::uint16_t v;
    std::memcpy(&v, &b.mem[addr], 2);
    return v;
  }
  static reloco::result<std::uint32_t> read32(fake_mmio &b, std::uint64_t addr) noexcept {
    if (addr + 4 > b.mem.size())
      return reloco::unexpected(reloco::error::out_of_range);
    std::uint32_t v;
    std::memcpy(&v, &b.mem[addr], 4);
    return v;
  }
  static reloco::result<std::uint64_t> read64(fake_mmio &b, std::uint64_t addr) noexcept {
    if (addr + 8 > b.mem.size())
      return reloco::unexpected(reloco::error::out_of_range);
    std::uint64_t v;
    std::memcpy(&v, &b.mem[addr], 8);
    return v;
  }
  static reloco::result<void> write8(fake_mmio &b, std::uint64_t addr, std::uint8_t val) noexcept {
    if (addr + 1 > b.mem.size())
      return reloco::unexpected(reloco::error::out_of_range);
    b.mem[addr] = val;
    return {};
  }
  static reloco::result<void> write16(fake_mmio &b, std::uint64_t addr, std::uint16_t val) noexcept {
    if (addr + 2 > b.mem.size())
      return reloco::unexpected(reloco::error::out_of_range);
    std::memcpy(&b.mem[addr], &val, 2);
    return {};
  }
  static reloco::result<void> write32(fake_mmio &b, std::uint64_t addr, std::uint32_t val) noexcept {
    if (addr + 4 > b.mem.size())
      return reloco::unexpected(reloco::error::out_of_range);
    std::memcpy(&b.mem[addr], &val, 4);
    return {};
  }
  static reloco::result<void> write64(fake_mmio &b, std::uint64_t addr, std::uint64_t val) noexcept {
    if (addr + 8 > b.mem.size())
      return reloco::unexpected(reloco::error::out_of_range);
    std::memcpy(&b.mem[addr], &val, 8);
    return {};
  }
};

namespace {

// --------------------------------------------------------------------
// A fake port backend with a native read_rep8 fast path (exercising the
// optional-trait detection branch), and only 8-bit reads supported
// (exercising per-width error::unsupported_operation).
// --------------------------------------------------------------------
struct fake_port {
  std::array<std::uint8_t, 4> fifo{0xAA, 0xBB, 0xCC, 0xDD};
  std::size_t pos = 0;
  int write_count = 0;
};

} // namespace

template <> struct structo::io_space_traits<fake_port> {
  static reloco::result<std::uint8_t> read8(fake_port &b, std::uint64_t) noexcept {
    auto v = b.fifo[b.pos % b.fifo.size()];
    ++b.pos;
    return v;
  }
  static reloco::result<std::uint16_t> read16(fake_port &, std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }
  static reloco::result<std::uint32_t> read32(fake_port &, std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }
  static reloco::result<std::uint64_t> read64(fake_port &, std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }
  static reloco::result<void> write8(fake_port &b, std::uint64_t, std::uint8_t) noexcept {
    ++b.write_count;
    return {};
  }
  static reloco::result<void> write16(fake_port &, std::uint64_t, std::uint16_t) noexcept { return {}; }
  static reloco::result<void> write32(fake_port &, std::uint64_t, std::uint32_t) noexcept { return {}; }
  static reloco::result<void> write64(fake_port &, std::uint64_t, std::uint64_t) noexcept { return {}; }

  // Native rep fast path for 8-bit only; io_space_ref must prefer this
  // over its own generic loop.
  static reloco::result<void> read_rep8(fake_port &b, std::uint64_t addr, std::uint8_t *dst,
                                        std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) {
      auto r = read8(b, addr);
      dst[i] = r.value();
    }
    return {};
  }
};

namespace {

TEST(IoSpaceRefTest, UnboundRefFailsEveryOperation) {
  io_space_ref<device_io_space> unbound;
  EXPECT_FALSE(static_cast<bool>(unbound));

  io_address<std::uint32_t, device_io_space> addr(0x10);
  auto r = unbound.read(addr);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::unsupported_operation);

  auto w = unbound.write(addr, std::uint32_t(1));
  ASSERT_FALSE(w.has_value());
  EXPECT_EQ(w.error(), error::unsupported_operation);
}

TEST(IoSpaceRefTest, BoundRefReportsTrue) {
  fake_mmio mmio;
  io_space_ref<device_io_space> ref(mmio);
  EXPECT_TRUE(static_cast<bool>(ref));
}

TEST(IoSpaceRefTest, Write32ThenRead32RoundTrips) {
  fake_mmio mmio;
  io_space_ref<device_io_space> ref(mmio);
  io_address<std::uint32_t, device_io_space> addr(0x10);

  auto w = ref.write(addr, std::uint32_t(0xDEADBEEFu));
  ASSERT_TRUE(w.has_value());

  auto r = ref.read(addr);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), 0xDEADBEEFu);
}

TEST(IoSpaceRefTest, Write8ThenRead8RoundTrips) {
  fake_mmio mmio;
  io_space_ref<device_io_space> ref(mmio);
  io_address<std::uint8_t, device_io_space> addr(0x40);

  auto w = ref.write(addr, std::uint8_t(0x7A));
  ASSERT_TRUE(w.has_value());

  auto r = ref.read(addr);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r.value(), 0x7Au);
}

TEST(IoSpaceRefTest, Write16AndWrite64RoundTrip) {
  fake_mmio mmio;
  io_space_ref<device_io_space> ref(mmio);

  io_address<std::uint16_t, device_io_space> addr16(0x20);
  ASSERT_TRUE(ref.write(addr16, std::uint16_t(0xBEEF)).has_value());
  auto r16 = ref.read(addr16);
  ASSERT_TRUE(r16.has_value());
  EXPECT_EQ(r16.value(), 0xBEEFu);

  io_address<std::uint64_t, device_io_space> addr64(0x30);
  ASSERT_TRUE(ref.write(addr64, std::uint64_t(0x0123456789ABCDEFull)).has_value());
  auto r64 = ref.read(addr64);
  ASSERT_TRUE(r64.has_value());
  EXPECT_EQ(r64.value(), 0x0123456789ABCDEFull);
}

TEST(IoSpaceRefTest, BackendOutOfRangeErrorPropagates) {
  fake_mmio mmio;
  io_space_ref<device_io_space> ref(mmio);
  io_address<std::uint32_t, device_io_space> past_end(253);

  auto r = ref.read(past_end);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::out_of_range);
}

TEST(IoSpaceRefTest, ReadRepSynthesizedGenericallyWhenBackendLacksFastPath) {
  fake_mmio mmio;
  io_space_ref<device_io_space> ref(mmio);
  io_address<std::uint8_t, device_io_space> addr(0x80);

  mmio.mem[0x80] = 0x42;

  std::array<std::uint8_t, 3> dst{};
  auto rr = ref.read_rep(addr, span<std::uint8_t>(dst.data(), dst.size()));
  ASSERT_TRUE(rr.has_value());
  EXPECT_EQ(dst[0], 0x42u);
  EXPECT_EQ(dst[1], 0x42u);
  EXPECT_EQ(dst[2], 0x42u);
}

TEST(IoSpaceRefTest, WriteRepSynthesizedGenericallyWhenBackendLacksFastPath) {
  fake_mmio mmio;
  io_space_ref<device_io_space> ref(mmio);
  io_address<std::uint8_t, device_io_space> addr(0x90);

  std::array<std::uint8_t, 3> src{1, 2, 3};
  auto wr = ref.write_rep(addr, span<const std::uint8_t>(src.data(), src.size()));
  ASSERT_TRUE(wr.has_value());
  // write_rep always targets the *same* fixed address, so only the last
  // write ends up observable there.
  EXPECT_EQ(mmio.mem[0x90], 3u);
}

TEST(IoSpaceRefTest, ReadRepUsesNativeFastPathWhenAvailable) {
  fake_port port;
  io_space_ref<port_io_space> ref(port);
  io_address<std::uint8_t, port_io_space> addr(0x3F8);

  std::array<std::uint8_t, 4> dst{};
  auto rr = ref.read_rep(addr, span<std::uint8_t>(dst.data(), dst.size()));
  ASSERT_TRUE(rr.has_value());
  EXPECT_EQ(dst[0], 0xAAu);
  EXPECT_EQ(dst[1], 0xBBu);
  EXPECT_EQ(dst[2], 0xCCu);
  EXPECT_EQ(dst[3], 0xDDu);
}

TEST(IoSpaceRefTest, UnsupportedWidthReportsUnsupportedOperation) {
  fake_port port;
  io_space_ref<port_io_space> ref(port);
  io_address<std::uint32_t, port_io_space> addr(0x3F8);

  auto r = ref.read(addr);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::unsupported_operation);
}

TEST(IoSpaceRefTest, WriteRoutesThroughBackendWriteCounter) {
  fake_port port;
  io_space_ref<port_io_space> ref(port);
  io_address<std::uint8_t, port_io_space> addr(0x3F9);

  ASSERT_TRUE(ref.write(addr, std::uint8_t(1)).has_value());
  ASSERT_TRUE(ref.write(addr, std::uint8_t(2)).has_value());
  EXPECT_EQ(port.write_count, 2);
}

TEST(IoSpaceRefTest, DistinctSpaceTagsAreDistinctTypes) {
  static_assert(!std::is_same_v<io_space_ref<device_io_space>, io_space_ref<port_io_space>>,
                "io_space_ref must be distinct per SpaceTag");
}

TEST(IoSpaceRefTest, DefaultSpaceTagIsDefaultIoSpace) {
  static_assert(std::is_same_v<io_space_ref<>, io_space_ref<default_io_space>>,
                "io_space_ref<> must default to default_io_space");
}

} // namespace
