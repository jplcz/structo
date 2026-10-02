// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/framebuffer_console.hpp>

using namespace structo::hw;

namespace {

constexpr std::size_t kFbWidth = 64;
constexpr std::size_t kFbHeight = 32;
constexpr std::size_t kCols = kFbWidth / block_font_8x8::glyph_width;   // 8
constexpr std::size_t kRows = kFbHeight / block_font_8x8::glyph_height; // 4

// `framebuffer<PixelFormat>::get_pixel()` returns a `result<rgb_color>`;
// dereferencing a temporary result is blocked by reloco's rvalue-safety
// guard, so this binds it to a named local first.
rgb_color px(const framebuffer<xrgb8888> &fb, std::size_t x, std::size_t y) {
  auto r = fb.get_pixel(x, y);
  return *r;
}

} // namespace

TEST(FramebufferConsoleTest, TryCreateFailsOnTooSmallFramebufferOrCellStorage) {
  std::byte fb_storage[kFbWidth * kFbHeight * 4]{};
  auto fb = framebuffer<xrgb8888>::try_create(reloco::span<std::byte>(fb_storage, sizeof(fb_storage)), kFbWidth,
                                              kFbHeight, kFbWidth * 4);
  ASSERT_TRUE(fb);

  // A framebuffer too small to fit even one glyph.
  std::byte tiny_storage[4 * 4 * 4]{};
  auto tiny_fb =
      framebuffer<xrgb8888>::try_create(reloco::span<std::byte>(tiny_storage, sizeof(tiny_storage)), 4, 4, 4 * 4);
  ASSERT_TRUE(tiny_fb);
  console_cell cells[1]{};
  auto too_small_fb = framebuffer_console<xrgb8888>::try_create(*tiny_fb, reloco::span<console_cell>(cells, 1));
  EXPECT_FALSE(too_small_fb);

  console_cell too_few_cells[1]{};
  auto bad_storage = framebuffer_console<xrgb8888>::try_create(*fb, reloco::span<console_cell>(too_few_cells, 1));
  ASSERT_FALSE(bad_storage);
  EXPECT_EQ(bad_storage.error(), reloco::error::out_of_range);

  console_cell cell_storage[kCols * kRows]{};
  auto ok = framebuffer_console<xrgb8888>::try_create(*fb, reloco::span<console_cell>(cell_storage, kCols * kRows));
  ASSERT_TRUE(ok);
  EXPECT_EQ(ok->columns(), kCols);
  EXPECT_EQ(ok->rows(), kRows);
}

TEST(FramebufferConsoleTest, TryCreateClearsEveryCellToSpaceOnBlack) {
  std::byte fb_storage[kFbWidth * kFbHeight * 4]{};
  auto fb = framebuffer<xrgb8888>::try_create(reloco::span<std::byte>(fb_storage, sizeof(fb_storage)), kFbWidth,
                                              kFbHeight, kFbWidth * 4);
  ASSERT_TRUE(fb);
  console_cell cell_storage[kCols * kRows]{};
  auto console =
      framebuffer_console<xrgb8888>::try_create(*fb, reloco::span<console_cell>(cell_storage, kCols * kRows));
  ASSERT_TRUE(console);

  for (std::size_t y = 0; y < kRows; ++y) {
    for (std::size_t x = 0; x < kCols; ++x) {
      console_cell c = console->get_cell(x, y);
      EXPECT_EQ(c.ch, ' ');
      EXPECT_EQ(c.bg, console_color::black);
    }
  }
}

