// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bootldr/boot_prompt.hpp>

#include <reloco/array.hpp>
#include <reloco/inline_string.hpp>

using namespace structo;
using namespace structo::bootldr;

namespace {

struct fake_uart {
  reloco::array<std::uint8_t, 32> rxbuf{};
  std::size_t rx_head = 0, rx_tail = 0;
  reloco::inline_string<256> tx;

  bool rx_empty() const { return rx_head == rx_tail; }
  void push(std::uint8_t b) { rxbuf[rx_tail++] = b; }
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

// Virtual clock: every read advances 10 ms; `key` is typed once time reaches `key_at`.
struct world {
  fake_uart dev;
  std::uint64_t now = 0;
  std::uint64_t key_at = ~std::uint64_t{0};
  reloco::string_view keys;
};

std::uint64_t tick(void *c) noexcept {
  auto *w = static_cast<world *>(c);
  w->now += 10;
  if (w->now >= w->key_at) {
    for (char k : w->keys)
      w->dev.push(static_cast<std::uint8_t>(k));
    w->key_at = ~std::uint64_t{0};
  }
  return w->now;
}

} // namespace

TEST(BootPrompt, KeyWithinTimeoutIsAccepted) {
  world w;
  w.key_at = 1500;
  w.keys = " ";
  auto r = boot_prompt(hw::uart_ref(w.dev), tick, &w);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, prompt_result::key_pressed);
  EXPECT_LT(w.now, 1700u);
  EXPECT_TRUE(w.dev.tx.contains("Press SPACE to boot (3)"));
  EXPECT_TRUE(w.dev.tx.contains("(2)"));
}

TEST(BootPrompt, TimesOutWithoutKey) {
  world w;
  boot_prompt_options o;
  o.timeout_ms = 1000;
  auto r = boot_prompt(hw::uart_ref(w.dev), tick, &w, o);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, prompt_result::timed_out);
  EXPECT_GE(w.now, 1000u);
  EXPECT_LT(w.now, 1100u);
}

TEST(BootPrompt, OtherKeysAreIgnored) {
  world w;
  w.key_at = 100;
  w.keys = "xyz\r";
  boot_prompt_options o;
  o.timeout_ms = 500;
  auto r = boot_prompt(hw::uart_ref(w.dev), tick, &w, o);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, prompt_result::timed_out);
}

TEST(BootPrompt, CaseHandling) {
  {
    world w;
    w.key_at = 100;
    w.keys = "B";
    boot_prompt_options o;
    o.key = 'b';
    auto r = boot_prompt(hw::uart_ref(w.dev), tick, &w, o);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, prompt_result::key_pressed);
  }
  {
    world w;
    w.key_at = 100;
    w.keys = "B";
    boot_prompt_options o;
    o.key = 'b';
    o.ignore_case = false;
    o.timeout_ms = 300;
    auto r = boot_prompt(hw::uart_ref(w.dev), tick, &w, o);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, prompt_result::timed_out);
  }
}

TEST(BootPrompt, StaleInputIsFlushedUnlessDisabled) {
  {
    world w;
    w.dev.push(' '); // typed before the prompt appeared
    boot_prompt_options o;
    o.timeout_ms = 200;
    auto r = boot_prompt(hw::uart_ref(w.dev), tick, &w, o);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, prompt_result::timed_out);
  }
  {
    world w;
    w.dev.push(' ');
    boot_prompt_options o;
    o.flush_input = false;
    auto r = boot_prompt(hw::uart_ref(w.dev), tick, &w, o);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, prompt_result::key_pressed);
  }
}

TEST(BootPrompt, PlainMessageWithoutCountdownAndNullClock) {
  world w;
  boot_prompt_options o;
  o.countdown = false;
  o.timeout_ms = 100;
  o.message = "Go?";
  auto r = boot_prompt(hw::uart_ref(w.dev), tick, &w, o);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(w.dev.tx, "Go?\r\n"); // write_string maps \n to \r\n

  auto bad = boot_prompt(hw::uart_ref(w.dev), nullptr, nullptr, o);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error(), reloco::error::invalid_argument);
}

namespace {

// 32-bit, 1 kHz hardware counter that advances 10 ticks per read; the key is typed once it passes `key_at`.
struct hw_counter {
  fake_uart *dev = nullptr;
  std::uint64_t value = 0;
  std::uint64_t key_at = ~std::uint64_t{0};
};

} // namespace

template <> struct structo::hw::clock_traits<hw_counter> {
  static reloco::result<std::uint64_t> read_counter(hw_counter &c) noexcept {
    c.value += 10;
    if (c.value >= c.key_at) {
      c.dev->push(' ');
      c.key_at = ~std::uint64_t{0};
    }
    return c.value;
  }
  static std::uint64_t frequency_hz(hw_counter &) noexcept { return 1000; }
  static unsigned counter_bits(hw_counter &) noexcept { return 32; }
};

TEST(BootPrompt, TimedByClockReader) {
  fake_uart dev;
  hw_counter c{&dev, 0, 1500};
  hw::clock_reader reader{hw::clock_ref{c}};
  ASSERT_TRUE(reader.reset().has_value());
  auto r = boot_prompt(hw::uart_ref(dev), reader);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, prompt_result::key_pressed);
}

TEST(BootPrompt, TimesOutWithAtomicClockReader) {
  fake_uart dev;
  hw_counter c{&dev};
  hw::atomic_clock_reader reader{hw::clock_ref{c}};
  ASSERT_TRUE(reader.reset().has_value());
  boot_prompt_options o;
  o.timeout_ms = 500;
  auto r = boot_prompt(hw::uart_ref(dev), reader, o);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, prompt_result::timed_out);
}
