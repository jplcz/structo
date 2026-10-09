// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/mmio_device_ref.hpp>

#include <cstring>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using reloco::error;
using reloco::result;
using reloco::span;
using reloco::unexpected;
using structo::hypervisor::mmio_device_ref;

namespace {

// A fully scriptable fake MMIO device: a flat byte array plus toggleable read-only/availability flags and
// a reset counter, mirroring test_block_device_ref.cpp's fake backend shape.
struct fake_mmio_device {
  std::byte storage[16] = {};
  bool read_only = false;
  bool available = true;
  int reset_count = 0;
  result<void> next_read_error = {};
};

} // namespace

template <> struct structo::hypervisor::mmio_device_traits<fake_mmio_device> {
  static std::size_t size(fake_mmio_device &d) noexcept { return sizeof(d.storage); }

  static result<void> try_read(fake_mmio_device &d, std::uint64_t offset, span<std::byte> dst) noexcept {
    if (!d.next_read_error)
      return unexpected(d.next_read_error.error());
    std::memcpy(dst.data(), d.storage + offset, dst.size());
    return {};
  }

  static result<void> try_write(fake_mmio_device &d, std::uint64_t offset, span<const std::byte> src) noexcept {
    std::memcpy(d.storage + offset, src.data(), src.size());
    return {};
  }

  static bool is_read_only(fake_mmio_device &d) noexcept { return d.read_only; }
  static bool is_available(fake_mmio_device &d) noexcept { return d.available; }

  static result<void> try_reset(fake_mmio_device &d) noexcept {
    ++d.reset_count;
    std::memset(d.storage, 0, sizeof(d.storage));
    return {};
  }
};

namespace {

// A minimal read-only-by-construction backend: no try_write at all, exercising the "no write support
// whatsoever" path distinct from the runtime is_read_only() probe above.
struct fake_rom_device {
  std::byte storage[8] = {};
};

} // namespace

template <> struct structo::hypervisor::mmio_device_traits<fake_rom_device> {
  static std::size_t size(fake_rom_device &d) noexcept { return sizeof(d.storage); }

  static result<void> try_read(fake_rom_device &d, std::uint64_t offset, span<std::byte> dst) noexcept {
    std::memcpy(dst.data(), d.storage + offset, dst.size());
    return {};
  }
};

class MmioDeviceRefTest : public ::testing::Test {
protected:
  fake_mmio_device dev_;
  mmio_device_ref ref_{dev_};
};

TEST(MmioDeviceRefUnboundTest, EveryOperationFailsWithUnsupportedOperationWhenUnbound) {
  mmio_device_ref ref;
  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_EQ(ref.size(), 0u);
  EXPECT_FALSE(ref.is_available());
  EXPECT_TRUE(ref.is_read_only());

  std::byte buf[4] = {};
  auto read = ref.try_read(0, span<std::byte>(buf, 4));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), error::unsupported_operation);

  auto write = ref.try_write(0, span<const std::byte>(buf, 4));
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(write.error(), error::unsupported_operation);

  auto reset = ref.try_reset();
  ASSERT_FALSE(reset.has_value());
  EXPECT_EQ(reset.error(), error::unsupported_operation);
}

TEST_F(MmioDeviceRefTest, SizeReportsBackendWindow) { EXPECT_EQ(ref_.size(), 16u); }

TEST_F(MmioDeviceRefTest, WriteThenReadRoundTrips) {
  const std::uint32_t value = 0xDEADBEEFu;
  ASSERT_TRUE(ref_.try_write(4, span<const std::byte>(reinterpret_cast<const std::byte *>(&value), 4)).has_value());

  std::uint32_t readback = 0;
  ASSERT_TRUE(ref_.try_read(4, span<std::byte>(reinterpret_cast<std::byte *>(&readback), 4)).has_value());
  EXPECT_EQ(readback, value);
}

TEST_F(MmioDeviceRefTest, ReadPastWindowFailsWithOutOfRange) {
  std::byte buf[4] = {};
  auto read = ref_.try_read(14, span<std::byte>(buf, 4)); // [14, 18) overruns the 16-byte window
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), error::out_of_range);
}

TEST_F(MmioDeviceRefTest, WritePastWindowFailsWithOutOfRange) {
  std::byte buf[4] = {};
  auto write = ref_.try_write(16, span<const std::byte>(buf, 1)); // offset == size() with nonzero length
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(write.error(), error::out_of_range);
}

TEST_F(MmioDeviceRefTest, EmptyAccessFailsWithInvalidArgument) {
  auto read = ref_.try_read(0, span<std::byte>());
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), error::invalid_argument);

  auto write = ref_.try_write(0, span<const std::byte>());
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(write.error(), error::invalid_argument);
}

TEST_F(MmioDeviceRefTest, RuntimeReadOnlyFlagRejectsWritesWithPermissionDenied) {
  dev_.read_only = true;
  EXPECT_TRUE(ref_.is_read_only());

  std::byte buf[4] = {};
  auto write = ref_.try_write(0, span<const std::byte>(buf, 4));
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(write.error(), error::permission_denied);
}

TEST_F(MmioDeviceRefTest, IsAvailableReflectsBackend) {
  EXPECT_TRUE(ref_.is_available());
  dev_.available = false;
  EXPECT_FALSE(ref_.is_available());
}

TEST_F(MmioDeviceRefTest, ReadFailurePropagatesFromBackend) {
  dev_.next_read_error = unexpected(error::io_error);
  std::byte buf[4] = {};
  auto read = ref_.try_read(0, span<std::byte>(buf, 4));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), error::io_error);
}

TEST_F(MmioDeviceRefTest, TryResetClearsBackendStateAndCounts) {
  const std::uint32_t value = 0x11223344u;
  ASSERT_TRUE(ref_.try_write(0, span<const std::byte>(reinterpret_cast<const std::byte *>(&value), 4)).has_value());

  ASSERT_TRUE(ref_.try_reset().has_value());
  EXPECT_EQ(dev_.reset_count, 1);

  std::uint32_t readback = 0xFFFFFFFFu;
  ASSERT_TRUE(ref_.try_read(0, span<std::byte>(reinterpret_cast<std::byte *>(&readback), 4)).has_value());
  EXPECT_EQ(readback, 0u);
}

TEST(MmioDeviceRefRomTest, NoWriteTraitMeansHardWiredReadOnly) {
  fake_rom_device dev;
  mmio_device_ref ref(dev);
  EXPECT_TRUE(ref.is_read_only());

  std::byte buf[4] = {};
  auto write = ref.try_write(0, span<const std::byte>(buf, 4));
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(write.error(), error::unsupported_operation);

  // try_reset is absent entirely too -- succeeds trivially.
  EXPECT_TRUE(ref.try_reset().has_value());
}

RELOCO_END_UNSAFE_BUFFER_USAGE