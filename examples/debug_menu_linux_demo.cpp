// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Linux demo of bootldr::debug_menu: the terminal you run it in becomes the "UART". Like a bootloader, it
// waits two seconds for the space bar before showing the menu. Up/Down/Enter or the hotkeys select items,
// `q` leaves a menu.
//
//   debug_menu_linux_demo

#include <structo/bootldr/debug_menu.hpp>

#if RELOCO_HAS_COROUTINES

#include <ctime>

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <reloco/lifetime.hpp>

// Example code indexes raw buffers freely; bounds are checked by the surrounding logic.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo;
using namespace structo::bootldr;

namespace {

// stdin/stdout as a polled UART; `rx_ready` has to peek, so one byte is read ahead.
struct tty_uart {
  bool have = false;
  std::uint8_t byte = 0;
};

std::uint64_t now_ms(void *) noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

// Sleeps until a key arrives (or 20 ms passed) instead of spinning.
void idle(void *) noexcept {
  pollfd p{STDIN_FILENO, POLLIN, 0};
  (void)::poll(&p, 1, 20);
}

} // namespace

template <> struct structo::hw::uart_traits<tty_uart> {
  static reloco::result<void> configure(tty_uart &, const uart_config &) noexcept { return {}; }
  static reloco::result<bool> tx_ready(tty_uart &) noexcept { return true; }
  static reloco::result<bool> rx_ready(tty_uart &u) noexcept {
    if (u.have)
      return true;
    pollfd p{STDIN_FILENO, POLLIN, 0};
    if (::poll(&p, 1, 0) > 0 && ::read(STDIN_FILENO, &u.byte, 1) == 1)
      u.have = true;
    return u.have;
  }
  static reloco::result<void> try_put_byte(tty_uart &, std::uint8_t v) noexcept {
    return ::write(STDOUT_FILENO, &v, 1) == 1 ? reloco::result<void>{}
                                              : reloco::result<void>(reloco::unexpected(reloco::error::invalid_state));
  }
  static reloco::result<std::uint8_t> try_get_byte(tty_uart &u) noexcept {
    if (!u.have)
      return reloco::unexpected(reloco::error::try_again);
    u.have = false;
    return u.byte;
  }
};

namespace {

// Shows the scheduler clock; `ctx` is unused here.
reloco::task<void> item_uptime(menu_context &c) noexcept {
  (void)c.print("uptime: {} ms\n", c.sched().now_ms());
  co_return;
}

// A slow item: counts down on the scheduler clock; other tasks would keep running meanwhile.
reloco::task<void> item_countdown(menu_context &c) noexcept {
  for (int i = 3; i > 0; --i) {
    (void)c.print("{}...\n", i);
    co_await co_await c.sched().sleep_for(500);
  }
  (void)c.write("liftoff\n");
}

// An interactive item: reads keys itself until Enter.
reloco::task<void> item_keys(menu_context &c) noexcept {
  (void)c.write("press keys, Enter to finish\n");
  for (;;) {
    auto k = co_await c.read_key();
    if (!k)
      co_await reloco::unexpected(k.error());
    if (k->code == key_code::enter)
      co_return;
    if (k->code == key_code::character)
      (void)c.print("char '{}'\n", k->ch);
    else
      (void)c.print("key code {}\n", static_cast<int>(k->code));
  }
}

// Failing item: the menu prints the error and waits for a key.
reloco::task<void> item_fail(menu_context &) noexcept {
  co_await reloco::unexpected(reloco::error::unsupported_operation);
}

// Submenu item: `ctx` is the nested debug_menu, which runs until the user presses `q` in it.
reloco::task<void> item_submenu(menu_context &c) noexcept {
  (void)co_await debug_menu::run(*static_cast<debug_menu *>(c.ctx()));
}

struct outcome {
  bool done = false;
};

// Boot flow: enter the menu only if the space bar is pressed within two seconds.
reloco::task<void> boot_task(scheduler &sched, tty_uart &uart, debug_menu &menu, outcome &out) noexcept {
  (void)hw::uart_ref(uart).write_string("Press SPACE within 2 s for the debug menu...\n");
  auto pressed = co_await wait_for_key(sched, hw::uart_ref(uart), 2000, ' ');
  if (pressed && *pressed)
    (void)co_await debug_menu::run(menu);
  else
    (void)hw::uart_ref(uart).write_string("booting...\n");
  out.done = true;
}

} // namespace

int main() {
  scheduler sched;
  sched.set_clock(now_ms, nullptr);

  termios saved{};
  if (::tcgetattr(STDIN_FILENO, &saved) != 0) {
    (void)!::write(STDERR_FILENO, "stdin must be a terminal\n", 25);
    return 1;
  }
  termios raw = saved;
  ::cfmakeraw(&raw);
  (void)::tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  tty_uart uart;
  debug_menu menu{sched, hw::uart_ref(uart), "structo debug menu"};
  debug_menu tools{sched, hw::uart_ref(uart), "Tools"};
  menu_item uptime{"Uptime", "show the scheduler clock", item_uptime};
  menu_item countdown{"Countdown", "slow item using sleep_for", item_countdown};
  menu_item keys{"Keys", "interactive key reader", item_keys};
  menu_item fail{"Failing item", "returns an error", item_fail};
  menu_item sub{"Tools...", "open a submenu", item_submenu, &tools};
  menu_item t1{"Uptime again", "same handler in the submenu", item_uptime};
  (void)menu.add(uptime);
  (void)menu.add(countdown);
  (void)menu.add(keys);
  (void)menu.add(fail);
  (void)menu.add(sub);
  (void)tools.add(t1);

  outcome out;
  if (sched.spawn(boot_task(sched, uart, menu, out))) {
    while (!out.done) {
      sched.run_once();
      idle(nullptr);
    }
  }

  (void)::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  return 0;
}

RELOCO_END_UNSAFE_BUFFER_USAGE

#else

int main() { return 0; }

#endif
