// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/mmio_gpu_command_buffer_device.hpp>

#include <cstring>
#include <utility>
#include <vector>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using structo::hw::framebuffer;
using structo::hw::gpu_accel_ref;
using structo::hw::rgb_color;
using structo::hw::xrgb8888;
using structo::hypervisor::decode_gpu_command;
using structo::hypervisor::encode_gpu_command;
using structo::hypervisor::gpu_command;
using structo::hypervisor::gpu_command_opcode;
using structo::hypervisor::mmio_device_ref;
using structo::hypervisor::mmio_gpu_command_buffer_device;
using structo::hypervisor::pack_gpu_color;
using structo::hypervisor::unpack_gpu_color;
using reloco::error;
using reloco::span;

namespace {

using device = mmio_gpu_command_buffer_device;

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

framebuffer<xrgb8888> make_framebuffer(std::vector<std::byte> &storage, std::size_t w, std::size_t h) {
  storage.assign(w * h * 4, std::byte{0});
  auto fb = framebuffer<xrgb8888>::try_create(span<std::byte>(storage.data(), storage.size()), w, h, w * 4);
  return std::move(*fb);
}

// get_pixel() returns a temporary reloco::result<rgb_color>; operator* is deleted on an rvalue, so this
// binds it to a named local first before dereferencing.
rgb_color pixel_at(const framebuffer<xrgb8888> &fb, std::size_t x, std::size_t y) {
  auto result = fb.get_pixel(x, y);
  return *result;
}

void stage_command(device &dev, std::size_t slot, const gpu_command &cmd) {
  span<std::byte> buf = dev.command_buffer();
  encode_gpu_command(cmd, span<std::byte>(buf.data() + slot * device::command_slot_size, device::command_slot_size));
}

class MmioGpuCommandBufferDeviceTest : public ::testing::Test {
protected:
  void SetUp() override {
    screen_ = make_framebuffer(storage_, 32, 32);
    target_ = gpu_accel_ref(screen_);
    auto maker = device::try_create(target_, 8);
    ASSERT_TRUE(maker.has_value());
    dev_ = std::move(maker.value());
  }

  std::vector<std::byte> storage_;
  framebuffer<xrgb8888> screen_;
  gpu_accel_ref target_;
  device dev_;
};

} // namespace

TEST(MmioGpuCommandBufferDeviceAllocation, ZeroCapacityIsRejected) {
  gpu_accel_ref target;
  EXPECT_EQ(device::try_create(target, 0).error(), error::invalid_argument);
}

TEST(MmioGpuCommandBufferDeviceAllocation, DefaultConstructedDeviceIsEmptyAndSafeToDestroy) {
  device dev;
  EXPECT_EQ(dev.capacity(), 0u);
  EXPECT_EQ(dev.count(), 0u);
}

TEST(MmioGpuCommandBufferDeviceAllocation, MoveTransfersOwnershipAndEmptiesSource) {
  gpu_accel_ref target;
  auto maker = device::try_create(target, 4);
  ASSERT_TRUE(maker.has_value());
  device a = std::move(maker.value());
  span<std::byte> a_buf = a.command_buffer();
  std::byte *base = a_buf.data();
  ASSERT_TRUE(a.set_count(2).has_value());

  device b = std::move(a);
  EXPECT_EQ(b.capacity(), 4u);
  EXPECT_EQ(b.count(), 2u);
  span<std::byte> b_buf = b.command_buffer();
  EXPECT_EQ(b_buf.data(), base);
  EXPECT_EQ(a.capacity(), 0u); // moved-from
}

TEST(MmioGpuCommandBufferDeviceAllocation, CommandBufferIsPageAlignedAndSizedForCapacity) {
  gpu_accel_ref target;
  auto maker = device::try_create(target, 8); // 8 * 24 = 192 bytes, well under one page
  ASSERT_TRUE(maker.has_value());
  device dev = std::move(maker.value());

  auto buf = dev.command_buffer();
  EXPECT_EQ(buf.size() % device::default_page_size, 0u);
  EXPECT_GE(buf.size(), 8u * device::command_slot_size);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(buf.data()) % device::default_page_size, 0u);
}

