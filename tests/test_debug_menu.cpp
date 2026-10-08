// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/debug_menu.hpp>

#include <reloco/array.hpp>
#include <reloco/inline_string.hpp>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo;
using namespace structo::bootldr;

namespace {

struct fake_uart {
  reloco::array<std::uint8_t, 256> rxbuf{};
  std::size_t rx_head = 0, rx_tail = 0;
  reloco::inline_string<2048> tx;

  bool rx_empty() const { return rx_head == rx_tail; }
  void type(reloco::string_view s) {
    for (std::size_t i = 0; i < s.size(); ++i)
      rxbuf[rx_tail++] = static_cast<std::uint8_t>(s[i]);
  }
};

} // namespace

template <> struct structo::hw::uart_traits<fake_uart> {
  static reloco::result<void> configure(fake_uart &, const hw::uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(fake_uart &) noexcept { return true; }
  static reloco::result<bool> rx_ready(fake_uart &b) noexcept { return !b.rx_empty(); }
  static reloco::result<void> try_put_byte(fake_uart &b, std::uint8_t v) noexcept {
    (void)b.tx.try_push_back(static_cast<char>(v));
    return {};
  }
  static reloco::result<std::uint8_t> try_get_byte(fake_uart &b) noexcept {
    if (b.rx_empty())
      return reloco::unexpected(reloco::error::try_again);
    return b.rxbuf[b.rx_head++];
  }
};

namespace {

reloco::task<void> item_hello(menu_context &c) noexcept {
  ++*static_cast<int *>(c.ctx());
  (void)c.write("hello item\n");
  co_return;
}

reloco::task<void> item_fail(menu_context &) noexcept {
  co_await reloco::unexpected(reloco::error::invalid_argument);
}

class DebugMenu : public ::testing::Test {
protected:
  fake_uart dev;
  scheduler sched;
  int hits = 0;
  debug_menu menu{sched, hw::uart_ref(dev), "Test menu"};
  menu_item hello{"Hello", "say hello", item_hello, &hits};
  menu_item fail{"Fail", "always fails", item_fail};

  void SetUp() override {
    ASSERT_TRUE(menu.add(hello).has_value());
    ASSERT_TRUE(menu.add(fail).has_value());
    ASSERT_TRUE(menu.start().has_value());
  }
  void run(unsigned rounds = 80) {
    for (unsigned i = 0; i < rounds; ++i)
      sched.run_once();
  }
  bool out_has(reloco::string_view s) const { return dev.tx.contains(s); }
};

} // namespace

TEST_F(DebugMenu, DrawsTitleAndItems) {
  run(4);
  EXPECT_TRUE(out_has("Test menu"));
  EXPECT_TRUE(out_has("Hello"));
  EXPECT_TRUE(out_has("say hello"));
  EXPECT_TRUE(out_has("Fail"));
}

TEST_F(DebugMenu, HotkeyRunsItemAndReturns) {
  run(4);
  dev.type("1");
  run();
  EXPECT_EQ(hits, 1);
  EXPECT_TRUE(out_has("hello item"));
  EXPECT_TRUE(out_has("press any key"));
  dev.type("x");
  run();
  EXPECT_TRUE(menu.running());
}

TEST_F(DebugMenu, EnterRunsSelectedItem) {
  run(4);
  dev.type("\x1b[B\r"); // Down, Enter -> second item, which fails
  run();
  EXPECT_TRUE(out_has("error:"));
  EXPECT_EQ(hits, 0);
}

TEST_F(DebugMenu, QuitEndsTask) {
  run(4);
  dev.type("q");
  run();
  EXPECT_FALSE(menu.running());
}

TEST_F(DebugMenu, ItemUnlinksOnDestruction) {
  {
    menu_item tmp{"Tmp", "", item_hello};
    ASSERT_TRUE(menu.add(tmp).has_value());
    EXPECT_EQ(menu.size(), 3u);
    EXPECT_FALSE(menu.add(tmp).has_value());
  }
  EXPECT_EQ(menu.size(), 2u);
}

TEST(DebugMenuWait, KeyArrivesBeforeTimeout) {
  fake_uart dev;
  scheduler sched;
  std::uint64_t now = 0;
  sched.set_clock([](void *p) noexcept { return *static_cast<std::uint64_t *>(p); }, &now);
  bool got = false, done = false;
  auto t = [&]() -> reloco::task<void> {
    auto r = co_await wait_for_key(sched, hw::uart_ref(dev), 1000, ' ');
    got = r && *r;
    done = true;
  };
  ASSERT_TRUE(sched.spawn(t()).has_value());
  dev.type("a ");
  for (int i = 0; i < 20; ++i)
    sched.run_once();
  EXPECT_TRUE(done);
  EXPECT_TRUE(got);
}

RELOCO_END_UNSAFE_BUFFER_USAGE
