// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/vt100.hpp>

using namespace structo::hw;

namespace {

constexpr std::size_t kCols = 20;
constexpr std::size_t kRows = 6;

struct fake_console {
  console_cell cells[kCols * kRows]{};
};

} // namespace

template <> struct structo::hw::console_traits<fake_console> {
  static void put_cell(fake_console &b, std::size_t x, std::size_t y, char ch, console_color fg,
                       console_color bg) noexcept {
    b.cells[y * kCols + x] = {ch, fg, bg};
  }
  static result<console_cell> get_cell(const fake_console &b, std::size_t x, std::size_t y) noexcept {
    return b.cells[y * kCols + x];
  }
  static std::size_t columns(const fake_console &) noexcept { return kCols; }
  static std::size_t rows(const fake_console &) noexcept { return kRows; }
};

namespace {

// console_ref::get_char() returns result<console_cell>; dereferencing a
// temporary result is blocked by reloco's rvalue-safety guard, so this
// binds it to a named local first.
console_cell cell_at(const console_ref &c, std::size_t x, std::size_t y) {
  auto r = c.get_char(x, y);
  return *r;
}

struct Vt100Test : ::testing::Test {
  fake_console backend;
  vt100_terminal term{console_ref(backend)};
};

} // namespace

TEST_F(Vt100Test, PlainTextAdvancesCursorAndWritesGlyphs) {
  term.feed("AB");
  EXPECT_EQ(term.console().cursor_x(), 2u);
  EXPECT_EQ(cell_at(term.console(), 0, 0).ch, 'A');
  EXPECT_EQ(cell_at(term.console(), 1, 0).ch, 'B');
}

TEST_F(Vt100Test, CarriageReturnAndStrictLineFeedDoNotImplyEachOther) {
  term.feed("AB");
  term.feed("\n"); // Strict VT100 LF: down one row, column unchanged.
  EXPECT_EQ(term.console().cursor_x(), 2u);
  EXPECT_EQ(term.console().cursor_y(), 1u);

  term.feed("\r"); // CR: column 0, row unchanged.
  EXPECT_EQ(term.console().cursor_x(), 0u);
  EXPECT_EQ(term.console().cursor_y(), 1u);
}

TEST_F(Vt100Test, LineFeedScrollsOnLastRow) {
  for (std::size_t i = 0; i < kRows; ++i) {
    term.feed("\r\n");
  }
  EXPECT_EQ(term.console().cursor_y(), kRows - 1);
}

TEST_F(Vt100Test, BackspaceMovesLeftClampedAtColumnZero) {
  term.feed("AB");
  term.feed("\b");
  EXPECT_EQ(term.console().cursor_x(), 1u);
  term.feed("\b\b"); // One more than available: clamps at 0.
  EXPECT_EQ(term.console().cursor_x(), 0u);
}

TEST_F(Vt100Test, TabAdvancesToNextMultipleOfEightColumn) {
  term.feed("AB");
  term.feed("\t");
  EXPECT_EQ(term.console().cursor_x(), 8u);
}

TEST_F(Vt100Test, CupMovesCursorToOneBasedRowColumn) {
  term.feed("\x1b[3;5H");
  EXPECT_EQ(term.console().cursor_x(), 4u);
  EXPECT_EQ(term.console().cursor_y(), 2u);
}

TEST_F(Vt100Test, CupWithNoParametersDefaultsToHome) {
  term.feed("\x1b[5;5H\x1b[H");
  EXPECT_EQ(term.console().cursor_x(), 0u);
  EXPECT_EQ(term.console().cursor_y(), 0u);
}

TEST_F(Vt100Test, CuuCudCufCubMoveRelativeAndClampAtEdges) {
  term.feed("\x1b[3;3H");
  term.feed("\x1b[2A");
  EXPECT_EQ(term.console().cursor_y(), 0u);
  term.feed("\x1b[1B");
  EXPECT_EQ(term.console().cursor_y(), 1u);
  term.feed("\x1b[3C");
  EXPECT_EQ(term.console().cursor_x(), 5u);
  term.feed("\x1b[2D");
  EXPECT_EQ(term.console().cursor_x(), 3u);

  // Clamp: moving up/left past the edge stops at 0.
  term.feed("\x1b[1;1H");
  term.feed("\x1b[99A");
  EXPECT_EQ(term.console().cursor_y(), 0u);
  term.feed("\x1b[99D");
  EXPECT_EQ(term.console().cursor_x(), 0u);
}

