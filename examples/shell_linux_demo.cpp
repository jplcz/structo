// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Linux demo of the bootloader shell: the terminal you run it in becomes the "UART". It registers the generic
// command set (help, set, function, md, mw, source, ...) plus `exit`, and exposes a 256-byte scratch buffer
// whose address is in `$buf`, so memory commands are safe to try:
//
//   shell_linux_demo
//   > mw $buf 0x11223344
//   > md $buf 16
//   > set n 5
//   > echo $((n * 4)) $((align_up(n, 8)))
//   > function twice 'echo $1; echo $1'
//   > twice hi
//   > exit

#include <structo/bootldr/shell.hpp>
#include <structo/bootldr/shell_commands.hpp>

#if RELOCO_HAS_COROUTINES

#include <cstdio>
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

bool g_quit = false;

// `exit`: leaves the main loop (which restores the terminal).
reloco::task<void> exit_cmd(command_call &) noexcept {
  g_quit = true;
  co_return;
}

alignas(8) std::uint8_t g_scratch[256];

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
  shell sh{sched, hw::uart_ref(uart)};

  generic_commands_hooks hooks;
  hooks.reset = [](void *) noexcept { g_quit = true; }; // "reset" just quits the demo
  generic_commands cmds{sh, hooks};
  (void)cmds.add_all();

  shell_command exit_command{"exit", "exit: leave the demo", exit_cmd};
  (void)sh.add(exit_command);

  char addr[24];
  const auto n = std::snprintf(addr, sizeof addr, "%#lx", reinterpret_cast<unsigned long>(g_scratch));
  (void)sh.context().set("buf", reloco::string_view(addr, static_cast<std::size_t>(n)));

  (void)hw::uart_ref(uart).write_string("structo shell demo; 'help' lists commands, $buf is a scratch buffer\n");
  if (sh.start()) {
    while (!g_quit) {
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
