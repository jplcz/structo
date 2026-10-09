// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <structo/hw/console_uart.hpp>

#include <deque>
#include <string>

using namespace structo::hw;

namespace {

constexpr std::size_t kCols = 20;
constexpr std::size_t kRows = 6;

struct fake_console {
  reloco::array<console_cell, kCols * kRows> cells{};
};

struct fake_keyboard {
  std::deque<input_event> events;
  std::uint8_t leds = 0;
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

template <> struct structo::hw::input_traits<fake_keyboard> {
  static input_capabilities capabilities(const fake_keyboard &) noexcept {
    return {static_cast<std::uint32_t>(input_class::keyboard)};
  }
  static result<bool> event_ready(fake_keyboard &b) noexcept { return !b.events.empty(); }
  static result<input_event> try_read_event(fake_keyboard &b) noexcept {
    if (b.events.empty())
      return reloco::unexpected(reloco::error::try_again);
    auto e = b.events.front();
    b.events.pop_front();
    return e;
  }
  static result<void> set_leds(fake_keyboard &b, std::uint8_t m) noexcept {
    b.leds = m;
    return {};
  }
};

namespace {

constexpr std::uint16_t K(hid_key k) { return static_cast<std::uint16_t>(k); }
// HID letters are contiguous from `a` (0x04).
constexpr std::uint16_t L(char c) { return static_cast<std::uint16_t>(0x04 + (c - 'a')); }

struct ConsoleUartTest : ::testing::Test {
  fake_console screen;
  fake_keyboard kbd;
  console_uart bridge{input_device_ref(kbd), console_ref(screen)};
  uart_ref uart{bridge};

  void tap(std::uint16_t code) {
    kbd.events.push_back(make_key_event(code, input_key_state::pressed));
    kbd.events.push_back(make_key_event(code, input_key_state::released));
  }
  void press(hid_key k) { kbd.events.push_back(make_key_event(K(k), input_key_state::pressed)); }
  void release(hid_key k) { kbd.events.push_back(make_key_event(K(k), input_key_state::released)); }

  // Drains every translated byte currently available.
  std::string drain() {
    std::string s;
    reloco::array<std::uint8_t, 16> buf{};
    for (;;) {
      auto n = uart.read_available(reloco::span<std::uint8_t>(buf.data(), buf.size()));
      if (!n || n.value() == 0)
        break;
      s.append(reinterpret_cast<const char *>(buf.data()), n.value());
    }
    return s;
  }

