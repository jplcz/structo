// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// A windowed demo of the virtio-gpu device (`virtio_gpu_function`) presented in an SDL3 window.
//
// The "guest" below is a tiny stand-in for Linux's virtio_gpu driver. Its RAM is a plain byte array,
// and it talks to the device the way a real driver does: through the virtio-mmio registers and a split
// virtqueue (using the library's own `split_virtq_driver`).
//
//   1. GET_DISPLAY_INFO          learns the scanout size
//   2. RESOURCE_CREATE_2D        a width x height B8G8R8X8 resource
//   3. RESOURCE_ATTACH_BACKING   its pixels live in four scattered "RAM bands" (scatter-gather)
//   4. SET_SCANOUT               shows the resource on scanout 0
//   5. every frame: move a box in the backing, then TRANSFER_TO_HOST_2D + RESOURCE_FLUSH of just the
//      changed rectangle (the first frame transfers and flushes everything)
//
// On the host side the device's `Display` is `framebuffer_accel_display`: flushed pixels land in an
// `mmio_framebuffer_device` surface (fills go through a `hw::gpu_accel_ref`), and this program copies
// that surface into an SDL texture each frame.
//
// Usage: sdl3_virtio_gpu_demo [--frames N]   (exit after N frames; handy with SDL_VIDEODRIVER=dummy)
//
// Only built when SDL3 development files are found (see `examples/CMakeLists.txt`).

#include <structo/hypervisor/mmio_framebuffer_device.hpp>
#include <structo/virtio/split_ring.hpp>
#include <structo/virtio/virtio_gpu.hpp>
#include <structo/virtio/virtio_gpu_accel.hpp>
#include <structo/virtio/virtio_mmio.hpp>

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

using namespace structo;
using namespace structo::virtio;
namespace reg = structo::virtio::mmio_reg;

constexpr std::uint32_t width = 640;
constexpr std::uint32_t height = 480;
constexpr std::uint32_t resource_id = 1;

struct guest_space {};
using gaddr = phys_addr<void, guest_space>;
using gmem = direct_virtq_memory<guest_space>;
using driver_t = split_virtq_driver<guest_space, guest_space, gmem>;
using sg_t = sg_entry<guest_space, std::uint64_t>;

// Guest "physical" layout.
constexpr std::uint64_t ram_base = 0x10000;
constexpr std::uint64_t ram_size = 4u << 20;
constexpr std::uint64_t ring_addr = ram_base; // one ring per queue, 0x1000 apart
constexpr std::uint64_t cmd_buf = ram_base + 0x4000; // request buffer
constexpr std::uint64_t resp_buf = ram_base + 0x6000; // response buffer
constexpr std::uint32_t ring_size = 8;

// The resource's pixels are scattered over four bands with gaps between them.
constexpr std::uint32_t bands = 4;
constexpr std::uint64_t band_bytes = std::uint64_t{width} * (height / bands) * 4;
constexpr std::uint64_t band_stride = 0x60000;
constexpr std::uint64_t band_base = ram_base + 0x100000;
static_assert(band_bytes <= band_stride, "bands must not overlap");

void die(const char *what) {
  std::fprintf(stderr, "demo failure: %s\n", what);
  std::exit(1);
}

using display_t = framebuffer_accel_display<hw::rgba8888>;
using gpu_t = virtio_gpu_function<guest_space, display_t>;
using device_t = virtio_mmio_device<guest_space, gmem, gpu_t>;

// Guest RAM; shared by the device (which reads the guest's buffers through `mem`) and the guest driver.
struct guest_ram {
  std::vector<std::byte> bytes{ram_size};
  gmem mem{bytes.data(), bytes.size(), gaddr{ram_base}};
};

class guest {
public:
  guest(device_t &dev, guest_ram &ram) : dev_(dev), ram_(ram) {}

