// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hypervisor/mmio_text_console.hpp>

#include <cstdint>
#include <cstring>
#include <utility>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using reloco::error;
using reloco::span;
using structo::hw::console_cell;
using structo::hw::console_color;
using structo::hw::console_ref;
using structo::hypervisor::mmio_device_ref;
using structo::hypervisor::mmio_text_console;

namespace {

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

class MmioTextConsoleTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto maker = mmio_text_console::try_create(80, 25);
    ASSERT_TRUE(maker.has_value());
    console_ = std::move(maker.value());
  }

  mmio_text_console console_;
};

} // namespace

TEST(MmioTextConsoleAllocation, ZeroDimensionsAreRejected) {
  EXPECT_EQ(mmio_text_console::try_create(0, 25).error(), error::invalid_argument);
  EXPECT_EQ(mmio_text_console::try_create(80, 0).error(), error::invalid_argument);
}

TEST(MmioTextConsoleAllocation, DefaultConstructedConsoleIsEmptyAndSafeToDestroy) {
  mmio_text_console console;
  EXPECT_EQ(console.columns(), 0u);
  EXPECT_EQ(console.rows(), 0u);
  EXPECT_EQ(console.framebuffer().size(), 0u);
}

TEST(MmioTextConsoleAllocation, MoveTransfersOwnershipAndLeavesSourceEmpty) {
  auto maker = mmio_text_console::try_create(80, 25);
  ASSERT_TRUE(maker.has_value());
  mmio_text_console a = std::move(maker.value());
  auto a_fb = a.framebuffer();
  const void *buffer_ptr = a_fb.data();

  mmio_text_console b = std::move(a);
  auto b_fb = b.framebuffer();
  EXPECT_EQ(b_fb.data(), buffer_ptr);
  EXPECT_EQ(b.columns(), 80u);
  EXPECT_EQ(a.columns(), 0u);            // NOLINT(bugprone-use-after-move) -- checking the moved-from state itself
  EXPECT_EQ(a.framebuffer().size(), 0u); // NOLINT(bugprone-use-after-move)
}

TEST_F(MmioTextConsoleTest, FramebufferIsPageAlignedAndAtLeastTwoBytesPerCell) {
  EXPECT_EQ(console_.columns(), 80u);
  EXPECT_EQ(console_.rows(), 25u);
  const auto fb = console_.framebuffer();
  // 80 * 25 * 2 == 4000 bytes of cell data, rounded up to the next whole page.
  EXPECT_GE(fb.size(), 80u * 25u * 2u);
  EXPECT_EQ(fb.size() % mmio_text_console::default_page_size, 0u);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(fb.data()) % mmio_text_console::default_page_size, 0u);
}

TEST_F(MmioTextConsoleTest, ConsoleRefWritesLandInTheFramebufferCells) {
  console_ref ref(console_);
  ref.write("Hi");
  EXPECT_EQ(console_.get_cell(0, 0).ch, 'H');
  EXPECT_EQ(console_.get_cell(1, 0).ch, 'i');
}

TEST_F(MmioTextConsoleTest, ControlWindowSizeMatchesTraits) {
  mmio_device_ref ref(console_);
  EXPECT_EQ(ref.size(), mmio_text_console::control_window_size);
}

TEST_F(MmioTextConsoleTest, ColumnsAndRowsRegistersAreReadOnly) {
  mmio_device_ref ref(console_);
  EXPECT_EQ(read_u32(ref, mmio_text_console::control_off_columns), 80u);
  EXPECT_EQ(read_u32(ref, mmio_text_console::control_off_rows), 25u);

  EXPECT_EQ(write_u32(ref, mmio_text_console::control_off_columns, 40).error(), error::permission_denied);
  EXPECT_EQ(write_u32(ref, mmio_text_console::control_off_rows, 10).error(), error::permission_denied);
}

TEST_F(MmioTextConsoleTest, CursorRegistersRoundTripAndClampToTheGrid) {
  mmio_device_ref ref(console_);
  ASSERT_TRUE(write_u32(ref, mmio_text_console::control_off_cursor_x, 5).has_value());
  ASSERT_TRUE(write_u32(ref, mmio_text_console::control_off_cursor_y, 7).has_value());
  EXPECT_EQ(read_u32(ref, mmio_text_console::control_off_cursor_x), 5u);
  EXPECT_EQ(read_u32(ref, mmio_text_console::control_off_cursor_y), 7u);
  EXPECT_EQ(console_.cursor_x(), 5u);
  EXPECT_EQ(console_.cursor_y(), 7u);

  // Out-of-grid values clamp to the last valid row/column, matching console_ref::set_cursor()'s own
  // clamping convention.
  ASSERT_TRUE(write_u32(ref, mmio_text_console::control_off_cursor_x, 999).has_value());
  EXPECT_EQ(read_u32(ref, mmio_text_console::control_off_cursor_x), 79u);
}

TEST_F(MmioTextConsoleTest, CursorVisibleRegisterRoundTrips) {
  mmio_device_ref ref(console_);
  EXPECT_EQ(read_u32(ref, mmio_text_console::control_off_cursor_visible), 1u); // visible by default
  ASSERT_TRUE(write_u32(ref, mmio_text_console::control_off_cursor_visible, 0).has_value());
  EXPECT_EQ(read_u32(ref, mmio_text_console::control_off_cursor_visible), 0u);
  EXPECT_FALSE(console_.cursor_visible());
}

TEST_F(MmioTextConsoleTest, SharedCursorIsVisibleFromBothSidesOfTheDevice) {
  // The hypervisor's own console_ref moves the cursor...
  console_ref ref(console_);
  ref.set_cursor(10, 3);

  // ... and the guest's MMIO control window reads back that exact same position.
  mmio_device_ref dev(console_);
  EXPECT_EQ(read_u32(dev, mmio_text_console::control_off_cursor_x), 10u);
  EXPECT_EQ(read_u32(dev, mmio_text_console::control_off_cursor_y), 3u);
}

TEST_F(MmioTextConsoleTest, WrongRegisterWidthIsRejected) {
  mmio_device_ref ref(console_);
  std::byte one_byte{0};
  auto read = ref.try_read(mmio_text_console::control_off_columns, span<std::byte>(&one_byte, 1));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), error::invalid_argument);
}

TEST_F(MmioTextConsoleTest, PagePaddingPastCellDataIsZeroed) {
  const auto fb = console_.framebuffer();
  const std::size_t cell_bytes = 80u * 25u * 2u;
  for (std::size_t i = cell_bytes; i < fb.size(); ++i) {
    EXPECT_EQ(fb[i], std::byte{0}) << "padding byte " << i << " was not zeroed";
  }
}

TEST_F(MmioTextConsoleTest, OutOfRangeControlAccessIsRejected) {
  mmio_device_ref ref(console_);
  std::byte buf[4] = {};
  auto read = ref.try_read(mmio_text_console::control_window_size, span<std::byte>(buf, 4));
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(read.error(), error::out_of_range);
}

RELOCO_END_UNSAFE_BUFFER_USAGE