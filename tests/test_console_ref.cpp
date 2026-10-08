// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/hw/console_ref.hpp>

using namespace structo::hw;

namespace {

constexpr std::size_t kCols = 8;
constexpr std::size_t kRows = 4;

struct fake_console {
  reloco::array<console_cell, kCols * kRows> cells{};
  std::size_t last_move_x = 999;
  std::size_t last_move_y = 999;
  std::size_t move_count = 0;
  bool cursor_visible = true;

  console_cell &at(std::size_t x, std::size_t y) { return cells[y * kCols + x]; }
};

// console_cell/console_ref::get_char() return result<T>; dereferencing a
// temporary result is blocked by reloco's rvalue-safety guard, so this
// binds it to a named local first.
console_cell cell_at(const console_ref &c, std::size_t x, std::size_t y) {
  auto r = c.get_char(x, y);
  return *r;
}

} // namespace

template <> struct structo::hw::console_traits<fake_console> {
  static void put_cell(fake_console &b, std::size_t x, std::size_t y, char ch, console_color fg,
                       console_color bg) noexcept {
    b.at(x, y) = {ch, fg, bg};
  }
  static result<console_cell> get_cell(const fake_console &b, std::size_t x, std::size_t y) noexcept {
    return b.cells[y * kCols + x];
  }
  static std::size_t columns(const fake_console &) noexcept { return kCols; }
  static std::size_t rows(const fake_console &) noexcept { return kRows; }
  static void move_cursor(fake_console &b, std::size_t x, std::size_t y) noexcept {
    b.last_move_x = x;
    b.last_move_y = y;
    ++b.move_count;
  }
  static void set_cursor_visible(fake_console &b, bool visible) noexcept { b.cursor_visible = visible; }
};

namespace {

// Backend adapted without the optional get_cell/move_cursor/set_cursor_visible members.
struct minimal_console {
  std::size_t cols;
  std::size_t rows;
};

} // namespace

template <> struct structo::hw::console_traits<minimal_console> {
  static void put_cell(minimal_console &, std::size_t, std::size_t, char, console_color, console_color) noexcept {}
  static std::size_t columns(const minimal_console &b) noexcept { return b.cols; }
  static std::size_t rows(const minimal_console &b) noexcept { return b.rows; }
};

TEST(ConsoleRefTest, UnboundRefReportsZeroSizeAndFailsEveryOperation) {
  console_ref ref;
  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_EQ(ref.columns(), 0u);
  EXPECT_EQ(ref.rows(), 0u);

  auto put = ref.put_char(0, 0, 'A', console_color::white, console_color::black);
  ASSERT_FALSE(put);
  EXPECT_EQ(put.error(), reloco::error::unsupported_operation);

  auto get = ref.get_char(0, 0);
  ASSERT_FALSE(get);
  EXPECT_EQ(get.error(), reloco::error::unsupported_operation);

  // Non-failing helpers are simply no-ops when unbound.
  ref.clear(console_color::white, console_color::black);
  ref.set_cursor(3, 3);
  EXPECT_EQ(ref.cursor_x(), 0u);
  EXPECT_EQ(ref.cursor_y(), 0u);
}

TEST(ConsoleRefTest, BoundRefReportsBackendGeometryAndInitialState) {
  fake_console backend;
  console_ref ref(backend);
  EXPECT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(ref.columns(), kCols);
  EXPECT_EQ(ref.rows(), kRows);
  EXPECT_EQ(ref.cursor_x(), 0u);
  EXPECT_EQ(ref.cursor_y(), 0u);
  EXPECT_EQ(ref.foreground(), console_color::light_gray);
  EXPECT_EQ(ref.background(), console_color::black);
}