  // Brings the device up the way a driver would: features, one ring per queue, DRIVER_OK.
  void probe() {
    if (read(reg::device_id) != gpu::device_id)
      die("not a virtio-gpu device");
    write(reg::status, 0);
    write(reg::status, reg::status_acknowledge | reg::status_driver);
    write(reg::driver_features_sel, 0);
    write(reg::driver_features, 0);
    write(reg::driver_features_sel, 1);
    write(reg::driver_features, 1u << (feature_version_1 - 32));
    write(reg::status, reg::status_acknowledge | reg::status_driver | reg::status_features_ok);
    if ((read(reg::status) & reg::status_features_ok) == 0)
      die("feature negotiation rejected");
    for (std::uint32_t q = 0; q < 2; ++q) {
      auto layout = try_split_layout(ring_size, false);
      if (!layout)
        die("ring layout");
      auto a = split_ring_addrs<guest_space>::try_from_contiguous(gaddr{ring_addr + 0x1000 * q}, *layout);
      if (!a)
        die("ring addresses");
      write(reg::queue_sel, q);
      write(reg::queue_num, ring_size);
      write(reg::queue_desc_low, static_cast<std::uint32_t>(a->desc.value));
      write(reg::queue_driver_low, static_cast<std::uint32_t>(a->avail.value));
      write(reg::queue_device_low, static_cast<std::uint32_t>(a->used.value));
      write(reg::queue_ready, 1);
      auto d = driver_t::try_create(ram_.mem, *a, ring_size, slots_[q].as_span());
      if (!d)
        die("driver ring");
      drv_[q].emplace(*d);
    }
    write(reg::status, read(reg::status) | reg::status_driver_ok);
  }

  // Sends one control command (a header followed by 32-bit words); returns the response type.
  std::uint32_t command(std::uint32_t type, std::initializer_list<std::uint32_t> words,
                        std::uint32_t resp_len = gpu::header_size) {
    std::uint8_t req[128] = {};
    std::memcpy(req, &type, 4); // little-endian host
    std::size_t len = gpu::header_size;
    for (auto w : words) {
      std::memcpy(req + len, &w, 4);
      len += 4;
    }
    return submit(req, len, resp_len);
  }

  std::uint32_t submit(const void *req, std::size_t len, std::uint32_t resp_len = gpu::header_size) {
    std::memcpy(ptr(cmd_buf), req, len);
    sg_t out[1] = {{gaddr{cmd_buf}, len}};
    sg_t in[1] = {{gaddr{resp_buf}, resp_len}};
    if (!drv_[0]->try_add(reloco::span<const sg_t>(out, 1), reloco::span<const sg_t>(in, 1), 1) ||
        !drv_[0]->try_publish())
      die("queue add");
    write(reg::queue_notify, 0); // the device runs the command synchronously
    auto done = drv_[0]->try_get_used();
    if (!done || !done->has_value())
      die("no completion");
    std::uint32_t type = 0;
    std::memcpy(&type, ptr(resp_buf), 4);
    return type;
  }

  // Display size as the device reports it (scanout 0).
  gpu::extent display_info() {
    if (command(gpu::cmd_get_display_info, {}, gpu::header_size + 16 * 24) != gpu::resp_ok_display_info)
      die("GET_DISPLAY_INFO");
    std::uint32_t e[6];
    std::memcpy(e, ptr(resp_buf) + gpu::header_size, sizeof e);
    return gpu::extent{e[2], e[3]};
  }

  // Attaches the four scattered bands as the resource's backing.
  void attach_backing() {
    std::uint8_t req[gpu::header_size + 8 + bands * 16] = {};
    const std::uint32_t type = gpu::cmd_resource_attach_backing;
    const std::uint32_t id = resource_id;
    const std::uint32_t count = bands;
    std::memcpy(req, &type, 4);
    std::memcpy(req + 24, &id, 4);
    std::memcpy(req + 28, &count, 4);
    for (std::uint32_t i = 0; i < bands; ++i) {
      const std::uint64_t addr = band_base + i * band_stride;
      const auto len = static_cast<std::uint32_t>(band_bytes);
      std::memcpy(req + 32 + i * 16, &addr, 8);
      std::memcpy(req + 40 + i * 16, &len, 4);
    }
    if (submit(req, sizeof req) != gpu::resp_ok_nodata)
      die("ATTACH_BACKING");
  }

