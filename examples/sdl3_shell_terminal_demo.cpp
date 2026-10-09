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
// sequences). The UI loop only shuttles bytes between that uart_ref and the pty master; shell output is read
// by a reloco::thread blocked in poll(2) and passed over a reloco SPSC ring buffer.
//
// SDL scancodes of the keyboard block are USB HID usages, so they pass straight through as HID key events.
// Font: `--font NAME` or STRUCTO_FONT (terminus, terminus-bold, terminus-14, spleen, dejavu); `--list-fonts`.
// Layout is US. Window size is fixed; the pty is told its size (columns x rows) via TIOCSWINSZ.
//
// The terminal speaks an xterm-style VT100/ANSI subset (see docs/vt100.md); TERM defaults to "xterm" (so
// cursor/function keys, colors and the alternate screen work in mc, vim, htop...); override with STRUCTO_TERM.
// Window title (OSC), bell and terminal queries (cursor position, device attributes) are wired through
// `vt100_callbacks`.

#include <structo/hw/console_uart.hpp>
#include <structo/hw/framebuffer_console.hpp>
#include <structo/hw/input_device_ref.hpp>
#include <structo/hypervisor/mmio_framebuffer_device.hpp>

#include "fonts/all_fonts.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <reloco/atomic_ring_buffer.hpp>
#include <reloco/function.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <reloco/thread.hpp>

// Example code indexes raw buffers freely; bounds are checked by the surrounding logic.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo;
using structo::hw::console_cell;
using structo::hw::rgba8888;

namespace {

constexpr std::size_t window_width = 1024;
constexpr std::size_t window_height = 768;

// The input backend: SDL pumps events into this queue; the input_device_ref side polls it.
struct sdl_keyboard {
  std::deque<hw::input_event> queue;
  bool quit = false;
  float cell_width = 8; // glyph size in pixels, set by main() from the selected font
  float cell_height = 16;

  // Translates one SDL event; keyboard, mouse and window-close are handled, everything else is ignored.
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
    } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
      point_at(ev.motion.x, ev.motion.y);
      queue.push_back(hw::make_sync_event());
    } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN || ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
      hw::input_button button;
      switch (ev.button.button) {
      case SDL_BUTTON_LEFT:
        button = hw::input_button::left;
        break;
      case SDL_BUTTON_MIDDLE:
        button = hw::input_button::middle;
        break;
      case SDL_BUTTON_RIGHT:
        button = hw::input_button::right;
        break;
      default:
        return;
      }
      point_at(ev.button.x, ev.button.y);
      queue.push_back(hw::make_button_event(button, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN));
      queue.push_back(hw::make_sync_event());
    } else if (ev.type == SDL_EVENT_MOUSE_WHEEL && ev.wheel.y != 0.0f) {
      point_at(ev.wheel.mouse_x, ev.wheel.mouse_y);
      // Positive = scrolled up/away from the user, one notch per event whatever its size.
      queue.push_back(hw::make_rel_event(hw::input_axis::wheel, ev.wheel.y > 0 ? 1 : -1));
      queue.push_back(hw::make_sync_event());
    }
  }

  // Pointer positions go to the input framework in character cells (what `console_uart` reports to programs).
  void point_at(float px, float py) {
    const auto cx = static_cast<std::int32_t>(px / cell_width);
    const auto cy = static_cast<std::int32_t>(py / cell_height);
    queue.push_back(hw::make_abs_event(hw::input_axis::x, cx));
    queue.push_back(hw::make_abs_event(hw::input_axis::y, cy));
  }
};

} // namespace