TEST(ConsoleRefTest, PutAndGetCharRoundTripAndBoundsCheck) {
  fake_console backend;
  console_ref ref(backend);

  ASSERT_TRUE(ref.put_char(2, 1, 'Z', console_color::yellow, console_color::blue));
  console_cell c = cell_at(ref, 2, 1);
  EXPECT_EQ(c.ch, 'Z');
  EXPECT_EQ(c.fg, console_color::yellow);
  EXPECT_EQ(c.bg, console_color::blue);

  auto oob_put = ref.put_char(kCols, 0, 'X', console_color::white, console_color::black);
  ASSERT_FALSE(oob_put);
  EXPECT_EQ(oob_put.error(), reloco::error::out_of_bounds);

  auto oob_get = ref.get_char(0, kRows);
  ASSERT_FALSE(oob_get);
  EXPECT_EQ(oob_get.error(), reloco::error::out_of_bounds);
}

TEST(ConsoleRefTest, GetCharFailsWhenBackendHasNoOptionalGetCell) {
  minimal_console backend{kCols, kRows};
  console_ref ref(backend);
  auto get = ref.get_char(0, 0);
  ASSERT_FALSE(get);
  EXPECT_EQ(get.error(), reloco::error::unsupported_operation);
}

TEST(ConsoleRefTest, SetCursorClampsAndForwardsToOptionalMoveCursor) {
  fake_console backend;
  console_ref ref(backend);

  ref.set_cursor(3, 2);
  EXPECT_EQ(ref.cursor_x(), 3u);
  EXPECT_EQ(ref.cursor_y(), 2u);
  EXPECT_EQ(backend.last_move_x, 3u);
  EXPECT_EQ(backend.last_move_y, 2u);

  ref.set_cursor(100, 100);
  EXPECT_EQ(ref.cursor_x(), kCols - 1);
  EXPECT_EQ(ref.cursor_y(), kRows - 1);
  EXPECT_EQ(backend.last_move_x, kCols - 1);
  EXPECT_EQ(backend.last_move_y, kRows - 1);
}

TEST(ConsoleRefTest, SetCursorIsNoOpOnBackendWithoutMoveCursor) {
  minimal_console backend{kCols, kRows};
  console_ref ref(backend);
  ref.set_cursor(1, 1); // Must not crash even though move_cursor is unimplemented.
  EXPECT_EQ(ref.cursor_x(), 1u);
  EXPECT_EQ(ref.cursor_y(), 1u);
}

TEST(ConsoleRefTest, SetCursorVisibleForwardsOrIsNoOp) {
  fake_console backend;
  console_ref ref(backend);
  ref.set_cursor_visible(false);
  EXPECT_FALSE(backend.cursor_visible);
  ref.set_cursor_visible(true);
  EXPECT_TRUE(backend.cursor_visible);

  minimal_console minimal{kCols, kRows};
  console_ref minimal_ref(minimal);
  minimal_ref.set_cursor_visible(false); // Must not crash.
}

TEST(ConsoleRefTest, SetColorsUpdatesForegroundAndBackground) {
  fake_console backend;
  console_ref ref(backend);
  ref.set_colors(console_color::light_red, console_color::dark_gray);
  EXPECT_EQ(ref.foreground(), console_color::light_red);
  EXPECT_EQ(ref.background(), console_color::dark_gray);
}

TEST(ConsoleRefTest, FillRectClipsToBoundsAndWritesEveryCell) {
  fake_console backend;
  console_ref ref(backend);
  ref.fill_rect(kCols - 2, kRows - 2, 10, 10, '#', console_color::white, console_color::black);
  for (std::size_t y = kRows - 2; y < kRows; ++y) {
    for (std::size_t x = kCols - 2; x < kCols; ++x) {
      EXPECT_EQ(cell_at(ref, x, y).ch, '#');
    }
  }
  // Untouched cell.
  EXPECT_EQ(cell_at(ref, 0, 0).ch, ' ');
}

TEST(ConsoleRefTest, ClearBlanksEveryCellWithGivenColors) {
  fake_console backend;
  console_ref ref(backend);
  ASSERT_TRUE(ref.put_char(3, 2, 'Q', console_color::red, console_color::red));
  ref.clear(console_color::green, console_color::blue);
  for (std::size_t y = 0; y < kRows; ++y) {
    for (std::size_t x = 0; x < kCols; ++x) {
      console_cell c = cell_at(ref, x, y);
      EXPECT_EQ(c.ch, ' ');
      EXPECT_EQ(c.fg, console_color::green);
      EXPECT_EQ(c.bg, console_color::blue);
    }
  }
}

