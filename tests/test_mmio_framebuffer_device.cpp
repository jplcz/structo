// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/mmio_framebuffer_device.hpp>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using structo::hw::rgb_color;
using structo::hw::xrgb8888;
using structo::hypervisor::mmio_device_ref;
using structo::hypervisor::mmio_framebuffer_device;
using structo::hypervisor::pixel_format_id;
using reloco::error;
using reloco::span;

namespace {

using device = mmio_framebuffer_device<xrgb8888>;

std::uint32_t read_u32(mmio_device_ref &ref, std::uint64_t offset) {
  std::byte buf[4] = {};
  auto read = ref.try_read(offset, span<std::byte>(buf, 4));
  EXPECT_TRUE(read.has_value());
  std::uint32_t value;
  std::memcpy(&value, buf, 4);
  return value;
}

reloco::result<void> write_u32(mmio_device_ref &ref, std::uint64_t offset, std::uint32_t value) {
  std::byte buf[4];
  std::memcpy(buf, &value, 4);
  return ref.try_write(offset, span<const std::byte>(buf, 4));
}

class MmioFramebufferDeviceTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto maker = device::try_create(64, 32);
    ASSERT_TRUE(maker.has_value());
    dev_ = std::move(maker.value());
  }

  device dev_;
};

} // namespace

TEST(MmioFramebufferDeviceAllocation, ZeroDimensionsAreRejected) {
  EXPECT_EQ(device::try_create(0, 32).error(), error::invalid_argument);
  EXPECT_EQ(device::try_create(64, 0).error(), error::invalid_argument);
}

TEST(MmioFramebufferDeviceAllocation, DefaultConstructedDeviceIsEmptyAndSafeToDestroy) {
  device dev;
  EXPECT_EQ(dev.width(), 0u);
  EXPECT_EQ(dev.height(), 0u);
  EXPECT_FALSE(dev.owns_allocation());
}

TEST(MmioFramebufferDeviceAllocation, MoveTransfersOwnershipAndLeavesSourceEmpty) {
  auto maker = device::try_create(64, 32);
  ASSERT_TRUE(maker.has_value());
  device a = std::move(maker.value());
  auto a_raw = a.pixels().raw();
  const void *buffer_ptr = a_raw.data();

  device b = std::move(a);
  auto b_raw = b.pixels().raw();
  EXPECT_EQ(b_raw.data(), buffer_ptr);
  EXPECT_EQ(b.width(), 64u);
  EXPECT_TRUE(b.owns_allocation());
  EXPECT_EQ(a.width(), 0u);             // NOLINT(bugprone-use-after-move)
  EXPECT_FALSE(a.owns_allocation());    // NOLINT(bugprone-use-after-move)
}

TEST_F(MmioFramebufferDeviceTest, OwnedFramebufferIsPageAlignedAndAtLeastTightlyPacked) {
  EXPECT_TRUE(dev_.owns_allocation());
  const auto raw = dev_.pixels().raw();
  EXPECT_GE(raw.size(), 64u * 32u * 4u);
  EXPECT_EQ(raw.size() % device::default_page_size, 0u);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(raw.data()) % device::default_page_size, 0u);
}

TEST_F(MmioFramebufferDeviceTest, HypervisorSideDrawingWritesDirectlyIntoThePixelBuffer) {
  dev_.pixels().clear(rgb_color::black());
  dev_.pixels().fill_rect(1, 1, 2, 2, rgb_color::red());
  auto px11 = dev_.pixels().get_pixel(1, 1);
  auto px00 = dev_.pixels().get_pixel(0, 0);
  ASSERT_TRUE(px11.has_value());
  ASSERT_TRUE(px00.has_value());
  EXPECT_EQ(*px11, rgb_color::red());
  EXPECT_EQ(*px00, rgb_color::black());
}

