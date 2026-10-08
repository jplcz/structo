// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/vga_text_console.hpp>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo::hw;

namespace {

constexpr std::size_t kCols = 10;
constexpr std::size_t kRows = 5;

} // namespace

TEST(VgaTextConsoleTest, TryCreateFailsOnZeroDimensionsOrTooSmallBuffer) {
  std::byte storage[kCols * kRows * 2];
  reloco::span<std::byte> buf(storage, sizeof(storage));

  EXPECT_FALSE(vga_text_console::try_create(buf, 0, kRows));
  EXPECT_FALSE(vga_text_console::try_create(buf, kCols, 0));

  auto too_small = vga_text_console::try_create(reloco::span<std::byte>(storage, 4), kCols, kRows);
  ASSERT_FALSE(too_small);
  EXPECT_EQ(too_small.error(), reloco::error::out_of_range);

  auto ok = vga_text_console::try_create(buf, kCols, kRows);
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->columns(), kCols);
  EXPECT_EQ(ok->rows(), kRows);
}

TEST(VgaTextConsoleTest, PutAndGetCellRoundTripsCharAndPackedAttribute) {
  std::byte storage[kCols * kRows * 2]{};
  auto console = vga_text_console::try_create(reloco::span<std::byte>(storage, sizeof(storage)), kCols, kRows);
  ASSERT_TRUE(console);

  console->put_cell(2, 1, 'Q', console_color::yellow, console_color::blue);
  console_cell c = console->get_cell(2, 1);
  EXPECT_EQ(c.ch, 'Q');
  EXPECT_EQ(c.fg, console_color::yellow);
  EXPECT_EQ(c.bg, console_color::blue);

  // Raw buffer layout: code point byte, then packed attribute byte
  // (bits 3-0 fg, bits 6-4 bg).
  std::size_t cell_offset = (1 * kCols + 2) * 2;
  EXPECT_EQ(static_cast<char>(storage[cell_offset]), 'Q');
  std::uint8_t attr = static_cast<std::uint8_t>(storage[cell_offset + 1]);
  EXPECT_EQ(attr & 0x0F, static_cast<std::uint8_t>(console_color::yellow));
  EXPECT_EQ((attr >> 4) & 0x07, static_cast<std::uint8_t>(console_color::blue));
}

TEST(VgaTextConsoleTest, BackgroundColorIsMaskedToThreeBitsPerVgaConvention) {
  std::byte storage[kCols * kRows * 2]{};
  auto console = vga_text_console::try_create(reloco::span<std::byte>(storage, sizeof(storage)), kCols, kRows);
  ASSERT_TRUE(console);

  // `white` (0x0F) as a background: only the low 3 bits (0x07) survive.
  console->put_cell(0, 0, 'A', console_color::white, console_color::white);
  console_cell c = console->get_cell(0, 0);
  EXPECT_EQ(static_cast<std::uint8_t>(c.bg), 0x07u);
}

TEST(VgaTextConsoleTest, MoveCursorForwardsToOptionalCallbackOrIsNoOp) {
  std::byte storage[kCols * kRows * 2]{};
  std::size_t last_x = 999;
  std::size_t last_y = 999;
  struct sink_fn {
    std::size_t *lx;
    std::size_t *ly;
    void operator()(std::size_t x, std::size_t y) const noexcept {
      *lx = x;
      *ly = y;
    }
  } fn{&last_x, &last_y};

  auto console = vga_text_console::try_create(reloco::span<std::byte>(storage, sizeof(storage)), kCols, kRows,
                                              vga_text_console::cursor_sink(fn));
  ASSERT_TRUE(console);
  console->move_cursor(4, 2);
  EXPECT_EQ(last_x, 4u);
  EXPECT_EQ(last_y, 2u);

  // Without a callback supplied: no-op, must not crash.
  auto no_sink = vga_text_console::try_create(reloco::span<std::byte>(storage, sizeof(storage)), kCols, kRows);
  ASSERT_TRUE(no_sink);
  no_sink->move_cursor(1, 1);
}

TEST(VgaTextConsoleTest, AdaptsToConsoleRefAndSupportsScrollViaGetCell) {
  std::byte storage[kCols * kRows * 2]{};
  auto backend = vga_text_console::try_create(reloco::span<std::byte>(storage, sizeof(storage)), kCols, kRows);
  ASSERT_TRUE(backend);

  console_ref ref(*backend);
  EXPECT_EQ(ref.columns(), kCols);
  EXPECT_EQ(ref.rows(), kRows);

  ref.write("Row0\n");
  ref.write("Row1\n");
  ref.scroll_up(1, console_color::light_gray, console_color::black);

  auto c = ref.get_char(0, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->ch, 'R'); // "Row1" moved up to row 0 after the scroll.
}

RELOCO_END_UNSAFE_BUFFER_USAGE