// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// A graphical terminal emulator: spawns the user's $SHELL on a pty and shows it in an SDL3 window, built only
// from the hardware abstractions:
//
//   SDL keyboard --> sdl_keyboard (input_traits) --> input_device_ref --+
//                                                                        |--> console_uart --> pty master --> $SHELL
//   SDL window <-- framebuffer_console (console_traits) <-- console_ref -+     (vt100 interpreter inside)
//
// `console_uart` is the bridge exposed as a `uart_ref`: bytes written to it are interpreted as VT100 onto the
// framebuffer console, bytes read from it are the translated keystrokes (Ctrl+letters, arrows, F-keys as ANSI
// sequences). The main loop only shuttles bytes between that uart_ref and the pty master.
//
// SDL scancodes of the keyboard block are USB HID usages, so they pass straight through as HID key events.
// Layout is US. Window size is fixed; the pty is told its size (columns x rows) via TIOCSWINSZ.
//
// The terminal speaks an xterm-style VT100/ANSI subset (see docs/vt100.md); TERM defaults to "vt100" so
// programs stay within it, override with STRUCTO_TERM (e.g. STRUCTO_TERM=xterm for colors from `ls`).
// Window title (OSC), bell and terminal queries (cursor position, device attributes) are wired through
// `vt100_callbacks`.

#include <structo/hw/console_uart.hpp>
#include <structo/hw/framebuffer_console.hpp>
#include <structo/hw/input_device_ref.hpp>
#include <structo/hypervisor/mmio_framebuffer_device.hpp>

#include "fonts/dejavu_sans_mono_8x16_font.hpp"

#include <SDL3/SDL.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <reloco/lifetime.hpp>

// Example code indexes raw buffers freely; bounds are checked by the surrounding logic.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo;
using structo::examples::fonts::dejavu_sans_mono_8x16;
using structo::hw::console_cell;
using structo::hw::rgba8888;

namespace {

constexpr std::size_t window_width = 1024;
constexpr std::size_t window_height = 768;

// The input backend: SDL pumps events into this queue; the input_device_ref side polls it.
struct sdl_keyboard {
  std::deque<hw::input_event> queue;
  bool quit = false;

  // Translates one SDL event; everything but keyboard and window-close is ignored.
  void feed(const SDL_Event &ev) {
    if (ev.type == SDL_EVENT_QUIT) {
      quit = true;
    } else if (ev.type == SDL_EVENT_KEY_DOWN || ev.type == SDL_EVENT_KEY_UP) {
      const auto sc = static_cast<std::uint16_t>(ev.key.scancode);
      if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_RGUI) { // the HID keyboard usage block (0x04..0xE7)
        const auto state = ev.type == SDL_EVENT_KEY_UP ? hw::input_key_state::released
                           : ev.key.repeat             ? hw::input_key_state::repeat
                                                       : hw::input_key_state::pressed;
        queue.push_back(hw::make_key_event(sc, state));
        queue.push_back({hw::input_event_type::sync, 0, 0, 0});
      }
    }
  }
};

} // namespace

template <> struct structo::hw::input_traits<sdl_keyboard> {
  static input_capabilities capabilities(const sdl_keyboard &) noexcept {
    return {static_cast<std::uint32_t>(input_class::keyboard)};
  }
  static reloco::result<bool> event_ready(sdl_keyboard &b) noexcept { return !b.queue.empty(); }
  static reloco::result<input_event> try_read_event(sdl_keyboard &b) noexcept {
    if (b.queue.empty())
      return reloco::unexpected(reloco::error::try_again);
    auto e = b.queue.front();
    b.queue.pop_front();
    return e;
  }
};