TEST_F(MmioFramebufferDeviceTest, ControlWindowReportsGeometryAndFormat) {
  mmio_device_ref ref(dev_);
  EXPECT_EQ(ref.size(), device::control_window_size);
  EXPECT_EQ(read_u32(ref, device::control_off_width), 64u);
  EXPECT_EQ(read_u32(ref, device::control_off_height), 32u);
  EXPECT_EQ(read_u32(ref, device::control_off_stride_bytes), 64u * 4u);
  EXPECT_EQ(read_u32(ref, device::control_off_bytes_per_pixel), 4u);
  EXPECT_EQ(read_u32(ref, device::control_off_pixel_format_id), pixel_format_id<xrgb8888>::value);
}

TEST_F(MmioFramebufferDeviceTest, GeometryRegistersAreReadOnly) {
  mmio_device_ref ref(dev_);
  EXPECT_EQ(write_u32(ref, device::control_off_width, 1).error(), error::permission_denied);
  EXPECT_EQ(write_u32(ref, device::control_off_pixel_format_id, 1).error(), error::permission_denied);
}

TEST_F(MmioFramebufferDeviceTest, PresentDoorbellSetsAndReadingOrTakeDirtyClearsIt) {
  mmio_device_ref ref(dev_);
  EXPECT_FALSE(dev_.take_dirty());

  ASSERT_TRUE(write_u32(ref, device::control_off_present, 0xABCDu).has_value());
  EXPECT_TRUE(dev_.take_dirty());
  EXPECT_FALSE(dev_.take_dirty()); // cleared by the previous take_dirty()

  ASSERT_TRUE(write_u32(ref, device::control_off_present, 1).has_value());
  EXPECT_EQ(read_u32(ref, device::control_off_present), 1u); // reading also acknowledges it
  EXPECT_EQ(read_u32(ref, device::control_off_present), 0u);
}

TEST_F(MmioFramebufferDeviceTest, OutOfRangeControlAccessIsRejected) {
  mmio_device_ref ref(dev_);
  std::byte buf[4] = {};
  auto read = ref.try_read(device::control_window_size, span<std::byte>(buf, 4));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), error::out_of_range);
}

TEST(MmioFramebufferDeviceExternal, BindsOverCallerOwnedMemoryWithoutOwningIt) {
  std::vector<std::byte> backing(64 * 32 * 4);
  auto maker = device::try_bind_external(span<std::byte>(backing.data(), backing.size()), 64, 32, 64 * 4);
  ASSERT_TRUE(maker.has_value());
  device dev = std::move(maker.value());
  EXPECT_FALSE(dev.owns_allocation());
  auto dev_raw = dev.pixels().raw();
  EXPECT_EQ(dev_raw.data(), backing.data());

  ASSERT_TRUE(dev.pixels().put_pixel(0, 0, rgb_color::white()).has_value());
  auto px00 = dev.pixels().get_pixel(0, 0);
  ASSERT_TRUE(px00.has_value());
  EXPECT_EQ(*px00, rgb_color::white());
  // `backing` itself remains valid and owned by the test after `dev` is destroyed below -- only the test
  // exercises that no double-free/crash happens on destruction of a non-owning device.
}

TEST(MmioFramebufferDeviceExternal, RespectsAByteOffsetIntoTheExternalBuffer) {
  constexpr std::size_t stride = 64 * 4;
  std::vector<std::byte> backing(stride * 32 + 128); // extra header-like region before the pixel data
  auto maker = device::try_bind_external(span<std::byte>(backing.data(), backing.size()), 64, 32, stride, 128);
  ASSERT_TRUE(maker.has_value());
  device dev = std::move(maker.value());
  auto dev_raw = dev.pixels().raw();
  EXPECT_EQ(dev_raw.data(), backing.data() + 128);
}

TEST(MmioFramebufferDeviceExternal, RejectsATooSmallExternalBuffer) {
  std::vector<std::byte> backing(10); // far too small for 64x32x4
  auto maker = device::try_bind_external(span<std::byte>(backing.data(), backing.size()), 64, 32, 64 * 4);
  EXPECT_FALSE(maker.has_value());
}

RELOCO_END_UNSAFE_BUFFER_USAGE