TEST(ConsoleRefTest, ScrollUpMovesRowsAndBlanksBottom) {
  fake_console backend;
  console_ref ref(backend);
  for (std::size_t y = 0; y < kRows; ++y) {
    ASSERT_TRUE(ref.put_char(0, y, static_cast<char>('A' + y), console_color::white, console_color::black));
  }
  ref.scroll_up(1, console_color::light_gray, console_color::black);
  for (std::size_t y = 0; y + 1 < kRows; ++y) {
    EXPECT_EQ(cell_at(ref, 0, y).ch, static_cast<char>('A' + y + 1));
  }
  EXPECT_EQ(cell_at(ref, 0, kRows - 1).ch, ' ');
}

TEST(ConsoleRefTest, ScrollUpByAllOrMoreRowsClears) {
  fake_console backend;
  console_ref ref(backend);
  ASSERT_TRUE(ref.put_char(0, 0, 'Z', console_color::white, console_color::black));
  ref.scroll_up(kRows + 5, console_color::light_gray, console_color::black);
  EXPECT_EQ(cell_at(ref, 0, 0).ch, ' ');
}

TEST(ConsoleRefTest, ScrollUpIsNoOpForZeroLinesOrUnbound) {
  fake_console backend;
  console_ref ref(backend);
  ASSERT_TRUE(ref.put_char(0, 0, 'Z', console_color::white, console_color::black));
  ref.scroll_up(0, console_color::light_gray, console_color::black);
  EXPECT_EQ(cell_at(ref, 0, 0).ch, 'Z');

  console_ref unbound;
  unbound.scroll_up(1, console_color::light_gray, console_color::black); // Must not crash.
}

TEST(ConsoleRefTest, WriteCharAdvancesCursorWithWrapAndScroll) {
  fake_console backend;
  console_ref ref(backend);
  for (std::size_t i = 0; i < kCols; ++i) {
    ref.write_char('x');
  }
  EXPECT_EQ(ref.cursor_x(), 0u);
  EXPECT_EQ(ref.cursor_y(), 1u);

  // Fill to the last row, then overflow to trigger a scroll.
  ref.set_cursor(0, kRows - 1);
  for (std::size_t i = 0; i < kCols; ++i) {
    ref.write_char('y');
  }
  EXPECT_EQ(ref.cursor_y(), kRows - 1);
}

TEST(ConsoleRefTest, PutHandlesNewlineCarriageReturnTabAndBackspace) {
  fake_console backend;
  console_ref ref(backend);

  ref.put('H');
  ref.put('i');
  EXPECT_EQ(ref.cursor_x(), 2u);
  EXPECT_EQ(cell_at(ref, 0, 0).ch, 'H');
  EXPECT_EQ(cell_at(ref, 1, 0).ch, 'i');

  ref.put('\n');
  EXPECT_EQ(ref.cursor_x(), 0u);
  EXPECT_EQ(ref.cursor_y(), 1u);

  ref.put('A');
  ref.put('\r');
  EXPECT_EQ(ref.cursor_x(), 0u);
  EXPECT_EQ(ref.cursor_y(), 1u);

  ref.put('\t');
  EXPECT_EQ(ref.cursor_x(), kCols - 1); // Next tab stop (8) is out of range: clamps to the last column.

  ref.set_cursor(3, 0);
  ref.put('\b');
  EXPECT_EQ(ref.cursor_x(), 2u);
  ref.set_cursor(0, 0);
  ref.put('\b'); // Already at column 0: no-op.
  EXPECT_EQ(ref.cursor_x(), 0u);
}

TEST(ConsoleRefTest, WriteFeedsEveryCharacterOfAStringView) {
  fake_console backend;
  console_ref ref(backend);
  ref.write("Hi\n");
  EXPECT_EQ(cell_at(ref, 0, 0).ch, 'H');
  EXPECT_EQ(cell_at(ref, 1, 0).ch, 'i');
  EXPECT_EQ(ref.cursor_x(), 0u);
  EXPECT_EQ(ref.cursor_y(), 1u);
}
