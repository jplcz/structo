// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// A windowed demo driving a VT100/ANSI terminal straight to a real SDL3
// window: `hw::vt100_terminal` interprets a fed byte stream (escape
// sequences included) through `hw::console_ref`, bound here to
// `hw::framebuffer_console<PixelFormat, Font>` rasterizing glyphs onto
// an `hypervisor::mmio_framebuffer_device`-owned pixel buffer, presented
// every frame exactly like `sdl3_gpu_accel_demo.cpp` does -- the only
// difference is *what* draws into the framebuffer (a text console
// instead of a gpu command buffer).
//
// Uses real bitmap fonts from `examples/fonts/` (Terminus by default; pick another with
// `--font NAME`, `STRUCTO_FONT=NAME` or list them with `--list-fonts`; licenses are in the
// generated headers and `examples/fonts/LICENSE.*`/`copyright`) instead of
// `framebuffer_console.hpp`'s own `block_font_8x8` solid-block stand-in, so the rendered
// text is actually legible.
//
// Only built when SDL3 development files are found (see
// `examples/CMakeLists.txt`); skipped entirely otherwise.

#include <structo/hw/framebuffer_console.hpp>
#include <structo/hw/vt100.hpp>
#include <structo/hypervisor/mmio_framebuffer_device.hpp>

#include "fonts/all_fonts.hpp"

#include <microfmt/microfmt.hpp>

#include <reloco/vector.hpp>

#include <SDL3/SDL.h>

#include <cstdio>

#include <reloco/lifetime.hpp>

// Example code indexes raw buffers freely; bounds are checked by the surrounding logic.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using structo::hw::console_cell;
using structo::hw::console_ref;
using structo::hw::framebuffer_console;
using structo::hw::rgba8888;
using structo::hw::vt100_terminal;
using structo::hypervisor::mmio_framebuffer_device;

constexpr std::size_t window_width = 1024;
constexpr std::size_t window_height = 768;

template <typename Font> int run() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow("structo: vt100_terminal + framebuffer_console",
                                        static_cast<int>(window_width), static_cast<int>(window_height), 0);
  SDL_Renderer *renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
  if (window == nullptr || renderer == nullptr) {
    std::fprintf(stderr, "SDL window/renderer creation failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                           static_cast<int>(window_width), static_cast<int>(window_height));
  if (texture == nullptr) {
    std::fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  // `mmio_framebuffer_device<rgba8888>` owns the page-aligned pixel buffer `framebuffer_console` rasterizes
  // onto -- a real hypervisor would map this straight into guest physical memory; here, this host simply
  // reads it back every frame via `pixels().raw()`, same as `sdl3_gpu_accel_demo.cpp`.
  auto fb_maker = mmio_framebuffer_device<rgba8888>::try_create(window_width, window_height);
  if (!fb_maker.has_value()) {
    std::fprintf(stderr, "mmio_framebuffer_device::try_create failed\n");
    return 1;
  }
  auto fb = std::move(fb_maker.value());

  // `framebuffer_console` needs a caller-owned `columns() * rows()` shadow cell grid (pixels are
  // write-only, see that header's own docs); sized only once `columns()`/`rows()` -- a function of the
  // framebuffer's pixel geometry divided by the font's glyph size -- are known, so this goes through
  // `reloco::vector<console_cell>::try_create()` + `try_resize()` rather than a compile-time-sized array.
  const std::size_t cols = window_width / Font::glyph_width;
  const std::size_t rows = window_height / Font::glyph_height;
  auto cells_maker = reloco::vector<console_cell>::try_create(cols * rows);
  if (!cells_maker.has_value()) {
    std::fprintf(stderr, "reloco::vector<console_cell>::try_create failed\n");
    return 1;
  }
  auto cells = std::move(cells_maker.value());
  if (auto resized = cells.try_resize(cols * rows); !resized.has_value()) {
    std::fprintf(stderr, "reloco::vector<console_cell>::try_resize failed\n");
    return 1;
  }

  auto console_maker = framebuffer_console<rgba8888, Font>::try_create(
      fb.pixels(), reloco::span<console_cell>(cells.data(), cells.size()));
  if (!console_maker.has_value()) {
    std::fprintf(stderr, "framebuffer_console::try_create failed\n");
    return 1;
  }
  auto console = std::move(console_maker.value());

  console_ref cref(console);
  vt100_terminal term(cref);

  // A static banner exercising cursor positioning, SGR colors, and erase -- fed once, up front.
  (void)microfmt::format_to(term.as_sink(),
                            "\x1b[2J\x1b[1;1H"
                            "\x1b[1;37m structo::hw::vt100_terminal demo \x1b[0m\r\n"
                            "\x1b[32mgreen\x1b[0m \x1b[31mred\x1b[0m \x1b[34mblue\x1b[0m \x1b[33myellow\x1b[0m "
                            "\x1b[36mcyan\x1b[0m \x1b[1;35mbright magenta\x1b[0m\r\n"
                            "{} columns x {} rows, glyph {}x{} (see --list-fonts)\r\n"
                            "\x1b[2m--------------------------------------------------------------\x1b[0m\r\n",
                            cols, rows, Font::glyph_width, Font::glyph_height);

  bool running = true;
  std::uint64_t tick = 0;
  std::uint64_t last_tick_ms = 0;
  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_QUIT || (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)) {
        running = false;
      }
    }

    // Once a second: two fixed-position "live" fields, cursor repositioned via CUP and the remainder of
    // each line erased via `\x1b[K` -- updated strictly in place, with no trailing `\r\n` of their own, so
    // nothing here ever reaches `console_ref::scroll_up`: writing a fresh line at the *last* row (the one
    // scrolling trigger `vt100_terminal`'s strict VT100 `\n` honors) would otherwise scroll every row of
    // the whole screen, banner included, on every single tick -- there is no per-region scroll support to
    // confine it to just a log area.
    const std::uint64_t now_ms = SDL_GetTicks();
    if (now_ms - last_tick_ms >= 1000) {
      last_tick_ms = now_ms;
      (void)microfmt::format_to(term.as_sink(), "\x1b[5;1H\x1b[1;33muptime: {:4}s\x1b[0m\x1b[K", tick);
      (void)microfmt::format_to(term.as_sink(), "\x1b[6;1H\x1b[36mtick #{}\x1b[0m\x1b[K", tick);
      ++tick;
    }

    auto pixels = fb.pixels().raw();
    (void)SDL_UpdateTexture(texture, nullptr, pixels.data(), static_cast<int>(fb.pixels().stride_bytes()));
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);

    SDL_Delay(16); // ~60 FPS
  }

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  return structo::examples::fonts::select_font(
      argc, argv, [](auto tag) { return run<typename decltype(tag)::type>(); });
}

RELOCO_END_UNSAFE_BUFFER_USAGE
