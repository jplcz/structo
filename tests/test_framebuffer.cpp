// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/framebuffer.hpp>

using namespace structo::hw;

namespace {

template <typename PixelFormat>
framebuffer<PixelFormat> make_framebuffer(reloco::span<std::byte> storage, std::size_t width, std::size_t height) {
  auto fb = framebuffer<PixelFormat>::try_create(storage, width, height, width * PixelFormat::bytes_per_pixel);
  return std::move(*fb);
}

// `get_pixel()` returns a `result<rgb_color>`; dereferencing a temporary
// result is blocked by reloco's rvalue-safety guard, so this binds it to
// a named local first, keeping the test bodies below concise.
template <typename PixelFormat> rgb_color px(const framebuffer<PixelFormat> &fb, std::size_t x, std::size_t y) {
  auto result = fb.get_pixel(x, y);
  return *result;
}

} // namespace

TEST(FramebufferTest, TryCreateFailsOnZeroDimensionsOrTooSmallStrideOrBuffer) {
  std::byte storage[16 * 16 * 4];
  reloco::span<std::byte> buf(storage, sizeof(storage));

  EXPECT_FALSE(framebuffer<xrgb8888>::try_create(buf, 0, 16, 16 * 4));
  EXPECT_FALSE(framebuffer<xrgb8888>::try_create(buf, 16, 0, 16 * 4));

  auto bad_stride = framebuffer<xrgb8888>::try_create(buf, 16, 16, 16 * 4 - 1);
  ASSERT_FALSE(bad_stride);
  EXPECT_EQ(bad_stride.error(), reloco::error::invalid_argument);

  auto too_small = framebuffer<xrgb8888>::try_create(reloco::span<std::byte>(storage, 4), 16, 16, 16 * 4);
  ASSERT_FALSE(too_small);
  EXPECT_EQ(too_small.error(), reloco::error::out_of_range);

  auto ok = framebuffer<xrgb8888>::try_create(buf, 16, 16, 16 * 4);
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->width(), 16u);
  EXPECT_EQ(ok->height(), 16u);
  EXPECT_EQ(ok->stride_bytes(), 16u * 4);
}

TEST(FramebufferTest, PutAndGetPixelRoundTripAndBoundsCheck) {
  std::byte storage[4 * 4 * 4]{};
  auto fb = make_framebuffer<xrgb8888>(reloco::span<std::byte>(storage, sizeof(storage)), 4, 4);

  ASSERT_TRUE(fb.put_pixel(1, 2, rgb_color{10, 20, 30}));
  auto got = fb.get_pixel(1, 2);
  ASSERT_TRUE(got);
  EXPECT_EQ(*got, (rgb_color{10, 20, 30, 0xff}));

  auto oob_put = fb.put_pixel(4, 0, rgb_color::red());
  ASSERT_FALSE(oob_put);
  EXPECT_EQ(oob_put.error(), reloco::error::out_of_bounds);

  auto oob_get = fb.get_pixel(0, 4);
  ASSERT_FALSE(oob_get);
  EXPECT_EQ(oob_get.error(), reloco::error::out_of_bounds);
}

TEST(FramebufferTest, ClearFillsEveryPixel) {
  std::byte storage[3 * 3 * 3]{};
  auto fb = make_framebuffer<rgb888>(reloco::span<std::byte>(storage, sizeof(storage)), 3, 3);

  fb.clear(rgb_color::red());
  for (std::size_t y = 0; y < 3; ++y) {
    for (std::size_t x = 0; x < 3; ++x) {
      EXPECT_EQ(px(fb, x, y), rgb_color::red());
    }
  }
}

TEST(FramebufferTest, FillRectIsClippedToBounds) {
  std::byte storage[4 * 4 * 3]{};
  auto fb = make_framebuffer<rgb888>(reloco::span<std::byte>(storage, sizeof(storage)), 4, 4);
  fb.clear(rgb_color::black());

  // Rectangle partially off the right/bottom edges.
  fb.fill_rect(2, 2, 10, 10, rgb_color::white());

  EXPECT_EQ(px(fb, 2, 2), rgb_color::white());
  EXPECT_EQ(px(fb, 3, 3), rgb_color::white());
  EXPECT_EQ(px(fb, 1, 1), rgb_color::black());
  EXPECT_EQ(px(fb, 0, 0), rgb_color::black());

  // A fully offscreen rectangle is a silent no-op.
  fb.fill_rect(100, 100, 5, 5, rgb_color::red());
  EXPECT_EQ(px(fb, 3, 3), rgb_color::white());
}

TEST(FramebufferTest, DrawRectDrawsOnlyTheOutline) {
  std::byte storage[5 * 5 * 3]{};
  auto fb = make_framebuffer<rgb888>(reloco::span<std::byte>(storage, sizeof(storage)), 5, 5);
  fb.clear(rgb_color::black());

  fb.draw_rect(1, 1, 3, 3, rgb_color::white());

  // Corners and edges of the 3x3 outline at (1,1)-(3,3) are white.
  EXPECT_EQ(px(fb, 1, 1), rgb_color::white());
  EXPECT_EQ(px(fb, 3, 1), rgb_color::white());
  EXPECT_EQ(px(fb, 1, 3), rgb_color::white());
  EXPECT_EQ(px(fb, 3, 3), rgb_color::white());
  EXPECT_EQ(px(fb, 2, 1), rgb_color::white());

  // The interior is left untouched.
  EXPECT_EQ(px(fb, 2, 2), rgb_color::black());
}