TEST_F(MmioGpuCommandBufferDeviceTest, ExecutePendingRunsStagedCommandsDirectly) {
  stage_command(dev_, 0, gpu_command{gpu_command_opcode::clear, 0, 0, 0, 0, pack_gpu_color({5, 5, 5})});
  stage_command(dev_, 1,
                gpu_command{gpu_command_opcode::fill_rect, 2, 2, 4, 4, pack_gpu_color({10, 20, 30})});
  ASSERT_TRUE(dev_.set_count(2).has_value());

  std::size_t executed = dev_.execute_pending();
  EXPECT_EQ(executed, 2u);
  EXPECT_EQ(dev_.last_executed(), 2u);
  EXPECT_EQ(dev_.count(), 0u); // reset after execution

  EXPECT_EQ(pixel_at(screen_, 0, 0), (rgb_color{5, 5, 5}));
  EXPECT_EQ(pixel_at(screen_, 3, 3), (rgb_color{10, 20, 30}));
}

TEST_F(MmioGpuCommandBufferDeviceTest, NopOpcodeAndZeroedTrailingSlotsAreSafe) {
  // Slot 0 left zeroed (decodes as `nop`); executing it must not touch the screen at all.
  ASSERT_TRUE(dev_.set_count(1).has_value());
  EXPECT_EQ(dev_.execute_pending(), 1u);
  EXPECT_EQ(pixel_at(screen_, 0, 0), (rgb_color{0, 0, 0}));
}

TEST_F(MmioGpuCommandBufferDeviceTest, NegativeRectOperandsAreSilentlyIgnoredNotWrapped) {
  stage_command(dev_, 0, gpu_command{gpu_command_opcode::fill_rect, -1, 0, 4, 4, pack_gpu_color({9, 9, 9})});
  ASSERT_TRUE(dev_.set_count(1).has_value());
  EXPECT_EQ(dev_.execute_pending(), 1u); // still counted as executed, just a no-op draw
  for (std::size_t y = 0; y < 32; ++y) {
    for (std::size_t x = 0; x < 32; ++x) {
      EXPECT_EQ(pixel_at(screen_, x, y), (rgb_color{0, 0, 0}));
    }
  }
}

TEST_F(MmioGpuCommandBufferDeviceTest, DrawLineAllowsOffscreenEndpoints) {
  stage_command(dev_, 0, gpu_command{gpu_command_opcode::draw_line, -5, -5, 31, 31, pack_gpu_color({1, 2, 3})});
  ASSERT_TRUE(dev_.set_count(1).has_value());
  dev_.execute_pending();
  EXPECT_EQ(pixel_at(screen_, 31, 31), (rgb_color{1, 2, 3}));
}

TEST_F(MmioGpuCommandBufferDeviceTest, SetCountRejectsValuesAboveCapacity) {
  EXPECT_EQ(dev_.set_count(9).error(), error::out_of_range);
  EXPECT_EQ(dev_.count(), 0u);
}

TEST_F(MmioGpuCommandBufferDeviceTest, ResetQueueDiscardsPendingCommandsWithoutRunningThem) {
  stage_command(dev_, 0, gpu_command{gpu_command_opcode::clear, 0, 0, 0, 0, pack_gpu_color({1, 1, 1})});
  ASSERT_TRUE(dev_.set_count(1).has_value());
  dev_.reset_queue();
  EXPECT_EQ(dev_.count(), 0u);
  EXPECT_EQ(dev_.execute_pending(), 0u); // nothing left queued
  EXPECT_EQ(pixel_at(screen_, 0, 0), (rgb_color{0, 0, 0}));
}

