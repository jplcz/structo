// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/text_editor.hpp>

#include <reloco/array.hpp>
#include <reloco/inline_string.hpp>

using namespace structo;
using namespace structo::bootldr;

namespace {

struct fake_uart {
  reloco::inline_string<4096> tx;
};

} // namespace

template <> struct structo::hw::uart_traits<fake_uart> {
  static reloco::result<void> configure(fake_uart &, const hw::uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(fake_uart &) noexcept { return true; }
  static reloco::result<bool> rx_ready(fake_uart &) noexcept { return false; }
  static reloco::result<void> try_put_byte(fake_uart &b, std::uint8_t v) noexcept {
    (void)b.tx.try_push_back(static_cast<char>(v));
    return {};
  }
  static reloco::result<std::uint8_t> try_get_byte(fake_uart &) noexcept {
    return reloco::unexpected(reloco::error::try_again);
  }
};

namespace {

// Feeds raw terminal bytes through a decoder into the editor.
void type(text_editor &ed, key_decoder &dec, reloco::string_view bytes) {
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    const key_event ev = dec.feed(static_cast<std::uint8_t>(bytes[i]));
    if (ev.code != key_code::none) {
      ASSERT_TRUE(ed.handle(ev).has_value());
    }
  }
}

} // namespace

TEST(KeyDecoder, DecodesKeysAndSequences) {
  key_decoder d;
  EXPECT_EQ(d.feed('a').code, key_code::character);
  EXPECT_EQ(d.feed('\r').code, key_code::enter);
  EXPECT_EQ(d.feed('\n').code, key_code::none); // CRLF is a single Enter
  EXPECT_EQ(d.feed('\n').code, key_code::enter);
  EXPECT_EQ(d.feed(0x7f).code, key_code::backspace);
  EXPECT_EQ(d.feed(0x13).code, key_code::save);
  EXPECT_EQ(d.feed(0x18).code, key_code::cancel);

  EXPECT_EQ(d.feed(0x1b).code, key_code::none);
  EXPECT_EQ(d.feed('[').code, key_code::none);
  EXPECT_EQ(d.feed('A').code, key_code::up);
  d.feed(0x1b);
  d.feed('[');
  d.feed('3');
  EXPECT_EQ(d.feed('~').code, key_code::del);
  d.feed(0x1b);
  d.feed('[');
  d.feed('1');
  d.feed(';');
  d.feed('5'); // Ctrl modifier is ignored
  EXPECT_EQ(d.feed('C').code, key_code::right);
  d.feed(0x1b);
  EXPECT_EQ(d.feed('O').code, key_code::none);
  EXPECT_EQ(d.feed('H').code, key_code::home);
  d.feed(0x1b);
  d.feed('x'); // unknown Alt sequence is dropped
  EXPECT_EQ(d.feed('b').code, key_code::character);
}

TEST(TextEditor, InsertsAndEditsDynamicBuffer) {
  reloco::string s;
  text_editor ed(s);
  key_decoder dec;
  type(ed, dec, "hello\rworld");
  EXPECT_EQ(s.view(), "hello\nworld");
  EXPECT_EQ(ed.line(), 1u);
  EXPECT_EQ(ed.column(), 5u);
  EXPECT_TRUE(ed.modified());

  type(ed, dec, "\x7f\x7f"); // backspace twice
  EXPECT_EQ(s.view(), "hello\nwor");
  type(ed, dec, "\x1b[H"); // Home
  EXPECT_EQ(ed.cursor(), 6u);
  type(ed, dec, "\x1b[3~"); // Delete 'w'
  EXPECT_EQ(s.view(), "hello\nor");
  type(ed, dec, "\x1b[A"); // up
  EXPECT_EQ(ed.cursor(), 0u);
  type(ed, dec, "\x1b[F"); // End
  EXPECT_EQ(ed.cursor(), 5u);
  type(ed, dec, "\x1b[B"); // down keeps the column (clamped to the shorter line)
  EXPECT_EQ(ed.cursor(), 8u);
}

TEST(TextEditor, StickyColumnAndLineJoin) {
  reloco::string s;
  ASSERT_TRUE(s.try_assign("abcdef\nxy\n123456").has_value());
  text_editor ed(s);
  key_decoder dec;
  type(ed, dec, "\x1b[C\x1b[C\x1b[C\x1b[C"); // column 4
  type(ed, dec, "\x1b[B");                   // short line: clamps to 2
  EXPECT_EQ(ed.column(), 2u);
  type(ed, dec, "\x1b[B"); // column 4 again on the third line
  EXPECT_EQ(ed.column(), 4u);

  text_editor ed2(s);
  type(ed2, dec, "\x0b"); // Ctrl-K kills "abcdef"
  EXPECT_EQ(s.view(), "\nxy\n123456");
  type(ed2, dec, "\x0b"); // on an empty line it joins the next one
  EXPECT_EQ(s.view(), "xy\n123456");
}

TEST(TextEditor, TabInsertsSpacesToTabStop) {
  reloco::string s;
  text_editor ed(s, 4);
  key_decoder dec;
  type(ed, dec, "a\t");
  EXPECT_EQ(s.view(), "a   ");
  type(ed, dec, "\t");
  EXPECT_EQ(s.view(), "a       ");
}

TEST(TextEditor, SaveAndCancelActions) {
  reloco::string s;
  text_editor ed(s);
  auto a = ed.handle(key_event{key_code::save, 0});
  auto b = ed.handle(key_event{key_code::cancel, 0});
  auto c = ed.handle(key_event{key_code::character, 'x'});
  ASSERT_TRUE(a.has_value() && b.has_value() && c.has_value());
  EXPECT_EQ(*a, edit_action::save);
  EXPECT_EQ(*b, edit_action::cancel);
  EXPECT_EQ(*c, edit_action::none);
}

TEST(TextEditor, RendersVisibleWindowAndStatus) {
  reloco::string s;
  ASSERT_TRUE(s.try_assign("line one\nline two\nline three\nline four\n").has_value());
  text_editor ed(s);
  ed.set_screen(4, 20); // 3 text rows + status bar
  fake_uart dev;
  ASSERT_TRUE(ed.render(hw::uart_ref(dev)).has_value());
  EXPECT_TRUE(dev.tx.contains("line one"));
  EXPECT_TRUE(dev.tx.contains("line three"));
  EXPECT_FALSE(dev.tx.contains("line four"));
  EXPECT_TRUE(dev.tx.contains("Ln 1, Col 1"));

  // Moving below the window scrolls it.
  key_decoder dec;
  type(ed, dec, "\x1b[B\x1b[B\x1b[B");
  dev.tx.clear();
  ASSERT_TRUE(ed.render(hw::uart_ref(dev)).has_value());
  EXPECT_FALSE(dev.tx.contains("line one"));
  EXPECT_TRUE(dev.tx.contains("line four"));
  EXPECT_TRUE(dev.tx.contains("Ln 4, Col 1"));
}