TEST(FramebufferTest, DrawLineCoversEndpointsAndClipsOffscreenPortions) {
  std::byte storage[4 * 4 * 3]{};
  auto fb = make_framebuffer<rgb888>(reloco::span<std::byte>(storage, sizeof(storage)), 4, 4);
  fb.clear(rgb_color::black());

  fb.draw_line(0, 0, 3, 3, rgb_color::green());
  EXPECT_EQ(px(fb, 0, 0), rgb_color::green());
  EXPECT_EQ(px(fb, 3, 3), rgb_color::green());

  // A line extending beyond the framebuffer only draws its onscreen portion.
  fb.draw_line(-5, 0, 3, 0, rgb_color::red());
  EXPECT_EQ(px(fb, 0, 0), rgb_color::red());
  EXPECT_EQ(px(fb, 3, 0), rgb_color::red());
}

TEST(FramebufferTest, BlitCopiesClippedOverlapBetweenDifferingPixelFormats) {
  std::byte src_storage[4 * 4 * 4]{};
  auto src = make_framebuffer<xrgb8888>(reloco::span<std::byte>(src_storage, sizeof(src_storage)), 4, 4);
  src.clear(rgb_color::blue());

  std::byte dst_storage[2 * 2 * 3]{};
  auto dst = make_framebuffer<rgb888>(reloco::span<std::byte>(dst_storage, sizeof(dst_storage)), 2, 2);
  dst.clear(rgb_color::black());

  // Source rectangle request (4x4) is larger than both the source and
  // destination framebuffers; blit clips to whatever actually overlaps.
  dst.blit(0, 0, src, 0, 0, 4, 4);

  EXPECT_EQ(px(dst, 0, 0), rgb_color::blue());
  EXPECT_EQ(px(dst, 1, 1), rgb_color::blue());
}

TEST(FramebufferTest, Rgb565RoundTripsBlackWhiteAndPrimaries) {
  std::byte storage[1 * 1 * 2]{};
  auto fb = make_framebuffer<rgb565>(reloco::span<std::byte>(storage, sizeof(storage)), 1, 1);

  ASSERT_TRUE(fb.put_pixel(0, 0, rgb_color::black()));
  EXPECT_EQ(px(fb, 0, 0), (rgb_color{0, 0, 0, 0xff}));

  ASSERT_TRUE(fb.put_pixel(0, 0, rgb_color::white()));
  EXPECT_EQ(px(fb, 0, 0), (rgb_color{0xff, 0xff, 0xff, 0xff}));
}

TEST(FramebufferTest, Gray8EncodeIsLossyAveragingAcrossChannels) {
  std::byte storage[1 * 1 * 1]{};
  auto fb = make_framebuffer<gray8>(reloco::span<std::byte>(storage, sizeof(storage)), 1, 1);

  ASSERT_TRUE(fb.put_pixel(0, 0, rgb_color{90, 90, 90}));
  auto got = fb.get_pixel(0, 0);
  ASSERT_TRUE(got);
  EXPECT_EQ(got->r, 90);
  EXPECT_EQ(got->g, 90);
  EXPECT_EQ(got->b, 90);
}

TEST(FramebufferTest, Bgr888StoresChannelsInReverseOrder) {
  std::byte storage[1 * 1 * 3]{};
  auto fb = make_framebuffer<bgr888>(reloco::span<std::byte>(storage, sizeof(storage)), 1, 1);

  ASSERT_TRUE(fb.put_pixel(0, 0, rgb_color{10, 20, 30}));
  EXPECT_EQ(storage[0], std::byte{30}); // blue first in memory
  EXPECT_EQ(storage[1], std::byte{20});
  EXPECT_EQ(storage[2], std::byte{10});

  auto got = fb.get_pixel(0, 0);
  ASSERT_TRUE(got);
  EXPECT_EQ(*got, (rgb_color{10, 20, 30, 0xff}));
}

TEST(FramebufferTest, StrideLargerThanWidthLeavesPaddingUntouched) {
  // 2x2 framebuffer with an extra padding column per row.
  std::byte storage[2 * 3 * 3]{};
  auto fb = framebuffer<rgb888>::try_create(reloco::span<std::byte>(storage, sizeof(storage)), 2, 2, 3 * 3);
  ASSERT_TRUE(fb);

  ASSERT_TRUE(fb->put_pixel(0, 1, rgb_color::red()));
  // Row 1 starts at byte offset stride_bytes() * 1 = 9.
  EXPECT_EQ(storage[9], std::byte{0xff});
  // The padding column (x == 2, outside `width()`) for row 0 is never touched.
  EXPECT_EQ(storage[6], std::byte{0});
}