int main() {
  const std::size_t cols = window_width / dejavu_sans_mono_8x16::glyph_width;
  const std::size_t rows = window_height / dejavu_sans_mono_8x16::glyph_height;

  // Spawn $SHELL on a pty sized to the text grid.
  winsize ws{};
  ws.ws_col = static_cast<unsigned short>(cols);
  ws.ws_row = static_cast<unsigned short>(rows);
  int master = -1;
  const pid_t child = ::forkpty(&master, nullptr, nullptr, &ws);
  if (child < 0) {
    std::perror("forkpty");
    return 1;
  }
  if (child == 0) {
    const char *shell = std::getenv("SHELL");
    if (shell == nullptr || *shell == '\0')
      shell = "/bin/sh";
    const char *term = std::getenv("STRUCTO_TERM");
    ::setenv("TERM", term != nullptr ? term : "vt100", 1);
    ::setenv("COLUMNS", std::to_string(cols).c_str(), 1);
    ::setenv("LINES", std::to_string(rows).c_str(), 1);
    ::execlp(shell, shell, static_cast<char *>(nullptr));
    std::perror("exec shell");
    ::_exit(127);
  }
  (void)::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("structo terminal", static_cast<int>(window_width),
                                        static_cast<int>(window_height), 0);
  SDL_Renderer *renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
  SDL_Texture *texture = renderer != nullptr
                             ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                                 static_cast<int>(window_width), static_cast<int>(window_height))
                             : nullptr;
  if (texture == nullptr) {
    std::fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  // Pixel buffer the text console rasterizes onto, plus its shadow cell grid.
  auto fb_maker = hypervisor::mmio_framebuffer_device<rgba8888>::try_create(window_width, window_height);
  if (!fb_maker.has_value()) {
    std::fprintf(stderr, "framebuffer creation failed\n");
    return 1;
  }
  auto fb = std::move(fb_maker.value());
  std::vector<console_cell> cells(cols * rows);
  auto console_maker = hw::framebuffer_console<rgba8888, dejavu_sans_mono_8x16>::try_create(
      fb.pixels(), reloco::span<console_cell>(cells.data(), cells.size()));
  if (!console_maker.has_value()) {
    std::fprintf(stderr, "framebuffer_console creation failed\n");
    return 1;
  }
  auto console = std::move(console_maker.value());

  // The adapters: keyboard -> input_device_ref, screen -> console_ref, both bridged into one uart_ref.
  sdl_keyboard keyboard;
  hw::input_device_ref input(keyboard);
  hw::console_ref screen(console);
  hw::console_uart bridge(input, screen);
  hw::uart_ref uart(bridge);

  // Terminal events detected in the shell's output: title, bell and the answers to queries
  // (cursor position, device attributes) that programs expect to read back from the terminal.
  struct host {
    SDL_Window *window;
    int pty;
  } hst{window, master};
  hw::vt100_callbacks events;
  events.ctx = &hst;
  events.title = [](void *c, reloco::string_view text) noexcept {
    const std::string t(text.data(), text.size()); // SDL wants a NUL-terminated string
    SDL_SetWindowTitle(static_cast<host *>(c)->window, t.c_str());
  };
  events.bell = [](void *c) noexcept { (void)SDL_FlashWindow(static_cast<host *>(c)->window, SDL_FLASH_BRIEFLY); };
  events.reply = [](void *c, reloco::string_view bytes) noexcept {
    (void)!::write(static_cast<host *>(c)->pty, bytes.data(), bytes.size());
  };
  bridge.terminal().set_callbacks(events);

  bool child_alive = true;
  while (!keyboard.quit && child_alive) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
      keyboard.feed(ev);

    // Keystrokes (translated by the bridge) -> shell.
    while (true) {
      auto b = uart.try_get_byte();
      if (!b)
        break;
      const std::uint8_t byte = b.value();
      (void)!::write(master, &byte, 1);
    }

    // Shell output -> VT100 -> framebuffer. Bounded per frame so a flood cannot starve rendering.
    std::uint8_t buf[4096];
    for (int i = 0; i < 16; ++i) {
      const ssize_t n = ::read(master, buf, sizeof buf);
      if (n > 0) {
        for (ssize_t k = 0; k < n; ++k)
          (void)uart.put_byte(buf[k]);
      } else {
        // 0 or EIO means the shell side closed; EAGAIN just means "nothing right now".
        if (n == 0 || (errno != EAGAIN && errno != EINTR))
          child_alive = false;
        break;
      }
    }

    auto pixels = fb.pixels().raw();
    (void)SDL_UpdateTexture(texture, nullptr, pixels.data(), static_cast<int>(fb.pixels().stride_bytes()));
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    SDL_Delay(8);
  }

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  ::close(master); // the shell gets SIGHUP
  int status = 0;
  (void)::waitpid(child, &status, 0);
  return 0;
}

RELOCO_END_UNSAFE_BUFFER_USAGE
