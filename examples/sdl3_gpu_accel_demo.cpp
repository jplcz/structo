// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// A windowed demo wiring `structo`'s hypervisor-side graphics emulation
// straight to a real SDL3 window: `mmio_framebuffer_device` owns the
// pixel buffer a "guest" renders into, `mmio_gpu_command_buffer_device`
// decodes and dispatches a batch of drawing primitives into it (through
// `hw::gpu_accel_ref`, bound here to the framebuffer's own software
// backend), and every frame this host simply copies the resulting pixels
// into an SDL texture and presents it -- standing in for whatever would
// otherwise map the framebuffer device's `command_buffer()` into real
// guest physical memory and trap the `execute` doorbell on a real VM
// exit.
//
// Exactly like `callout_scheduler_demo.cpp`'s "toy_callout_scheduler" is
// not part of the public `structo` API, the per-frame "fill in some
// gpu_command structs" logic below is just a stand-in for a real guest
// GPU driver -- the point of this demo is exercising
// `mmio_framebuffer_device`/`mmio_gpu_command_buffer_device`/
// `gpu_accel_ref` end-to-end against a real presentation surface, not
// modeling an actual guest.
//
// Only built when SDL3 development files are found (see
// `examples/CMakeLists.txt`); skipped entirely otherwise.

#include <structo/hypervisor/mmio_framebuffer_device.hpp>
#include <structo/hypervisor/mmio_gpu_command_buffer_device.hpp>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>

#include <reloco/lifetime.hpp>

// Example code indexes raw buffers freely; bounds are checked by the surrounding logic.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using structo::hw::gpu_accel_ref;
using structo::hw::rgba8888;
using structo::hw::rgb_color;
using structo::hypervisor::encode_gpu_command;
using structo::hypervisor::gpu_command;
using structo::hypervisor::gpu_command_opcode;
using structo::hypervisor::mmio_device_ref;
using structo::hypervisor::mmio_framebuffer_device;
using structo::hypervisor::mmio_gpu_command_buffer_device;
using structo::hypervisor::pack_gpu_color;

constexpr std::size_t window_width = 640;
constexpr std::size_t window_height = 480;
constexpr std::size_t max_commands_per_frame = 64;

// Stands in for a guest GPU driver filling the command buffer with this
// frame's drawing primitives: a moving ball bouncing inside a border,
// redrawn from scratch every frame (clear + border + ball).
constexpr std::int32_t ball_radius = 24;

gpu_command make_command(gpu_command_opcode op, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1,
                         rgb_color color) noexcept {
  return {op, x0, y0, x1, y1, pack_gpu_color(color)};
}

// Encodes @p cmd directly into command slot @p index of @p accel's mapped command buffer -- exactly the
// ordinary store a real guest driver would perform, with no intermediate container/allocation involved.
void stage_command(mmio_gpu_command_buffer_device &accel, std::size_t index, const gpu_command &cmd) noexcept {
  auto buffer = accel.command_buffer();
  encode_gpu_command(cmd, buffer.subspan(index * mmio_gpu_command_buffer_device::command_slot_size,
                                        mmio_gpu_command_buffer_device::command_slot_size));
}

void fill_frame_commands(mmio_gpu_command_buffer_device &accel, double t) noexcept {
  const auto margin = ball_radius + 8;
  const auto cx = static_cast<std::int32_t>(window_width / 2)
      + static_cast<std::int32_t>(std::lround((static_cast<double>(window_width) / 2 - margin) * std::sin(t)));
  const auto cy = static_cast<std::int32_t>(window_height / 2)
      + static_cast<std::int32_t>(std::lround((static_cast<double>(window_height) / 2 - margin) * std::cos(t * 1.3)));

  std::size_t n = 0;
  const std::size_t capacity = accel.capacity();

  if (n < capacity) {
    stage_command(accel, n++, make_command(gpu_command_opcode::clear, 0, 0, 0, 0, {20, 20, 30}));
  }
  if (n < capacity) {
    stage_command(accel, n++,
                  make_command(gpu_command_opcode::draw_rect, 4, 4, static_cast<std::int32_t>(window_width) - 8,
                              static_cast<std::int32_t>(window_height) - 8, {80, 80, 100}));
  }

  // A small filled "ball" drawn as a stack of horizontal fill_rects approximating a disc -- the command
  // buffer only exposes rectangles/lines/pixels, no native circle primitive, same constraint a guest GPU
  // driver would face.
  for (std::int32_t dy = -ball_radius; dy <= ball_radius && n < capacity; ++dy) {
    const auto dx = static_cast<std::int32_t>(
        std::lround(std::sqrt(static_cast<double>(ball_radius * ball_radius - dy * dy))));
    stage_command(accel, n++,
                  make_command(gpu_command_opcode::fill_rect, cx - dx, cy + dy, 2 * dx + 1, 1, {255, 200, 60}));
  }

  (void)accel.set_count(n);
}

} // namespace

int main() {
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window *window = SDL_CreateWindow("structo: gpu_accel_ref + mmio_gpu_command_buffer_device",
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

  // `mmio_framebuffer_device<rgba8888>` owns a page-aligned pixel buffer the "guest" renders into -- a real
  // hypervisor would map this straight into guest physical memory; here, this host simply reads it back
  // every frame via `pixels().raw()`. `rgba8888`'s in-memory byte order (R, G, B, A) matches
  // `SDL_PIXELFORMAT_RGBA32` exactly, so no per-pixel conversion is needed before `SDL_UpdateTexture`.
  auto fb_maker = mmio_framebuffer_device<rgba8888>::try_create(window_width, window_height);
  if (!fb_maker.has_value()) {
    std::fprintf(stderr, "mmio_framebuffer_device::try_create failed\n");
    return 1;
  }
  auto fb = std::move(fb_maker.value());

  // `gpu_accel_ref` binds the command buffer device's drawing target to that same framebuffer's software
  // backend (the only backend `structo` itself ships); a real embedding could specialize
  // `hw::gpu_accel_traits` for an `SDL_Renderer`-backed type instead and plug it in here unchanged.
  gpu_accel_ref gpu(fb.pixels());

  auto accel_maker = mmio_gpu_command_buffer_device::try_create(gpu, max_commands_per_frame);
  if (!accel_maker.has_value()) {
    std::fprintf(stderr, "mmio_gpu_command_buffer_device::try_create failed\n");
    return 1;
  }
  auto accel = std::move(accel_maker.value());
  mmio_device_ref accel_control(accel);

  bool running = true;
  double t = 0.0;
  while (running) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
      if (ev.type == SDL_EVENT_QUIT
          || (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE)) {
        running = false;
      }
    }

    // Stand-in for a guest driver: stage this frame's commands directly into the mapped command buffer
    // with ordinary stores (no VM exit per command) ...
    fill_frame_commands(accel, t);
    // ... then trigger their synchronous execution with a single trapped MMIO write to the `execute`
    // doorbell register, exactly as a real guest driver would.
    std::byte doorbell[4] = {};
    (void)accel_control.try_write(mmio_gpu_command_buffer_device::control_off_execute,
                                  reloco::span<const std::byte>(doorbell, 4));

    auto pixels = fb.pixels().raw();
    (void)SDL_UpdateTexture(texture, nullptr, pixels.data(), static_cast<int>(fb.pixels().stride_bytes()));
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);

    SDL_Delay(16); // ~60 FPS
    t += 0.03;
  }

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}

RELOCO_END_UNSAFE_BUFFER_USAGE