TEST(FramebufferConsoleTest, PutCellUpdatesShadowAndRasterizesGlyph) {
  std::byte fb_storage[kFbWidth * kFbHeight * 4]{};
  auto fb = framebuffer<xrgb8888>::try_create(reloco::span<std::byte>(fb_storage, sizeof(fb_storage)), kFbWidth,
                                              kFbHeight, kFbWidth * 4);
  ASSERT_TRUE(fb);
  console_cell cell_storage[kCols * kRows]{};
  auto console =
      framebuffer_console<xrgb8888>::try_create(*fb, reloco::span<console_cell>(cell_storage, kCols * kRows));
  ASSERT_TRUE(console);

  console->put_cell(1, 0, 'A', console_color::white, console_color::black);
  console_cell c = console->get_cell(1, 0);
  EXPECT_EQ(c.ch, 'A');
  EXPECT_EQ(c.fg, console_color::white);

  // block_font_8x8 renders 'A' (non-space, printable) as a solid filled
  // block: the whole 8x8 glyph area should now be fg-colored.
  rgb_color expected_fg = console_color_to_rgb(console_color::white);
  for (std::size_t row = 0; row < block_font_8x8::glyph_height; ++row) {
    for (std::size_t col = 0; col < block_font_8x8::glyph_width; ++col) {
      EXPECT_EQ(px(console->pixels(), 1 * block_font_8x8::glyph_width + col, 0 * block_font_8x8::glyph_height + row),
                expected_fg);
    }
  }
}

TEST(FramebufferConsoleTest, SpaceGlyphRendersBlank) {
  std::byte fb_storage[kFbWidth * kFbHeight * 4]{};
  auto fb = framebuffer<xrgb8888>::try_create(reloco::span<std::byte>(fb_storage, sizeof(fb_storage)), kFbWidth,
                                              kFbHeight, kFbWidth * 4);
  ASSERT_TRUE(fb);
  console_cell cell_storage[kCols * kRows]{};
  auto console =
      framebuffer_console<xrgb8888>::try_create(*fb, reloco::span<console_cell>(cell_storage, kCols * kRows));
  ASSERT_TRUE(console);

  console->put_cell(0, 0, ' ', console_color::white, console_color::blue);
  rgb_color expected_bg = console_color_to_rgb(console_color::blue);
  for (std::size_t row = 0; row < block_font_8x8::glyph_height; ++row) {
    for (std::size_t col = 0; col < block_font_8x8::glyph_width; ++col) {
      EXPECT_EQ(px(console->pixels(), col, row), expected_bg);
    }
  }
}

TEST(FramebufferConsoleTest, AdaptsToConsoleRefAndSupportsScrollViaShadowGetCell) {
  std::byte fb_storage[kFbWidth * kFbHeight * 4]{};
  auto fb = framebuffer<xrgb8888>::try_create(reloco::span<std::byte>(fb_storage, sizeof(fb_storage)), kFbWidth,
                                              kFbHeight, kFbWidth * 4);
  ASSERT_TRUE(fb);
  console_cell cell_storage[kCols * kRows]{};
  auto backend =
      framebuffer_console<xrgb8888>::try_create(*fb, reloco::span<console_cell>(cell_storage, kCols * kRows));
  ASSERT_TRUE(backend);

  console_ref ref(*backend);
  EXPECT_EQ(ref.columns(), kCols);
  EXPECT_EQ(ref.rows(), kRows);

  for (std::size_t y = 0; y < kRows; ++y) {
    ASSERT_TRUE(ref.put_char(0, y, static_cast<char>('A' + y), console_color::white, console_color::black));
  }
  ref.scroll_up(1, console_color::light_gray, console_color::black);

  auto c = ref.get_char(0, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->ch, 'B');
}

TEST(ConsoleColorToRgbTest, MapsAllSixteenColorsToDistinctRgbValues) {
  rgb_color seen[16];
  for (int i = 0; i < 16; ++i) {
    seen[i] = console_color_to_rgb(static_cast<console_color>(i));
  }
  for (int i = 0; i < 16; ++i) {
    for (int j = i + 1; j < 16; ++j) {
      EXPECT_NE(seen[i], seen[j]) << "colors " << i << " and " << j << " collide";
    }
  }
  EXPECT_EQ(console_color_to_rgb(console_color::black), rgb_color::black());
  EXPECT_EQ(console_color_to_rgb(console_color::white), rgb_color::white());
}