TEST_F(Vt100Test, DefaultCountIsOneWhenOmittedOrExplicitZero) {
  term.feed("\x1b[3;3H");
  term.feed("\x1b[A"); // No param: default count 1.
  EXPECT_EQ(term.console().cursor_y(), 1u);
  term.feed("\x1b[0A"); // Explicit 0: also default count 1.
  EXPECT_EQ(term.console().cursor_y(), 0u);
}

TEST_F(Vt100Test, EdZeroErasesFromCursorToEndOfScreen) {
  for (std::size_t y = 0; y < kRows; ++y) {
    for (std::size_t x = 0; x < kCols; ++x) {
      ASSERT_TRUE(term.console().put_char(x, y, '#', console_color::white, console_color::black));
    }
  }
  term.feed("\x1b[3;3H\x1b[0J");
  EXPECT_EQ(cell_at(term.console(), 0, 0).ch, '#'); // Before cursor: untouched.
  EXPECT_EQ(cell_at(term.console(), 1, 2).ch, '#'); // Before cursor on cursor's row: untouched.
  EXPECT_EQ(cell_at(term.console(), 2, 2).ch, ' '); // At/after cursor: erased.
  EXPECT_EQ(cell_at(term.console(), 0, 4).ch, ' '); // Rows below cursor: erased.
}

TEST_F(Vt100Test, EdTwoErasesEntireScreen) {
  for (std::size_t x = 0; x < kCols; ++x) {
    ASSERT_TRUE(term.console().put_char(x, 0, '#', console_color::white, console_color::black));
  }
  term.feed("\x1b[2J");
  for (std::size_t x = 0; x < kCols; ++x) {
    EXPECT_EQ(cell_at(term.console(), x, 0).ch, ' ');
  }
}

TEST_F(Vt100Test, ElZeroErasesFromCursorToEndOfLine) {
  for (std::size_t x = 0; x < kCols; ++x) {
    ASSERT_TRUE(term.console().put_char(x, 0, '#', console_color::white, console_color::black));
  }
  term.feed("\x1b[1;5H\x1b[0K");
  EXPECT_EQ(cell_at(term.console(), 0, 0).ch, '#');
  EXPECT_EQ(cell_at(term.console(), 3, 0).ch, '#');
  EXPECT_EQ(cell_at(term.console(), 4, 0).ch, ' ');
  EXPECT_EQ(cell_at(term.console(), kCols - 1, 0).ch, ' ');
}

TEST_F(Vt100Test, SgrSetsStandardAndBrightForegroundAndBackground) {
  term.feed("\x1b[31m");
  EXPECT_EQ(term.console().foreground(), console_color::red);

  term.feed("\x1b[91m");
  EXPECT_EQ(term.console().foreground(), console_color::light_red);

  term.feed("\x1b[44m");
  EXPECT_EQ(term.console().background(), console_color::blue);

  term.feed("\x1b[104m");
  EXPECT_EQ(term.console().background(), console_color::light_blue);
}

TEST_F(Vt100Test, SgrBoldBrightensAStandardIntensityForeground) {
  term.feed("\x1b[1;32m"); // bold, then standard-intensity green.
  EXPECT_EQ(term.console().foreground(), console_color::light_green);

  term.feed("\x1b[22m"); // Normal intensity again, same color code.
  term.feed("\x1b[32m");
  EXPECT_EQ(term.console().foreground(), console_color::green);
}

TEST_F(Vt100Test, SgrDefaultAndResetRestoreStandardColors) {
  term.feed("\x1b[91;44m");
  term.feed("\x1b[39;49m");
  EXPECT_EQ(term.console().foreground(), console_color::light_gray);
  EXPECT_EQ(term.console().background(), console_color::black);

  term.feed("\x1b[1;91;44m");
  term.feed("\x1b[0m");
  EXPECT_EQ(term.console().foreground(), console_color::light_gray);
  EXPECT_EQ(term.console().background(), console_color::black);
}

TEST_F(Vt100Test, SgrWithNoParametersResets) {
  term.feed("\x1b[91m\x1b[m");
  EXPECT_EQ(term.console().foreground(), console_color::light_gray);
}

TEST_F(Vt100Test, UnsupportedEscapeSequenceIsConsumedWithoutLeakingBytes) {
  term.feed("\x1b(B"); // Charset-select, not supported: must be dropped silently.
  term.feed("X");
  EXPECT_EQ(cell_at(term.console(), 0, 0).ch, 'X');
  EXPECT_EQ(term.console().cursor_x(), 1u);
}

TEST_F(Vt100Test, FeedStringViewProcessesEveryByte) {
  term.feed("\x1b[2;2HHi");
  EXPECT_EQ(cell_at(term.console(), 1, 1).ch, 'H');
  EXPECT_EQ(cell_at(term.console(), 2, 1).ch, 'i');
}