  // Writes pixel (x, y) of the resource's backing in guest RAM, through the scatter list (B, G, R, X).
  void put(std::uint32_t x, std::uint32_t y, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const std::uint64_t linear = (std::uint64_t{y} * width + x) * 4;
    std::uint8_t *p = ptr(band_base + (linear / band_bytes) * band_stride + linear % band_bytes);
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = 0;
  }

  // Tells the device which rectangle of the guest backing changed, then shows it.
  void present(const gpu::rect &r) {
    const std::uint64_t offset = (std::uint64_t{r.y} * width + r.x) * 4;
    if (command(gpu::cmd_transfer_to_host_2d,
                {r.x, r.y, r.width, r.height, static_cast<std::uint32_t>(offset),
                 static_cast<std::uint32_t>(offset >> 32), resource_id, 0}) != gpu::resp_ok_nodata)
      die("TRANSFER_TO_HOST_2D");
    if (command(gpu::cmd_resource_flush, {r.x, r.y, r.width, r.height, resource_id, 0}) != gpu::resp_ok_nodata)
      die("RESOURCE_FLUSH");
  }

private:
  std::uint8_t *ptr(std::uint64_t addr) {
    return reinterpret_cast<std::uint8_t *>(ram_.bytes.data()) + (addr - ram_base);
  }

  std::uint32_t read(std::uint64_t off) {
    std::byte b[4] = {};
    if (!dev_.try_read(off, reloco::span<std::byte>(b, 4)))
      die("mmio read");
    return load_le<std::uint32_t>(reloco::span<const std::byte>(b, 4));
  }
  void write(std::uint64_t off, std::uint32_t v) {
    std::byte b[4] = {};
    store_le<std::uint32_t>(reloco::span<std::byte>(b, 4), v);
    if (!dev_.try_write(off, reloco::span<const std::byte>(b, 4)))
      die("mmio write");
  }

  device_t &dev_;
  guest_ram &ram_;
  reloco::array<reloco::array<split_driver_slot, ring_size>, 2> slots_{};
  reloco::array<reloco::optional<driver_t>, 2> drv_{};
};

// Background the guest paints once and repaints under the moving box.
void paint_background(guest &g, const gpu::rect &r) {
  for (std::uint32_t y = r.y; y < r.y + r.height; ++y)
    for (std::uint32_t x = r.x; x < r.x + r.width; ++x) {
      const bool grid = (x % 32 == 0) || (y % 32 == 0);
      g.put(x, y, static_cast<std::uint8_t>(grid ? 70 : 20 + x * 40 / width),
            static_cast<std::uint8_t>(grid ? 70 : 20 + y * 40 / height), static_cast<std::uint8_t>(grid ? 110 : 60));
    }
}

void paint_box(guest &g, const gpu::rect &r, std::uint8_t shade) {
  for (std::uint32_t y = r.y; y < r.y + r.height; ++y)
    for (std::uint32_t x = r.x; x < r.x + r.width; ++x)
      g.put(x, y, 255, static_cast<std::uint8_t>(120 + shade / 2), static_cast<std::uint8_t>(shade));
}

} // namespace

