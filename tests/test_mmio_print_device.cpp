// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/mmio_print_device.hpp>

#include <string>

using reloco::error;
using reloco::span;
using structo::hw::console_color;
using structo::hw::console_ref;
using structo::hypervisor::mmio_device_ref;
using structo::hypervisor::mmio_print_device;

namespace {

// A minimal console_traits backend that just records every printed character into a string, ignoring
// color/position -- this test only cares about which bytes mmio_print_device forwards, not rendering.
constexpr std::size_t kCols = 80;
constexpr std::size_t kRows = 25;

struct recording_console {
  std::string printed;
};

} // namespace

template <> struct structo::hw::console_traits<recording_console> {
  static void put_cell(recording_console &b, std::size_t, std::size_t, char ch, console_color, console_color) noexcept {
    b.printed.push_back(ch);
  }
  static std::size_t columns(const recording_console &) noexcept { return kCols; }
  static std::size_t rows(const recording_console &) noexcept { return kRows; }
};

namespace {

class MmioPrintDeviceTest : public ::testing::Test {
protected:
  recording_console console_;
  console_ref sink_{console_};
  mmio_print_device device_{sink_};
  mmio_device_ref ref_{device_};
};

} // namespace

TEST_F(MmioPrintDeviceTest, SizeIsExactlyOneByte) { EXPECT_EQ(ref_.size(), 1u); }

TEST_F(MmioPrintDeviceTest, WritingEachByteOfAStringPrintsItToTheSink) {
  const std::string message = "Hi!\n";
  for (char ch : message) {
    const std::byte b = static_cast<std::byte>(ch);
    ASSERT_TRUE(ref_.try_write(0, span<const std::byte>(&b, 1)).has_value());
  }
  // console_ref::put() translates '\n' into a carriage-return + line-feed rather than a literal glyph, so
  // only the printable characters actually reach put_cell().
  EXPECT_EQ(console_.printed, "Hi!");
}

TEST_F(MmioPrintDeviceTest, ReadingAlwaysReturnsTheProbeMagic) {
  std::byte readback{0};
  auto read = ref_.try_read(0, span<std::byte>(&readback, 1));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(readback, mmio_print_device::probe_magic);

  // Still the probe magic after a write -- there is no real register state to read back.
  const std::byte written{'X'};
  ASSERT_TRUE(ref_.try_write(0, span<const std::byte>(&written, 1)).has_value());
  read = ref_.try_read(0, span<std::byte>(&readback, 1));
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(readback, mmio_print_device::probe_magic);
}

TEST_F(MmioPrintDeviceTest, OutOfRangeAccessIsRejectedBeforeReachingTheDevice) {
  std::byte buf[2] = {};
  auto write = ref_.try_write(0, span<const std::byte>(buf, 2)); // wider than the 1-byte window
  ASSERT_FALSE(write.has_value());
  EXPECT_EQ(write.error(), error::out_of_range);
}

TEST_F(MmioPrintDeviceTest, IsAvailableReflectsWhetherTheSinkIsBound) {
  EXPECT_TRUE(ref_.is_available());

  mmio_print_device unbound_device{console_ref{}};
  mmio_device_ref unbound_ref(unbound_device);
  EXPECT_FALSE(unbound_ref.is_available());

  // Writing through an unbound sink is still safe -- console_ref::put() silently no-ops -- it just prints
  // nowhere.
  const std::byte b{'Z'};
  EXPECT_TRUE(unbound_ref.try_write(0, span<const std::byte>(&b, 1)).has_value());
}