template <> struct structo::hw::input_traits<sdl_keyboard> {
  static input_capabilities capabilities(const sdl_keyboard &) noexcept {
    return {input_class::keyboard | input_class::tablet};
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

template <typename Font> int run() {
  const std::size_t cols = window_width / Font::glyph_width;
  const std::size_t rows = window_height / Font::glyph_height;

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
    ::setenv("TERM", term != nullptr ? term : "xterm", 1);
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
  SDL_Window *window =
      SDL_CreateWindow("structo terminal", static_cast<int>(window_width), static_cast<int>(window_height), 0);
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
  auto console_maker = hw::framebuffer_console<rgba8888, Font>::try_create(
      fb.pixels(), reloco::span<console_cell>(cells.data(), cells.size()));
  if (!console_maker.has_value()) {
    std::fprintf(stderr, "framebuffer_console creation failed\n");
    return 1;
  }
  auto console = std::move(console_maker.value());

  // The adapters: keyboard -> input_device_ref, screen -> console_ref, both bridged into one uart_ref.
  sdl_keyboard keyboard;
  keyboard.cell_width = static_cast<float>(Font::glyph_width);
  keyboard.cell_height = static_cast<float>(Font::glyph_height);
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

  // Storage that lets full-screen programs use the alternate screen (CSI ? 1049 h/l).
  std::vector<hw::console_cell> alt_screen(cols * rows);
  bridge.terminal().set_alternate_screen_storage(reloco::span<hw::console_cell>(alt_screen.data(), alt_screen.size()));

  // Shell output pump: a reloco::thread blocks in poll(2) on the pty master (reloco has no poll wrapper, so
  // this is plain libc) and hands bytes to the UI thread through a lock-free SPSC ring. It posts an SDL user
  // event after each batch so the UI loop wakes immediately instead of sleeping a fixed interval.
  reloco::heap_spsc_ring_buffer<std::uint8_t> pty_ring;
  if (!pty_ring.try_initialize(64 * 1024)) {
    std::fprintf(stderr, "ring allocation failed\n");
    return 1;
  }
  std::atomic<bool> stop_reader{false};
  std::atomic<bool> pty_eof{false};
  std::atomic<bool> wake_pending{false};
  const Uint32 wake_event = SDL_RegisterEvents(1);

  auto reader_fn = reloco::function<void()>::try_create([&] {
    while (!stop_reader.load(std::memory_order_relaxed)) {
      auto [chunk, chunk2] = pty_ring.write_slices(1);
      (void)chunk2;
      if (chunk.empty()) { // ring full: let the UI drain it (backpressure to the shell)
        ::poll(nullptr, 0, 2);
        continue;
      }
      pollfd pfd{master, POLLIN, 0};
      const int pr = ::poll(&pfd, 1, 50); // timeout only so we notice stop_reader
      if (pr < 0 && errno != EINTR)
        break;
      if (pr <= 0)
        continue;
      const ssize_t n = ::read(master, chunk.data(), chunk.size());
      if (n > 0) {
        pty_ring.commit(static_cast<std::size_t>(n));
        if (!wake_pending.exchange(true)) {
          SDL_Event wake{};
          wake.type = wake_event;
          SDL_PushEvent(&wake);
        }
      } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
        break; // 0 or EIO: the shell side closed
      }
    }
    pty_eof.store(true);
    SDL_Event wake{};
    wake.type = wake_event;
    SDL_PushEvent(&wake);
  });
  if (!reader_fn) {
    std::fprintf(stderr, "failed to allocate the pty reader\n");
    return 1;
  }
  auto reader = reloco::thread::try_spawn(std::move(reader_fn.value()));
  if (!reader) {
    std::fprintf(stderr, "failed to start the pty reader thread\n");
    return 1;
  }

  while (!keyboard.quit) {
    // Sleep until input, shell output (wake event) or ~8 ms; then handle everything pending.
    SDL_Event ev;
    if (SDL_WaitEventTimeout(&ev, 8)) {
      do {
        keyboard.feed(ev);
      } while (SDL_PollEvent(&ev));
    }
    wake_pending.store(false);

    // Keystrokes (translated by the bridge) -> shell.
    while (true) {
      auto b = uart.try_get_byte();
      if (!b)
        break;
      const std::uint8_t byte = b.value();
      (void)!::write(master, &byte, 1);
    }

    // Shell output (from the reader thread's ring) -> VT100 -> framebuffer. Bounded by a time budget per
    // frame so a flood cannot starve input and rendering.
    const std::uint64_t deadline = SDL_GetTicksNS() + 10'000'000;
    bool drained = false;
    while (SDL_GetTicksNS() < deadline) {
      auto [s1, s2] = pty_ring.read_slices(1);
      (void)s2;
      if (s1.empty()) {
        drained = true;
        break;
      }
      for (const std::uint8_t byte : s1)
        (void)uart.put_byte(byte);
      pty_ring.consume(s1.size());
    }
    if (drained && pty_eof.load())
      break; // shell exited and everything it printed has been shown

    auto pixels = fb.pixels().raw();
    (void)SDL_UpdateTexture(texture, nullptr, pixels.data(), static_cast<int>(fb.pixels().stride_bytes()));
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
  }

  stop_reader.store(true);
  reader.value().join();

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  ::close(master); // the shell gets SIGHUP
  int status = 0;
  (void)::waitpid(child, &status, 0);
  return 0;
}

int main(int argc, char **argv) {
  // Fonts are compile-time types, so the demo is instantiated once per font and `--font NAME`/`STRUCTO_FONT`
  // (default: terminus) picks one at start-up; `--list-fonts` shows them.
  return structo::examples::fonts::select_font(argc, argv,
                                               [](auto tag) { return run<typename decltype(tag)::type>(); });
}

RELOCO_END_UNSAFE_BUFFER_USAGE