int main(int argc, char **argv) {
  long max_frames = -1;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--frames") == 0)
      max_frames = std::atol(argv[i + 1]);

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window *window = SDL_CreateWindow("structo: virtio-gpu", static_cast<int>(width), static_cast<int>(height), 0);
  SDL_Renderer *renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
  SDL_Texture *texture = renderer != nullptr
                             ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                                 static_cast<int>(width), static_cast<int>(height))
                             : nullptr;
  if (texture == nullptr) {
    std::fprintf(stderr, "SDL setup failed: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  // Host side: the scanout surface, and the virtio-gpu device presenting into it. `rgba8888`'s byte
  // order (R, G, B, A) matches SDL_PIXELFORMAT_RGBA32, so the surface can be uploaded as is.
  auto screen = hypervisor::mmio_framebuffer_device<hw::rgba8888>::try_create(width, height);
  if (!screen)
    die("framebuffer allocation");
  hw::gpu_accel_ref accel(screen->pixels());
  display_t display(screen->pixels(), accel);
  gpu_t gpu(display);

  guest_ram ram;
  device_t device(ram.mem, gpu);
  guest drv(device, ram);

  // ---- guest driver start-up -------------------------------------------------------------------------
  drv.probe();
  const gpu::extent size = drv.display_info();
  if (size.width != width || size.height != height)
    die("unexpected display size");
  if (drv.command(gpu::cmd_resource_create_2d, {resource_id, gpu::format_b8g8r8x8, size.width, size.height}) !=
      gpu::resp_ok_nodata)
    die("RESOURCE_CREATE_2D");
  drv.attach_backing();
  if (drv.command(gpu::cmd_set_scanout, {0, 0, size.width, size.height, 0, resource_id}) != gpu::resp_ok_nodata)
    die("SET_SCANOUT");
  paint_background(drv, gpu::rect{0, 0, width, height});
  drv.present(gpu::rect{0, 0, width, height});

  // ---- frame loop ------------------------------------------------------------------------------------
  constexpr std::uint32_t box = 64;
  std::int32_t bx = 100, by = 80, vx = 4, vy = 3;
  gpu::rect prev{static_cast<std::uint32_t>(bx), static_cast<std::uint32_t>(by), box, box};
  bool running = true;
  long frame = 0;
  while (running && (max_frames < 0 || frame < max_frames)) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
      if (ev.type == SDL_EVENT_QUIT || (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE))
        running = false;

    bx += vx;
    by += vy;
    if (bx < 0 || bx + static_cast<std::int32_t>(box) > static_cast<std::int32_t>(width)) {
      vx = -vx;
      bx += 2 * vx;
    }
    if (by < 0 || by + static_cast<std::int32_t>(box) > static_cast<std::int32_t>(height)) {
      vy = -vy;
      by += 2 * vy;
    }
    const gpu::rect cur{static_cast<std::uint32_t>(bx), static_cast<std::uint32_t>(by), box, box};

    // Erase the old box, draw the new one, and tell the device about the union of both: a partial update.
    paint_background(drv, prev);
    paint_box(drv, cur, static_cast<std::uint8_t>(frame * 3));
    const std::uint32_t x0 = std::min(prev.x, cur.x), y0 = std::min(prev.y, cur.y);
    const std::uint32_t x1 = std::max(prev.x + box, cur.x + box), y1 = std::max(prev.y + box, cur.y + box);
    drv.present(gpu::rect{x0, y0, x1 - x0, y1 - y0});
    prev = cur;

    const auto pixels = screen->pixels().raw();
    (void)SDL_UpdateTexture(texture, nullptr, pixels.data(), static_cast<int>(screen->pixels().stride_bytes()));
    SDL_RenderClear(renderer);
    SDL_RenderTexture(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
    SDL_Delay(16);
    ++frame;
  }

  // Self-check of what reached the host surface: a background pixel and the centre of the last box.
  const auto bg_px = screen->pixels().get_pixel(5, 5);
  const auto box_px = screen->pixels().get_pixel(prev.x + box / 2, prev.y + box / 2);
  const bool picture_ok = bg_px.has_value() && box_px.has_value() && bg_px->r == 20 && bg_px->g == 20 &&
                          bg_px->b == 60 && box_px->r == 255;

  std::printf("frames=%ld commands=%llu transfers=%llu flushes=%llu errors=%llu host_bytes=%llu picture=%s\n", frame,
              static_cast<unsigned long long>(gpu.stats().commands),
              static_cast<unsigned long long>(gpu.stats().transfers),
              static_cast<unsigned long long>(gpu.stats().flushes),
              static_cast<unsigned long long>(gpu.stats().errors),
              static_cast<unsigned long long>(gpu.host_bytes()), picture_ok ? "ok" : "WRONG");

  SDL_DestroyTexture(texture);
  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  return picture_ok ? 0 : 1;
}