  char cell(std::size_t x, std::size_t y) const { return screen.cells[y * kCols + x].ch; }
};

} // namespace

TEST_F(ConsoleUartTest, TxDrawsOnConsoleAndLoneNewlineReturnsCarriage) {
  ASSERT_TRUE(uart.write_string("AB\nC")); // write_string already emits "\r\n"
  EXPECT_EQ(cell(0, 0), 'A');
  EXPECT_EQ(cell(1, 0), 'B');
  EXPECT_EQ(cell(0, 1), 'C'); // carriage returned, one row down
}

TEST_F(ConsoleUartTest, RawLfIsExpandedToCrLf) {
  const reloco::array<std::uint8_t, 3> data{{'A', '\n', 'B'}};
  ASSERT_TRUE(uart.write(data)); // raw write(): bridge supplies the CR
  EXPECT_EQ(cell(0, 0), 'A');
  EXPECT_EQ(cell(0, 1), 'B');
}

TEST_F(ConsoleUartTest, CrLfIsNotDoubled) {
  const reloco::array<std::uint8_t, 4> data{{'A', '\r', '\n', 'B'}};
  ASSERT_TRUE(uart.write(data));
  EXPECT_EQ(cell(0, 0), 'A');
  EXPECT_EQ(cell(0, 1), 'B');
  EXPECT_EQ(bridge.terminal().console().cursor_y(), 1u);
}

TEST_F(ConsoleUartTest, AnsiSequencesReachTheVt100) {
  ASSERT_TRUE(uart.write_string("\x1b[2;3HX"));
  EXPECT_EQ(cell(2, 1), 'X');
}

TEST_F(ConsoleUartTest, RxNothingPending) {
  EXPECT_FALSE(uart.rx_ready().value());
  EXPECT_EQ(uart.try_get_byte().error(), reloco::error::try_again);
}

TEST_F(ConsoleUartTest, PrintableKeysAndShift) {
  tap(K(hid_key::a));
  press(hid_key::left_shift);
  tap(K(hid_key::a));
  tap(K(hid_key::n1));
  release(hid_key::left_shift);
  tap(K(hid_key::n1));
  EXPECT_EQ(drain(), "aA!1");
}

TEST_F(ConsoleUartTest, SpecialKeysAsSerialTerminalBytes) {
  tap(K(hid_key::enter));
  tap(K(hid_key::backspace));
  tap(K(hid_key::tab));
  tap(K(hid_key::up));
  tap(K(hid_key::delete_key));
  EXPECT_EQ(drain(), std::string("\r\x7f\t\x1b[A\x1b[3~"));
}

TEST_F(ConsoleUartTest, CtrlLettersAndNonKeyEventsIgnored) {
  press(hid_key::left_ctrl);
  tap(L('c'));
  tap(L('u'));
  release(hid_key::left_ctrl);
  kbd.events.push_back(make_rel_event(input_axis::x, 5));
  tap(L('x'));
  EXPECT_EQ(drain(), std::string("\x03\x15x"));
}

TEST_F(ConsoleUartTest, FunctionKeysUseXtermSequences) {
  const reloco::array<std::string, 12> expected{{"\x1bOP",   "\x1bOQ",   "\x1bOR",   "\x1bOS",   "\x1b[15~", "\x1b[17~",
                                                 "\x1b[18~", "\x1b[19~", "\x1b[20~", "\x1b[21~", "\x1b[23~", "\x1b[24~"}};
  for (std::uint16_t i = 0; i < 12; ++i) {
    tap(static_cast<std::uint16_t>(K(hid_key::f1) + i));
    EXPECT_EQ(drain(), expected[i]) << "F" << (i + 1);
  }
}

TEST_F(ConsoleUartTest, CapsLockTogglesLettersAndLed) {
  tap(K(hid_key::caps_lock));
  EXPECT_FALSE(uart.rx_ready().value()); // events are consumed lazily, on polling
  EXPECT_EQ(kbd.leds, static_cast<std::uint8_t>(input_led::caps_lock));
  std::string out;
  tap(L('a'));
  press(hid_key::right_shift);
  tap(L('a')); // shift inverts caps
  release(hid_key::right_shift);
  tap(K(hid_key::n1)); // caps does not affect digits
  out += drain();
  tap(K(hid_key::caps_lock));
  EXPECT_FALSE(uart.rx_ready().value());
  EXPECT_EQ(kbd.leds, 0);
  tap(L('a'));
  out += drain();
  EXPECT_EQ(out, "Aa1a");
}

TEST_F(ConsoleUartTest, ConfigureIsAccepted) {
  uart_config cfg;
  cfg.baud_rate = 9600;
  EXPECT_TRUE(uart.configure(cfg));
  EXPECT_EQ(uart.current_config().value().baud_rate, 9600u);
}

TEST_F(ConsoleUartTest, GetByteBlocksOnKeyboardThenTimesOut) {
  EXPECT_EQ(uart.get_byte(10).error(), reloco::error::timed_out);
  tap(L('q'));
  EXPECT_EQ(uart.get_byte(10).value(), 'q');
}

TEST_F(ConsoleUartTest, ApplicationCursorKeysSendSs3) {
  tap(K(hid_key::up));
  EXPECT_EQ(drain(), "\x1b[A");
  ASSERT_TRUE(uart.write_string("\x1b[?1h")); // program (e.g. mc via smkx) asks for application mode
  tap(K(hid_key::up));
  tap(K(hid_key::home));
  EXPECT_EQ(drain(), "\x1bOA\x1bOH");
  ASSERT_TRUE(uart.write_string("\x1b[?1l"));
  tap(K(hid_key::up));
  EXPECT_EQ(drain(), "\x1b[A");
}

namespace {
void move_to(ConsoleUartTest &t, std::int32_t x, std::int32_t y) {
  t.kbd.events.push_back(make_abs_event(input_axis::x, x));
  t.kbd.events.push_back(make_abs_event(input_axis::y, y));
  t.kbd.events.push_back(make_sync_event());
}
} // namespace

TEST_F(ConsoleUartTest, PointerIsSilentUntilProgramEnablesMouseReporting) {
  move_to(*this, 3, 2);
  kbd.events.push_back(make_button_event(input_button::left, true));
  EXPECT_EQ(drain(), "");
}

TEST_F(ConsoleUartTest, SgrMouseReportsButtonsAndWheel) {
  ASSERT_TRUE(uart.write_string("\x1b[?1000h\x1b[?1006h"));
  move_to(*this, 3, 2); // no button held: normal tracking reports no motion
  kbd.events.push_back(make_button_event(input_button::left, true));
  kbd.events.push_back(make_button_event(input_button::left, false));
  kbd.events.push_back(make_button_event(input_button::right, true));
  kbd.events.push_back(make_rel_event(input_axis::wheel, 1));
  kbd.events.push_back(make_rel_event(input_axis::wheel, -1));
  EXPECT_EQ(drain(), "\x1b[<0;4;3M\x1b[<0;4;3m\x1b[<2;4;3M\x1b[<64;4;3M\x1b[<65;4;3M");
}

TEST_F(ConsoleUartTest, LegacyMouseEncodingAndModifiers) {
  ASSERT_TRUE(uart.write_string("\x1b[?1000h"));
  move_to(*this, 0, 0);
  kbd.events.push_back(make_button_event(input_button::middle, true));
  kbd.events.push_back(make_button_event(input_button::middle, false));
  EXPECT_EQ(drain(), "\x1b[M!!!\x1b[M#!!"); // 32+1 / release = 32+3, coords 1,1 -> 33
  press(hid_key::left_ctrl);
  kbd.events.push_back(make_button_event(input_button::left, true));
  EXPECT_EQ(drain(), "\x1b[M0!!"); // 32 + 0 + 16 (ctrl)
}

TEST_F(ConsoleUartTest, DragMotionReportedOnlyInButtonOrAnyMode) {
  ASSERT_TRUE(uart.write_string("\x1b[?1002h\x1b[?1006h"));
  move_to(*this, 1, 1);
  EXPECT_EQ(drain(), ""); // button mode: hovering is not reported
  kbd.events.push_back(make_button_event(input_button::left, true));
  EXPECT_EQ(drain(), "\x1b[<0;2;2M");
  move_to(*this, 2, 1);
  EXPECT_EQ(drain(), "\x1b[<32;3;2M"); // motion bit 32 + held left button
  ASSERT_TRUE(uart.write_string("\x1b[?1003h"));
  kbd.events.push_back(make_button_event(input_button::left, false));
  (void)drain();
  move_to(*this, 3, 1);
  EXPECT_EQ(drain(), "\x1b[<35;4;2M"); // any-motion with nothing held
  ASSERT_TRUE(uart.write_string("\x1b[?1003l"));
  move_to(*this, 4, 1);
  EXPECT_EQ(drain(), "");
}

TEST_F(ConsoleUartTest, RelativePointerMotionIsClampedToTheScreen) {
  ASSERT_TRUE(uart.write_string("\x1b[?1003h\x1b[?1006h"));
  kbd.events.push_back(make_rel_event(input_axis::x, 1000));
  kbd.events.push_back(make_rel_event(input_axis::y, -5));
  kbd.events.push_back(make_sync_event());
  EXPECT_EQ(drain(), "\x1b[<35;" + std::to_string(kCols) + ";1M");
}