TEST_F(MmioGpuCommandBufferDeviceTest, ControlWindowReportsCapacityCountAndLastExecuted) {
  mmio_device_ref ref(dev_);
  EXPECT_EQ(ref.size(), device::control_window_size);
  EXPECT_EQ(read_u32(ref, device::control_off_capacity), 8u);
  EXPECT_EQ(read_u32(ref, device::control_off_count), 0u);
  EXPECT_EQ(read_u32(ref, device::control_off_execute), 0u);

  stage_command(dev_, 0, gpu_command{gpu_command_opcode::clear, 0, 0, 0, 0, pack_gpu_color({42, 42, 42})});
  ASSERT_TRUE(write_u32(ref, device::control_off_count, 1).has_value());
  EXPECT_EQ(read_u32(ref, device::control_off_count), 1u);

  ASSERT_TRUE(write_u32(ref, device::control_off_execute, 0xdeadbeef).has_value()); // value itself ignored
  EXPECT_EQ(read_u32(ref, device::control_off_count), 0u); // reset after execution
  EXPECT_EQ(read_u32(ref, device::control_off_execute), 1u); // last_executed
  EXPECT_EQ(pixel_at(screen_, 5, 5), (rgb_color{42, 42, 42}));
}

TEST_F(MmioGpuCommandBufferDeviceTest, CapacityRegisterIsReadOnly) {
  mmio_device_ref ref(dev_);
  EXPECT_EQ(write_u32(ref, device::control_off_capacity, 123).error(), error::permission_denied);
}

TEST_F(MmioGpuCommandBufferDeviceTest, CountRegisterRejectsValueAboveCapacity) {
  mmio_device_ref ref(dev_);
  EXPECT_EQ(write_u32(ref, device::control_off_count, 99).error(), error::out_of_range);
}

TEST_F(MmioGpuCommandBufferDeviceTest, OutOfRangeControlAccessIsRejected) {
  mmio_device_ref ref(dev_);
  std::byte buf[4] = {};
  EXPECT_EQ(ref.try_read(100, span<std::byte>(buf, 4)).error(), error::out_of_range);
}

TEST_F(MmioGpuCommandBufferDeviceTest, TryResetDiscardsPendingCommands) {
  mmio_device_ref ref(dev_);
  stage_command(dev_, 0, gpu_command{gpu_command_opcode::clear, 0, 0, 0, 0, pack_gpu_color({1, 1, 1})});
  ASSERT_TRUE(write_u32(ref, device::control_off_count, 1).has_value());

  ASSERT_TRUE(ref.try_reset().has_value());
  EXPECT_EQ(read_u32(ref, device::control_off_count), 0u);
  ASSERT_TRUE(write_u32(ref, device::control_off_execute, 1).has_value());
  EXPECT_EQ(pixel_at(screen_, 0, 0), (rgb_color{0, 0, 0})); // the clear never ran
}

TEST(MmioGpuCommandBufferDeviceAvailability, ReflectsWhetherTargetIsBound) {
  gpu_accel_ref unbound_target;
  auto maker = device::try_create(unbound_target, 4);
  ASSERT_TRUE(maker.has_value());
  device dev = std::move(maker.value());
  mmio_device_ref ref(dev);
  EXPECT_FALSE(ref.is_available());

  std::vector<std::byte> storage;
  auto fb = make_framebuffer(storage, 8, 8);
  auto maker2 = device::try_create(gpu_accel_ref(fb), 4);
  dev = std::move(maker2.value());
  mmio_device_ref ref2(dev);
  EXPECT_TRUE(ref2.is_available());
}

TEST(GpuCommandCodec, RoundTripsThroughEncodeDecode) {
  gpu_command original{gpu_command_opcode::draw_line, -1, 2, 100, -50, pack_gpu_color({1, 2, 3, 255})};
  std::byte slot[mmio_gpu_command_buffer_device::command_slot_size];
  encode_gpu_command(original, span<std::byte>(slot, sizeof(slot)));
  gpu_command decoded = decode_gpu_command(span<const std::byte>(slot, sizeof(slot)));

  EXPECT_EQ(decoded.opcode, original.opcode);
  EXPECT_EQ(decoded.x0, original.x0);
  EXPECT_EQ(decoded.y0, original.y0);
  EXPECT_EQ(decoded.x1, original.x1);
  EXPECT_EQ(decoded.y1, original.y1);
  EXPECT_EQ(decoded.color, original.color);
}

TEST(GpuCommandCodec, PackUnpackColorRoundTrips) {
  rgb_color c{10, 20, 30, 40};
  EXPECT_EQ(unpack_gpu_color(pack_gpu_color(c)), c);
}

RELOCO_END_UNSAFE_BUFFER_USAGE