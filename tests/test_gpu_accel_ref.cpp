// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/gpu_accel_ref.hpp>

#include <vector>

using structo::hw::framebuffer;
using structo::hw::gpu_accel_ref;
using structo::hw::gpu_accel_traits;
using structo::hw::rgb_color;
using structo::hw::xrgb8888;
using reloco::error;
using reloco::span;

namespace {

framebuffer<xrgb8888> make_framebuffer(std::vector<std::byte> &storage, std::size_t w, std::size_t h) {
  storage.assign(w * h * 4, std::byte{0});
  auto fb = framebuffer<xrgb8888>::try_create(span<std::byte>(storage.data(), storage.size()), w, h, w * 4);
  return std::move(*fb);
}

// get_pixel() returns a temporary reloco::result<rgb_color>; operator* is deleted on an rvalue, so this
// binds it to a named local first before dereferencing.
rgb_color pixel_at(const framebuffer<xrgb8888> &fb, std::size_t x, std::size_t y) {
  auto result = fb.get_pixel(x, y);
  return *result;
}

// A minimal backend implementing only the mandatory gpu_accel_traits members, to exercise the
// synthesized draw_rect/put_pixel fallbacks.
struct minimal_backend {
  framebuffer<xrgb8888> fb;
};

} // namespace

template <> struct structo::hw::gpu_accel_traits<minimal_backend> {
  static std::size_t width(const minimal_backend &b) noexcept { return b.fb.width(); }
  static std::size_t height(const minimal_backend &b) noexcept { return b.fb.height(); }
  static void clear(minimal_backend &b, rgb_color c) noexcept { b.fb.clear(c); }
  static void fill_rect(minimal_backend &b, std::size_t x, std::size_t y, std::size_t w, std::size_t h,
                       rgb_color c) noexcept {
    b.fb.fill_rect(x, y, w, h, c);
  }
  static void draw_line(minimal_backend &b, std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1,
                       std::ptrdiff_t y1, rgb_color c) noexcept {
    b.fb.draw_line(x0, y0, x1, y1, c);
  }
};

namespace {

TEST(GpuAccelRef, UnboundRefIsSafeAndReportsZeroDimensions) {
  gpu_accel_ref ref;
  EXPECT_FALSE(static_cast<bool>(ref));
  EXPECT_EQ(ref.width(), 0u);
  EXPECT_EQ(ref.height(), 0u);
  ref.clear({1, 2, 3}); // no-op, must not crash
  ref.fill_rect(0, 0, 10, 10, {1, 2, 3});
  ref.draw_rect(0, 0, 10, 10, {1, 2, 3});
  ref.draw_line(0, 0, 10, 10, {1, 2, 3});
  EXPECT_EQ(ref.put_pixel(0, 0, {1, 2, 3}).error(), error::unsupported_operation);
}

TEST(GpuAccelRef, ForwardsDirectlyToFramebufferBackend) {
  std::vector<std::byte> storage;
  auto fb = make_framebuffer(storage, 16, 16);
  gpu_accel_ref ref(fb);

  EXPECT_EQ(ref.width(), 16u);
  EXPECT_EQ(ref.height(), 16u);

  ref.clear({0, 0, 0});
  ref.fill_rect(2, 2, 4, 4, {10, 20, 30});
  EXPECT_EQ(pixel_at(fb, 3, 3), (rgb_color{10, 20, 30}));
  EXPECT_EQ(pixel_at(fb, 0, 0), (rgb_color{0, 0, 0}));

  ref.draw_rect(0, 0, 5, 5, {255, 0, 0});
  EXPECT_EQ(pixel_at(fb, 0, 0), (rgb_color{255, 0, 0}));
  EXPECT_EQ(pixel_at(fb, 4, 4), (rgb_color{255, 0, 0}));

  ref.draw_line(0, 0, 15, 15, {0, 255, 0});
  EXPECT_EQ(pixel_at(fb, 7, 7), (rgb_color{0, 255, 0}));

  auto result = ref.put_pixel(8, 8, {1, 2, 3});
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(pixel_at(fb, 8, 8), (rgb_color{1, 2, 3}));

  EXPECT_EQ(ref.put_pixel(100, 100, {1, 2, 3}).error(), error::out_of_bounds);
}

TEST(GpuAccelRef, SynthesizesDrawRectAndPutPixelWhenBackendOmitsThem) {
  minimal_backend backend;
  std::vector<std::byte> storage;
  backend.fb = make_framebuffer(storage, 16, 16);
  gpu_accel_ref ref(backend);

  ref.draw_rect(2, 2, 4, 4, {7, 8, 9}); // synthesized from four draw_line calls
  EXPECT_EQ(pixel_at(backend.fb, 2, 2), (rgb_color{7, 8, 9}));
  EXPECT_EQ(pixel_at(backend.fb, 5, 5), (rgb_color{7, 8, 9}));
  EXPECT_EQ(pixel_at(backend.fb, 3, 3), (rgb_color{0, 0, 0})); // interior untouched

  auto result = ref.put_pixel(9, 9, {11, 22, 33}); // synthesized from a 1x1 fill_rect
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(pixel_at(backend.fb, 9, 9), (rgb_color{11, 22, 33}));

  EXPECT_EQ(ref.put_pixel(100, 100, {1, 2, 3}).error(), error::out_of_bounds);
}

} // namespace
