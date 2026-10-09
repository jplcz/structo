// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/uart_input.hpp>

#include <deque>
#include <string>
#include <vector>

using namespace structo::hw;

namespace {

struct fake_uart {
  std::deque<std::uint8_t> rx;
};

} // namespace

template <> struct structo::hw::uart_traits<fake_uart> {
  static reloco::result<void> configure(fake_uart &, const uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(fake_uart &) noexcept { return true; }
  static reloco::result<bool> rx_ready(fake_uart &b) noexcept { return !b.rx.empty(); }
  static reloco::result<void> try_put_byte(fake_uart &, std::uint8_t) noexcept { return {}; }
  static reloco::result<std::uint8_t> try_get_byte(fake_uart &b) noexcept {
    if (b.rx.empty())
      return reloco::unexpected(reloco::error::try_again);
    auto v = b.rx.front();
    b.rx.pop_front();
    return v;
  }
};

namespace {

constexpr std::uint16_t K(hid_key k) { return static_cast<std::uint16_t>(k); }
constexpr std::uint16_t L(char c) { return static_cast<std::uint16_t>(0x04 + (c - 'a')); }

struct UartInputTest : ::testing::Test {
  fake_uart port;
  uart_input_config cfg{3}; // tiny escape timeout to keep tests fast
  uart_input decoder{uart_ref(port), cfg};
  input_device_ref dev{decoder};

  void send(const std::string &s) { port.rx.insert(port.rx.end(), s.begin(), s.end()); }

  std::vector<input_event> events() {
    std::vector<input_event> v;
    for (;;) {
      auto e = dev.try_read_event();
      if (!e)
        break;
      v.push_back(e.value());
    }
    return v;
  }

  // Key-down events only, as "usage" values, for compact comparisons.
  std::vector<std::uint16_t> presses() {
    std::vector<std::uint16_t> v;
    for (auto &e : events())
      if (e.type == input_event_type::key && e.value == static_cast<std::int32_t>(input_key_state::pressed))
        v.push_back(e.code);
    return v;
  }
};

} // namespace

TEST_F(UartInputTest, CapabilitiesAndIdle) {
  EXPECT_TRUE(dev.capabilities().has(input_class::keyboard));
  EXPECT_FALSE(dev.capabilities().supports_callback);
  EXPECT_FALSE(dev.event_ready().value());
  EXPECT_EQ(dev.try_read_event().error(), reloco::error::try_again);
}

TEST_F(UartInputTest, PlainKeyIsPressReleaseSync) {
  send("a");
  auto ev = events();
  ASSERT_EQ(ev.size(), 3u);
  EXPECT_EQ(ev[0], make_key_event(L('a'), input_key_state::pressed));
  EXPECT_EQ(ev[1], make_key_event(L('a'), input_key_state::released));
  EXPECT_EQ(ev[2].type, input_event_type::sync);
}

TEST_F(UartInputTest, UppercaseAndSymbolsWrapInShift) {
  send("A");
  auto ev = events();
  ASSERT_EQ(ev.size(), 5u);
  EXPECT_EQ(ev[0], make_key_event(K(hid_key::left_shift), input_key_state::pressed));
  EXPECT_EQ(ev[1], make_key_event(L('a'), input_key_state::pressed));
  EXPECT_EQ(ev[2], make_key_event(L('a'), input_key_state::released));
  EXPECT_EQ(ev[3], make_key_event(K(hid_key::left_shift), input_key_state::released));

  send("!?");
  auto p = presses();
  EXPECT_EQ(p, (std::vector<std::uint16_t>{K(hid_key::left_shift), K(hid_key::n1), K(hid_key::left_shift),
                                           K(hid_key::slash)}));
}

TEST_F(UartInputTest, ControlCharacters) {
  send(std::string("\x03", 1)); // Ctrl-C
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::left_ctrl), L('c')}));
  send("\r\t\x7f");
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::enter), K(hid_key::tab), K(hid_key::backspace)}));
}

TEST_F(UartInputTest, ArrowsAndNavigationBothIntroducers) {
  send("\x1b[A\x1bOB\x1b[C\x1b[D\x1b[H\x1b[F\x1b[3~\x1b[5~\x1b[2~");
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::up), K(hid_key::down), K(hid_key::right),
                                                   K(hid_key::left), K(hid_key::home), K(hid_key::end),
                                                   K(hid_key::delete_key), K(hid_key::page_up), K(hid_key::insert)}));
}

TEST_F(UartInputTest, FunctionKeys) {
  send("\x1bOP\x1bOS\x1b[15~\x1b[17~\x1b[21~\x1b[23~\x1b[24~");
  const auto f = [](unsigned n) { return static_cast<std::uint16_t>(K(hid_key::f1) + n - 1); };
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{f(1), f(4), f(5), f(6), f(10), f(11), f(12)}));
}

TEST_F(UartInputTest, XtermModifierParameters) {
  send("\x1b[1;5A"); // Ctrl+Up
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::left_ctrl), K(hid_key::up)}));
  send("\x1b[3;2~"); // Shift+Delete
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::left_shift), K(hid_key::delete_key)}));
  send("\x1b[1;3P"); // Alt+F1
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::left_alt), K(hid_key::f1)}));
  send("\x1b[Z"); // back-tab
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::left_shift), K(hid_key::tab)}));
}

TEST_F(UartInputTest, AltCharacter) {
  send("\x1b"
       "x");
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::left_alt), L('x')}));
}

TEST_F(UartInputTest, LoneEscapeNeedsIdlePolls) {
  send("\x1b");
  EXPECT_FALSE(dev.event_ready().value()); // idle poll 1
  EXPECT_FALSE(dev.event_ready().value()); // 2
  EXPECT_TRUE(dev.event_ready().value());  // 3: timeout reached
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::escape)}));
}

TEST_F(UartInputTest, SplitSequenceIsReassembled) {
  send("\x1b[");
  EXPECT_FALSE(dev.event_ready().value());
  send("1;5");
  EXPECT_FALSE(dev.event_ready().value());
  send("A");
  EXPECT_EQ(presses(), (std::vector<std::uint16_t>{K(hid_key::left_ctrl), K(hid_key::up)}));
}

TEST_F(UartInputTest, SgrMouse) {
  send("\x1b[<0;11;6M"); // left press at column 11, row 6 (1-based)
  auto ev = events();
  ASSERT_EQ(ev.size(), 4u);
  EXPECT_EQ(ev[0], make_abs_event(input_axis::x, 10));
  EXPECT_EQ(ev[1], make_abs_event(input_axis::y, 5));
  EXPECT_EQ(ev[2], make_button_event(input_button::left, true));
  EXPECT_EQ(ev[3].type, input_event_type::sync);

  send("\x1b[<0;11;6m");
  ev = events();
  EXPECT_EQ(ev[2], make_button_event(input_button::left, false));

  send("\x1b[<64;1;1M"); // wheel up
  ev = events();
  ASSERT_EQ(ev.size(), 4u);
  EXPECT_EQ(ev[2], make_rel_event(input_axis::wheel, 1));
  send("\x1b[<65;1;1M"); // wheel down
  ev = events();
  EXPECT_EQ(ev[2], make_rel_event(input_axis::wheel, -1));
}

TEST_F(UartInputTest, UnknownSequencesAndHighBytesAreDropped) {
  send("\x1b[99;99Z\xc3\xa9q");
  // "ESC [ 99;99 Z" decodes as back-tab (Z ignores params); the UTF-8 bytes vanish; 'q' survives.
  auto p = presses();
  ASSERT_FALSE(p.empty());
  EXPECT_EQ(p.back(), L('q'));
}
