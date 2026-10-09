// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/hw/vt100.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace structo::hw;

namespace {

constexpr std::size_t kCols = 20;
constexpr std::size_t kRows = 6;

struct fake_console {
  reloco::array<console_cell, kCols * kRows> cells{};
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

TEST_F(Vt100Test, NulBelDelAndHighBytesAreIgnored) {
  term.feed(std::string_view("A\0\a\x7f\xe2\x9e\x9c" "B", 8));
  EXPECT_EQ(term.console().cursor_x(), 2u);
  EXPECT_EQ(cell_at(term.console(), 0, 0).ch, 'A');
  EXPECT_EQ(cell_at(term.console(), 1, 0).ch, 'B');
}

TEST_F(Vt100Test, PrivateModeAndIntermediateSequencesAreConsumedSilently) {
  term.feed("A\x1b[?1h\x1b=\x1b[?2004h\x1b[?25l\x1b[1 qB");
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

namespace {

// reloco::string_view refuses temporary std::strings; bind to a named one first.
void feed_str(vt100_terminal &t, const std::string &s) { t.feed(reloco::string_view(s)); }

std::string row_text(const console_ref &c, std::size_t y) {
  std::string s;
  for (std::size_t x = 0; x < kCols; ++x)
    s += cell_at(c, x, y).ch;
  while (!s.empty() && s.back() == ' ')
    s.pop_back();
  return s;
}

// One letter per row: row 0 = 'A', row 1 = 'B', ...
void fill_rows(vt100_terminal &t) {
  for (std::size_t y = 0; y < kRows; ++y) {
    std::string s = "\x1b[" + std::to_string(y + 1) + ";1H";
    s += static_cast<char>('A' + y);
    feed_str(t, s);
  }
}

struct event_log {
  int bells = 0;
  int resets = 0;
  std::string title;
  std::vector<std::pair<std::uint32_t, std::string>> oscs;
  std::vector<std::pair<vt100_mode, bool>> modes;
  std::vector<std::uint32_t> styles;
  std::string replies;
};

vt100_callbacks make_callbacks(event_log &log) {
  vt100_callbacks cb;
  cb.ctx = &log;
  cb.bell = [](void *c) noexcept { ++static_cast<event_log *>(c)->bells; };
  cb.title = [](void *c, reloco::string_view s) noexcept {
    static_cast<event_log *>(c)->title.assign(s.data(), s.size());
  };
  cb.osc = [](void *c, std::uint32_t code, reloco::string_view s) noexcept {
    static_cast<event_log *>(c)->oscs.emplace_back(code, std::string(s.data(), s.size()));
  };
  cb.mode = [](void *c, vt100_mode m, bool on) noexcept { static_cast<event_log *>(c)->modes.emplace_back(m, on); };
  cb.cursor_style = [](void *c, std::uint32_t s) noexcept { static_cast<event_log *>(c)->styles.push_back(s); };
  cb.reply = [](void *c, reloco::string_view s) noexcept { static_cast<event_log *>(c)->replies.append(s.data(), s.size()); };
  cb.reset = [](void *c) noexcept { ++static_cast<event_log *>(c)->resets; };
  return cb;
}

} // namespace

TEST_F(Vt100Test, DeferredWrapDoesNotInsertBlankRowAfterFullLine) {
  feed_str(term, std::string(kCols, 'x'));
  EXPECT_EQ(term.console().cursor_x(), kCols - 1);
  EXPECT_EQ(term.console().cursor_y(), 0u);
  term.feed("\r\n");
  EXPECT_EQ(term.console().cursor_y(), 1u);
  EXPECT_EQ(term.console().cursor_x(), 0u);

  feed_str(term, std::string(kCols, 'y') + "z"); // the 21st character wraps first
  EXPECT_EQ(cell_at(term.console(), 0, 2).ch, 'z');
}

TEST_F(Vt100Test, AutowrapCanBeDisabled) {
  term.feed("\x1b[?7l");
  feed_str(term, std::string(kCols, 'x') + "!");
  EXPECT_EQ(term.console().cursor_y(), 0u);
  EXPECT_EQ(cell_at(term.console(), kCols - 1, 0).ch, '!'); // overwrote the last column
  term.feed("\x1b[?7h");
}

TEST_F(Vt100Test, ScrollRegionScrollsOnlyInsideMargins) {
  fill_rows(term);
  term.feed("\x1b[2;4r"); // rows 2..4 (1-based), cursor homes
  term.feed("\x1b[4;1H\n"); // LF on the region's bottom row
  EXPECT_EQ(row_text(term.console(), 0), "A");
  EXPECT_EQ(row_text(term.console(), 1), "C");
  EXPECT_EQ(row_text(term.console(), 2), "D");
  EXPECT_EQ(row_text(term.console(), 3), "");
  EXPECT_EQ(row_text(term.console(), 4), "E");
  EXPECT_EQ(row_text(term.console(), 5), "F");
}

TEST_F(Vt100Test, ReverseIndexAtRegionTopScrollsDown) {
  fill_rows(term);
  term.feed("\x1b[1;1H\x1bM");
  EXPECT_EQ(row_text(term.console(), 0), "");
  EXPECT_EQ(row_text(term.console(), 1), "A");
  EXPECT_EQ(row_text(term.console(), 5), "E");
}

TEST_F(Vt100Test, InsertAndDeleteLines) {
  fill_rows(term);
  term.feed("\x1b[2;1H\x1b[L"); // insert one line at row 2
  EXPECT_EQ(row_text(term.console(), 1), "");
  EXPECT_EQ(row_text(term.console(), 2), "B");
  EXPECT_EQ(row_text(term.console(), 5), "E");
  term.feed("\x1b[M"); // delete it again
  EXPECT_EQ(row_text(term.console(), 1), "B");
  EXPECT_EQ(row_text(term.console(), 5), "");
}

TEST_F(Vt100Test, InsertDeleteEraseCharacters) {
  term.feed("abcdef");
  term.feed("\x1b[1;3H\x1b[2P"); // delete "cd"
  EXPECT_EQ(row_text(term.console(), 0), "abef");
  term.feed("\x1b[2@"); // insert two blanks at the cursor
  EXPECT_EQ(row_text(term.console(), 0), "ab  ef");
  term.feed("\x1b[3X"); // erase three cells, no shifting
  EXPECT_EQ(row_text(term.console(), 0), "ab   f");
}

TEST_F(Vt100Test, CursorSaveRestoreIncludesColors) {
  term.feed("\x1b[2;5H\x1b[31m\x1b" "7");
  term.feed("\x1b[1;1H\x1b[0m");
  term.feed("\x1b" "8");
  EXPECT_EQ(term.console().cursor_x(), 4u);
  EXPECT_EQ(term.console().cursor_y(), 1u);
  EXPECT_EQ(term.console().foreground(), console_color::red);
  term.feed("\x1b[1;1H\x1b[s\x1b[3;3H\x1b[u"); // CSI s / CSI u form
  EXPECT_EQ(term.console().cursor_x(), 0u);
}

TEST_F(Vt100Test, ExtendedColorsMapToNearestOf16) {
  term.feed("\x1b[38;5;196m"); // 256-colour pure red
  EXPECT_EQ(term.console().foreground(), console_color::red);
  term.feed("\x1b[48;2;0;0;170m"); // true-colour blue
  EXPECT_EQ(term.console().background(), console_color::blue);
  term.feed("\x1b[1;38;5;2m\x1b[m"); // several params in one sequence, then reset
  EXPECT_EQ(term.console().foreground(), console_color::light_gray);
  term.feed("\x1b[31;7m"); // reverse video swaps fg and bg
  EXPECT_EQ(term.console().foreground(), console_color::black);
  EXPECT_EQ(term.console().background(), console_color::red);
}

TEST_F(Vt100Test, RelativeAndAbsoluteCursorForms) {
  term.feed("\x1b[3;4H\x1b[2E"); // CNL
  EXPECT_EQ(term.console().cursor_x(), 0u);
  EXPECT_EQ(term.console().cursor_y(), 4u);
  term.feed("\x1b[2F\x1b[7G\x1b[6d"); // CPL, CHA, VPA
  EXPECT_EQ(term.console().cursor_x(), 6u);
  EXPECT_EQ(term.console().cursor_y(), 5u);
}

TEST_F(Vt100Test, OscIsConsumedAndReportedWithEitherTerminator) {
  event_log log;
  term.set_callbacks(make_callbacks(log));
  term.feed("A\x1b]0;my title\x07" "B\x1b]2;second\x1b\\" "C\x1b]7;file:///tmp\x07");
  EXPECT_EQ(row_text(term.console(), 0), "ABC"); // no payload leaked onto the screen
  EXPECT_EQ(log.title, "second");
  ASSERT_EQ(log.oscs.size(), 1u);
  EXPECT_EQ(log.oscs[0].first, 7u);
  EXPECT_EQ(log.oscs[0].second, "file:///tmp");
}

TEST_F(Vt100Test, OscSplitAcrossFeedsAndTruncated) {
  event_log log;
  term.set_callbacks(make_callbacks(log));
  term.feed("\x1b]0;ab");
  term.feed("cd\x07");
  EXPECT_EQ(log.title, "abcd");
  feed_str(term, "\x1b]0;" + std::string(400, 'z') + "\x07");
  EXPECT_EQ(log.title.size(), 255u);
}

TEST_F(Vt100Test, BellAndResetCallbacks) {
  event_log log;
  term.set_callbacks(make_callbacks(log));
  term.feed("a\x07\x07");
  EXPECT_EQ(log.bells, 2);
  EXPECT_EQ(term.console().cursor_x(), 1u); // BEL draws nothing
  term.feed("\x1b[31m\x1b[3;3H\x1b" "c");
  EXPECT_EQ(log.resets, 1);
  EXPECT_EQ(row_text(term.console(), 0), "");
  EXPECT_EQ(term.console().cursor_x(), 0u);
  EXPECT_EQ(term.console().foreground(), console_color::light_gray);
}

TEST_F(Vt100Test, PrivateModeAndStyleCallbacks) {
  event_log log;
  term.set_callbacks(make_callbacks(log));
  term.feed("\x1b[?2004h\x1b[?1;25l\x1b=\x1b>\x1b[5 q");
  ASSERT_EQ(log.modes.size(), 5u);
  EXPECT_EQ(log.modes[0], std::make_pair(vt100_mode::bracketed_paste, true));
  EXPECT_EQ(log.modes[1], std::make_pair(vt100_mode::application_cursor_keys, false));
  EXPECT_EQ(log.modes[2], std::make_pair(vt100_mode::cursor_visible, false));
  EXPECT_EQ(log.modes[3], std::make_pair(vt100_mode::application_keypad, true));
  EXPECT_EQ(log.modes[4], std::make_pair(vt100_mode::application_keypad, false));
  ASSERT_EQ(log.styles.size(), 1u);
  EXPECT_EQ(log.styles[0], 5u);
  EXPECT_EQ(row_text(term.console(), 0), "");
}

TEST_F(Vt100Test, StatusAndAttributeQueriesProduceReplies) {
  event_log log;
  term.set_callbacks(make_callbacks(log));
  term.feed("\x1b[3;5H\x1b[6n");
  EXPECT_EQ(log.replies, "\x1b[3;5R");
  log.replies.clear();
  term.feed("\x1b[5n\x1b[c");
  EXPECT_EQ(log.replies, "\x1b[0n\x1b[?1;2c");
  log.replies.clear();
  term.feed("\x1b[>c"); // secondary DA is not implemented: no reply, and nothing printed
  EXPECT_EQ(log.replies, "");
  EXPECT_EQ(row_text(term.console(), 0), "");
}

TEST_F(Vt100Test, EventsWithoutCallbacksAreHarmless) {
  term.feed("\x07\x1b]0;t\x07\x1b[6n\x1b[?2004h\x1b[2 q");
  EXPECT_EQ(row_text(term.console(), 0), "");
}

TEST_F(Vt100Test, DecSpecialGraphicsDrawsAsciiLookalikesViaG0AndShift) {
  term.feed("\x1b(0lqqk\x1b(B|"); // G0 = graphics: box corner/line/corner, then back to ASCII
  EXPECT_EQ(row_text(term.console(), 0), "+--+|");
  term.feed("\r\n\x1b)0\x0e" "x\x0f" "x"); // G1 = graphics, SO selects it, SI returns to G0 (ASCII)
  EXPECT_EQ(row_text(term.console(), 1), "|x");
}

TEST_F(Vt100Test, ModeGettersTrackProgramRequests) {
  EXPECT_FALSE(term.application_cursor_keys());
  term.feed("\x1b[?1h\x1b=\x1b[?2004h");
  EXPECT_TRUE(term.application_cursor_keys());
  EXPECT_TRUE(term.application_keypad());
  EXPECT_TRUE(term.bracketed_paste());
  term.feed("\x1b[?1l\x1b>\x1b[?2004l");
  EXPECT_FALSE(term.application_cursor_keys());
  EXPECT_FALSE(term.application_keypad());
  EXPECT_FALSE(term.bracketed_paste());
  term.feed("\x1b[?1h\x1b" "c"); // RIS clears them
  EXPECT_FALSE(term.application_cursor_keys());
}

TEST_F(Vt100Test, AlternateScreenSavesAndRestoresMainScreen) {
  reloco::array<console_cell, kCols * kRows> storage{};
  term.set_alternate_screen_storage(reloco::span<console_cell>(storage.data(), storage.size()));
  term.feed("main\x1b[3;3H");
  term.feed("\x1b[?1049h");
  EXPECT_TRUE(term.alternate_screen());
  EXPECT_EQ(row_text(term.console(), 0), ""); // blank alternate screen
  term.feed("ALT");
  EXPECT_EQ(row_text(term.console(), 0), "ALT");
  term.feed("\x1b[?1049l");
  EXPECT_FALSE(term.alternate_screen());
  EXPECT_EQ(row_text(term.console(), 0), "main");
  EXPECT_EQ(term.console().cursor_x(), 2u);
  EXPECT_EQ(term.console().cursor_y(), 2u);
}

TEST_F(Vt100Test, AlternateScreenWithoutStorageIsOnlyReported) {
  event_log log;
  term.set_callbacks(make_callbacks(log));
  term.feed("keep\x1b[?1049h");
  EXPECT_FALSE(term.alternate_screen());
  EXPECT_EQ(row_text(term.console(), 0), "keep");
  ASSERT_EQ(log.modes.size(), 1u);
  EXPECT_EQ(log.modes[0], std::make_pair(vt100_mode::alt_screen_save, true));
}

TEST_F(Vt100Test, MouseModesAreTracked) {
  EXPECT_EQ(term.mouse_tracking(), vt100_mouse_tracking::off);
  term.feed("\x1b[?1000h");
  EXPECT_EQ(term.mouse_tracking(), vt100_mouse_tracking::normal);
  term.feed("\x1b[?1002h\x1b[?1006h");
  EXPECT_EQ(term.mouse_tracking(), vt100_mouse_tracking::button);
  EXPECT_TRUE(term.mouse_sgr());
  term.feed("\x1b[?1000l"); // disabling a level that is not current leaves the active one alone
  EXPECT_EQ(term.mouse_tracking(), vt100_mouse_tracking::button);
  term.feed("\x1b[?1002l\x1b[?1006l");
  EXPECT_EQ(term.mouse_tracking(), vt100_mouse_tracking::off);
  EXPECT_FALSE(term.mouse_sgr());
  term.feed("\x1b[?1003h\x1b" "c");
  EXPECT_EQ(term.mouse_tracking(), vt100_mouse_tracking::off);
}
