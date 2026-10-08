// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Linux demo of bootldr::edit_text: the terminal you run it in becomes the "UART" and the editor
// runs as a scheduler task. Ctrl-S saves, Ctrl-X cancels; the resulting text is printed on exit.
//
//   text_editor_linux_demo [FILE]     (FILE is only read, never written)

#include <structo/bootldr/text_editor.hpp>

#include <reloco/string.hpp>

#if RELOCO_HAS_COROUTINES

#include <cstdio>
#include <ctime>

#include <poll.h>
#include <termios.h>
#include <unistd.h>

using namespace structo;

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

struct outcome {
  bool done = false;
  bool saved = false;
};

// Wraps edit_text so main can see when the editor has finished.
reloco::task<void> editor_task(reloco::allocator_arg_t, reloco::allocator_ref alloc, bootldr::scheduler &sched,
                               tty_uart &uart, reloco::string &text, outcome &out) noexcept {
  auto r = co_await bootldr::edit_text(reloco::allocator_arg, alloc, sched, hw::uart_ref(uart), text);
  out.saved = r && *r;
  out.done = true;
}

} // namespace

int main(int argc, char **argv) {
  bootldr::scheduler sched;
  sched.set_clock(now_ms, nullptr);
  sched.set_idle(idle, nullptr);

  reloco::string text(sched.allocator());
  if (argc > 1) {
    if (FILE *f = std::fopen(argv[1], "rb")) {
      char chunk[256];
      std::size_t n;
      while ((n = std::fread(chunk, 1, sizeof chunk, f)) > 0)
        if (!text.try_append(reloco::string_view(chunk, n))) {
          std::fprintf(stderr, "out of memory\n");
          std::fclose(f);
          return 1;
        }
      std::fclose(f);
    } else {
      std::perror(argv[1]);
      return 1;
    }
  } else {
    (void)text.try_assign("Welcome to the structo text editor demo.\nCtrl-S saves, Ctrl-X cancels.\n");
  }

  termios saved{};
  if (::tcgetattr(STDIN_FILENO, &saved) != 0) {
    std::fprintf(stderr, "stdin must be a terminal\n");
    return 1;
  }
  termios raw = saved;
  ::cfmakeraw(&raw);
  (void)::tcsetattr(STDIN_FILENO, TCSANOW, &raw);

  tty_uart uart;
  outcome out;
  if (!sched.spawn(editor_task(reloco::allocator_arg, sched.allocator(), sched, uart, text, out))) {
    (void)::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return 1;
  }
  while (!out.done) {
    sched.run_once();
    idle(nullptr);
  }

  (void)::tcsetattr(STDIN_FILENO, TCSANOW, &saved);
  std::printf("\x1b[2J\x1b[H%s; %zu bytes:\n%.*s", out.saved ? "saved" : "cancelled", text.size(),
              static_cast<int>(text.size()), text.view().data());
  return 0;
}

#else

int main() { return 0; }

#endif
