// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include "virtio_mmio_fixture.hpp"

#include <structo/hw/framebuffer.hpp>
#include <structo/virtio/virtio_gpu.hpp>
#include <structo/virtio/virtio_gpu_accel.hpp>
#include <structo/virtio/virtio_gpu_framebuffer.hpp>

namespace {

using namespace virtio_test;

struct recording_display {
  struct present_call {
    std::uint32_t scanout;
    gpu::rect src;
    std::uint32_t dx, dy;
    std::uint8_t first_b, first_g, first_r; // first presented pixel
  };
  reloco::result<gpu::extent> try_display_size(std::uint32_t i) noexcept {
    if (i != 0)
      return reloco::unexpected(reloco::error::not_found);
    return gpu::extent{8, 4};
  }
  reloco::result<void> try_present(std::uint32_t s, const gpu::surface &src, const gpu::rect &r, std::uint32_t dx,
                                   std::uint32_t dy) noexcept {
    if (fail)
      return reloco::unexpected(reloco::error::io_error);
    const std::size_t o = std::size_t{r.y} * src.stride + std::size_t{r.x} * 4;
    last = present_call{s,
                        r,
                        dx,
                        dy,
                        static_cast<std::uint8_t>(src.pixels[o]),
                        static_cast<std::uint8_t>(src.pixels[o + 1]),
                        static_cast<std::uint8_t>(src.pixels[o + 2])};
    ++presents;
    return {};
  }
  void scanout_disabled(std::uint32_t) noexcept { ++disabled; }
  void cursor_update(std::uint32_t, const gpu::surface *img, std::uint32_t x, std::uint32_t y, std::uint32_t hx,
                     std::uint32_t) noexcept {
    cursor_visible = img != nullptr;
    cx = x;
    cy = y;
    chot = hx;
  }
  void cursor_move(std::uint32_t, std::uint32_t x, std::uint32_t y) noexcept {
    cx = x;
    cy = y;
  }
  present_call last{};
  int presents = 0;
  int disabled = 0;
  bool fail = false;
  bool cursor_visible = false;
  std::uint32_t cx = 0, cy = 0, chot = 0;
};

using gpu_t = virtio_gpu_function<guest_space, recording_display, 4>;

struct display_holder {
  recording_display display;
};

constexpr std::uint64_t kReq = 0;
constexpr std::uint64_t kResp = 0x400;
constexpr std::uint64_t kPix = 0x1000; // guest backing pages

class VirtioGpuTest : private display_holder, public mmio_fixture<gpu_t, 2> {
protected:
  VirtioGpuTest() : mmio_fixture<gpu_t, 2>(display, reloco::default_allocator(), gpu::limits{4096, 8}) {}
  recording_display &disp() { return display; }

  using words = std::initializer_list<std::uint64_t>;

  // Sends a command: header(type, flags, fence) followed by 32-bit words in `body`. Returns the response type.
  std::uint32_t cmd(std::uint32_t type, std::initializer_list<std::uint32_t> body, std::uint32_t flags = 0,
                    std::uint64_t fence = 0, std::uint32_t resp_len = 24) {
    reloco::array<std::byte, 128> b{};
    auto s = b.as_span();
    store_le<std::uint32_t>(s, type);
    store_le<std::uint32_t>(s.subspan(4), flags);
    store_le<std::uint64_t>(s.subspan(8), fence);
    std::size_t off = 24;
    for (auto w : body) {
      store_le<std::uint32_t>(s.subspan(off), w);
      off += 4;
    }
    return send(b.data(), off, resp_len);
  }

  std::uint32_t send(const void *req, std::size_t len, std::uint32_t resp_len = 24) {
    put(kReq, req, len);
    sg_t out[1] = {{gaddr{kBuf + kReq}, len}};
    sg_t in[1] = {{gaddr{kBuf + kResp}, resp_len}};
    EXPECT_TRUE(drv_[0]->try_add(reloco::span<const sg_t>(out, 1), reloco::span<const sg_t>(in, 1), 1).has_value());
    EXPECT_TRUE(drv_[0]->try_publish().has_value());
    kick(0);
    used_len = reap(0);
    if (used_len < 4)
      return 0;
    std::uint32_t t = 0;
    get(kResp, &t, 4);
    return t;
  }

  void make_resource(std::uint32_t id = 1, std::uint32_t w = 8, std::uint32_t h = 4) {
    ASSERT_EQ(cmd(gpu::cmd_resource_create_2d, {id, gpu::format_b8g8r8x8, w, h}), gpu::resp_ok_nodata);
  }

  // Attaches one backing page-run at kPix and fills it with a gradient: pixel i = (b=i, g=i+1, r=i+2).
  void attach(std::uint32_t id = 1, std::uint32_t pixels = 32) {
    for (std::uint32_t i = 0; i < pixels; ++i) {
      const std::uint8_t px[4] = {static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i + 1),
                                  static_cast<std::uint8_t>(i + 2), 0};
      put(kPix + i * 4, px, 4);
    }
    reloco::array<std::byte, 64> b{};
    auto s = b.as_span();
    store_le<std::uint32_t>(s, gpu::cmd_resource_attach_backing);
    store_le<std::uint32_t>(s.subspan(24), id);
    store_le<std::uint32_t>(s.subspan(28), 1);
    store_le<std::uint64_t>(s.subspan(32), kBuf + kPix);
    store_le<std::uint32_t>(s.subspan(40), pixels * 4);
    ASSERT_EQ(send(b.data(), 48), gpu::resp_ok_nodata);
  }

  std::int64_t used_len = -1;
};

TEST_F(VirtioGpuTest, IdentityAndConfig) {
  EXPECT_EQ(rd(reg::device_id), 16u);
  EXPECT_EQ(dev_.size(), reg::config + gpu::config_size);
  EXPECT_EQ(rd(reg::config + 8), 1u);  // num_scanouts
  EXPECT_EQ(rd(reg::config + 12), 0u); // num_capsets
}

TEST_F(VirtioGpuTest, DisplayInfoReportsScanout) {
  bring_up();
  EXPECT_EQ(cmd(gpu::cmd_get_display_info, {}, 0, 0, 24 + 16 * 24), gpu::resp_ok_display_info);
  EXPECT_EQ(used_len, 24 + 16 * 24);
  std::uint32_t e[6] = {};
  get(kResp + 24, e, sizeof e);
  EXPECT_EQ(e[2], 8u);
  EXPECT_EQ(e[3], 4u);
  EXPECT_EQ(e[4], 1u); // enabled
  get(kResp + 24 + 24, e, sizeof e);
  EXPECT_EQ(e[4], 0u); // scanout 1 disabled
}

TEST_F(VirtioGpuTest, DisplayInfoClearsEventBit) {
  fn_.notify_display_changed();
  EXPECT_EQ(rd(reg::config), 1u);
  bring_up();
  EXPECT_EQ(cmd(gpu::cmd_get_display_info, {}, 0, 0, 24 + 16 * 24), gpu::resp_ok_display_info);
  EXPECT_EQ(rd(reg::config), 0u);
}

TEST_F(VirtioGpuTest, CreateTransferFlushPresentsPixels) {
  bring_up();
  make_resource();
  attach();
  ASSERT_EQ(cmd(gpu::cmd_set_scanout, {0, 0, 8, 4, 0, 1}), gpu::resp_ok_nodata);
  ASSERT_EQ(cmd(gpu::cmd_transfer_to_host_2d, {0, 0, 8, 4, 0, 0, 1, 0}), gpu::resp_ok_nodata);
  ASSERT_EQ(cmd(gpu::cmd_resource_flush, {0, 0, 8, 4, 1, 0}), gpu::resp_ok_nodata);
  EXPECT_EQ(disp().presents, 1);
  EXPECT_EQ(disp().last.src.width, 8u);
  EXPECT_EQ(disp().last.first_b, 0);
  EXPECT_EQ(disp().last.first_g, 1);
  EXPECT_EQ(disp().last.first_r, 2);
  EXPECT_EQ(fn_.stats().transfers, 1u);
  EXPECT_EQ(fn_.stats().flushes, 1u);
}

TEST_F(VirtioGpuTest, PartialTransferUsesOffsetAndRowStride) {
  bring_up();
  make_resource();
  attach();
  ASSERT_EQ(cmd(gpu::cmd_set_scanout, {0, 0, 8, 4, 0, 1}), gpu::resp_ok_nodata);
  // Rect (2,1) 3x2: guest offset of its first pixel is (1*8 + 2) pixels in.
  ASSERT_EQ(cmd(gpu::cmd_transfer_to_host_2d, {2, 1, 3, 2, 10 * 4, 0, 1, 0}), gpu::resp_ok_nodata);
  ASSERT_EQ(cmd(gpu::cmd_resource_flush, {2, 1, 3, 2, 1, 0}), gpu::resp_ok_nodata);
  EXPECT_EQ(disp().last.src.x, 2u);
  EXPECT_EQ(disp().last.src.y, 1u);
  EXPECT_EQ(disp().last.dx, 2u);
  EXPECT_EQ(disp().last.dy, 1u);
  EXPECT_EQ(disp().last.first_b, 10); // pixel index 10
}

TEST_F(VirtioGpuTest, ScanoutRectOffsetsDestination) {
  bring_up();
  make_resource();
  attach();
  ASSERT_EQ(cmd(gpu::cmd_set_scanout, {4, 2, 4, 2, 0, 1}), gpu::resp_ok_nodata); // scanout = lower-right quadrant
  ASSERT_EQ(cmd(gpu::cmd_transfer_to_host_2d, {0, 0, 8, 4, 0, 0, 1, 0}), gpu::resp_ok_nodata);
  ASSERT_EQ(cmd(gpu::cmd_resource_flush, {0, 0, 8, 4, 1, 0}), gpu::resp_ok_nodata);
  EXPECT_EQ(disp().last.src.x, 4u);
  EXPECT_EQ(disp().last.src.y, 2u);
  EXPECT_EQ(disp().last.src.width, 4u);
  EXPECT_EQ(disp().last.dx, 0u);
  EXPECT_EQ(disp().last.dy, 0u);
}

TEST_F(VirtioGpuTest, FlushWithoutScanoutPresentsNothing) {
  bring_up();
  make_resource();
  ASSERT_EQ(cmd(gpu::cmd_resource_flush, {0, 0, 8, 4, 1, 0}), gpu::resp_ok_nodata);
  EXPECT_EQ(disp().presents, 0);
}

TEST_F(VirtioGpuTest, DisplayFailureReportsUnspec) {
  bring_up();
  make_resource();
  ASSERT_EQ(cmd(gpu::cmd_set_scanout, {0, 0, 8, 4, 0, 1}), gpu::resp_ok_nodata);
  disp().fail = true;
  EXPECT_EQ(cmd(gpu::cmd_resource_flush, {0, 0, 8, 4, 1, 0}), gpu::resp_err_unspec);
}

TEST_F(VirtioGpuTest, RejectsBadCreates) {
  bring_up();
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {0, 2, 8, 4}), gpu::resp_err_invalid_parameter);  // id 0
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {1, 99, 8, 4}), gpu::resp_err_invalid_parameter); // format
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {1, 2, 0, 4}), gpu::resp_err_invalid_parameter);
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {1, 2, 0xffffffffu, 0xffffffffu}), gpu::resp_err_invalid_parameter);
  make_resource(1);
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {1, 2, 8, 4}), gpu::resp_err_invalid_resource_id); // duplicate
  EXPECT_EQ(fn_.resource_count(), 1u);
}

TEST_F(VirtioGpuTest, EnforcesMemoryAndSlotLimits) {
  bring_up();
  make_resource(1, 16, 16); // 1024 bytes of the 4096 budget
  make_resource(2, 16, 16);
  make_resource(3, 16, 16);
  make_resource(4, 16, 16);
  EXPECT_EQ(fn_.host_bytes(), 4096u);
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {5, 2, 1, 1}), gpu::resp_err_out_of_memory); // budget and slots
  EXPECT_EQ(cmd(gpu::cmd_resource_unref, {1, 0}), gpu::resp_ok_nodata);
  EXPECT_EQ(fn_.host_bytes(), 3072u);
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {5, 2, 32, 32}), gpu::resp_err_out_of_memory); // 4096 > 1024 free
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {5, 2, 16, 16}), gpu::resp_ok_nodata);
}

TEST_F(VirtioGpuTest, RejectsBadRectsAndIds) {
  bring_up();
  make_resource();
  attach();
  EXPECT_EQ(cmd(gpu::cmd_set_scanout, {0, 0, 8, 4, 1, 1}), gpu::resp_err_invalid_scanout_id);
  EXPECT_EQ(cmd(gpu::cmd_set_scanout, {0, 0, 9, 4, 0, 1}), gpu::resp_err_invalid_parameter);
  EXPECT_EQ(cmd(gpu::cmd_set_scanout, {0, 0, 8, 4, 0, 7}), gpu::resp_err_invalid_resource_id);
  EXPECT_EQ(cmd(gpu::cmd_resource_flush, {7, 0, 0xfffffffeu, 1, 1, 0}), gpu::resp_err_invalid_parameter); // wrap
  EXPECT_EQ(cmd(gpu::cmd_resource_flush, {0, 0, 0, 4, 1, 0}), gpu::resp_err_invalid_parameter);           // empty
  EXPECT_EQ(cmd(gpu::cmd_resource_flush, {0, 0, 8, 4, 9, 0}), gpu::resp_err_invalid_resource_id);
  EXPECT_EQ(cmd(gpu::cmd_transfer_to_host_2d, {0, 0, 9, 4, 0, 0, 1, 0}), gpu::resp_err_invalid_parameter);
  // Offset pushing the read past the backing (32 pixels = 128 bytes).
  EXPECT_EQ(cmd(gpu::cmd_transfer_to_host_2d, {0, 0, 8, 4, 4, 0, 1, 0}), gpu::resp_err_invalid_parameter);
  EXPECT_EQ(cmd(gpu::cmd_transfer_to_host_2d, {0, 0, 8, 4, 0xfffffff0u, 0xffffffffu, 1, 0}),
            gpu::resp_err_invalid_parameter);
  EXPECT_EQ(cmd(gpu::cmd_resource_unref, {9, 0}), gpu::resp_err_invalid_resource_id);
  EXPECT_GE(fn_.stats().errors, 9u);
}

TEST_F(VirtioGpuTest, TransferNeedsBackingAndDetachRemovesIt) {
  bring_up();
  make_resource();
  EXPECT_EQ(cmd(gpu::cmd_transfer_to_host_2d, {0, 0, 8, 4, 0, 0, 1, 0}), gpu::resp_err_unspec);
  attach();
  EXPECT_EQ(cmd(gpu::cmd_resource_detach_backing, {1, 0}), gpu::resp_ok_nodata);
  EXPECT_EQ(cmd(gpu::cmd_transfer_to_host_2d, {0, 0, 8, 4, 0, 0, 1, 0}), gpu::resp_err_unspec);
}

TEST_F(VirtioGpuTest, AttachValidation) {
  bring_up();
  make_resource();
  reloco::array<std::byte, 96> b{};
  auto s = b.as_span();
  store_le<std::uint32_t>(s, gpu::cmd_resource_attach_backing);
  store_le<std::uint32_t>(s.subspan(24), 1);
  store_le<std::uint32_t>(s.subspan(28), 9); // more than max_backing_entries (8)
  EXPECT_EQ(send(b.data(), 96), gpu::resp_err_invalid_parameter);
  store_le<std::uint32_t>(s.subspan(28), 3); // claims 3 entries but only 1 is sent
  EXPECT_EQ(send(b.data(), 48), gpu::resp_err_invalid_parameter);
  store_le<std::uint32_t>(s.subspan(28), 1);
  store_le<std::uint64_t>(s.subspan(32), ~std::uint64_t{0}); // addr + len wraps
  store_le<std::uint32_t>(s.subspan(40), 16);
  EXPECT_EQ(send(b.data(), 48), gpu::resp_err_invalid_parameter);
  store_le<std::uint32_t>(s.subspan(24), 5); // unknown resource
  EXPECT_EQ(send(b.data(), 48), gpu::resp_err_invalid_resource_id);
}

TEST_F(VirtioGpuTest, AttachTwiceIsRefused) {
  bring_up();
  make_resource();
  attach();
  reloco::array<std::byte, 48> b{};
  auto s = b.as_span();
  store_le<std::uint32_t>(s, gpu::cmd_resource_attach_backing);
  store_le<std::uint32_t>(s.subspan(24), 1);
  store_le<std::uint32_t>(s.subspan(28), 1);
  store_le<std::uint64_t>(s.subspan(32), kBuf + kPix);
  store_le<std::uint32_t>(s.subspan(40), 128);
  EXPECT_EQ(send(b.data(), 48), gpu::resp_err_unspec);
}

TEST_F(VirtioGpuTest, UnrefDisablesScanout) {
  bring_up();
  make_resource();
  ASSERT_EQ(cmd(gpu::cmd_set_scanout, {0, 0, 8, 4, 0, 1}), gpu::resp_ok_nodata);
  ASSERT_EQ(cmd(gpu::cmd_resource_unref, {1, 0}), gpu::resp_ok_nodata);
  EXPECT_EQ(disp().disabled, 1);
  EXPECT_EQ(cmd(gpu::cmd_resource_flush, {0, 0, 8, 4, 1, 0}), gpu::resp_err_invalid_resource_id);
  ASSERT_EQ(cmd(gpu::cmd_set_scanout, {0, 0, 0, 0, 0, 0}), gpu::resp_ok_nodata); // explicit disable
  EXPECT_EQ(disp().disabled, 2);
}

TEST_F(VirtioGpuTest, FenceIsEchoed) {
  bring_up();
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {1, 2, 8, 4}, gpu::flag_fence, 0x1122334455667788ull),
            gpu::resp_ok_nodata);
  std::uint32_t flags = 0;
  std::uint64_t fence = 0;
  get(kResp + 4, &flags, 4);
  get(kResp + 8, &fence, 8);
  EXPECT_EQ(flags, gpu::flag_fence);
  EXPECT_EQ(fence, 0x1122334455667788ull);
}

TEST_F(VirtioGpuTest, UnknownAndShortCommandsAreAnswered) {
  bring_up();
  EXPECT_EQ(cmd(0x108, {0, 0}), gpu::resp_err_unspec);                               // GET_CAPSET_INFO: no capsets
  EXPECT_EQ(cmd(0x10a, {0, 0}), gpu::resp_err_unspec);                               // GET_EDID: not offered
  EXPECT_EQ(cmd(gpu::cmd_resource_create_2d, {1}), gpu::resp_err_invalid_parameter); // truncated body
  std::uint8_t tiny[4] = {};
  EXPECT_EQ(send(tiny, sizeof tiny), gpu::resp_err_invalid_parameter);
}

TEST_F(VirtioGpuTest, SmallResponseBufferGetsNothing) {
  bring_up();
  EXPECT_EQ(cmd(gpu::cmd_get_display_info, {}, 0, 0, 24), 0u); // 24-byte buffer can't hold the 408-byte reply
  EXPECT_EQ(used_len, 0);
}

TEST_F(VirtioGpuTest, CursorCommandsReachDisplay) {
  bring_up();
  make_resource(2, 8, 4);
  reloco::array<std::byte, 56> b{};
  auto s = b.as_span();
  store_le<std::uint32_t>(s, gpu::cmd_update_cursor);
  store_le<std::uint32_t>(s.subspan(24), 0); // scanout
  store_le<std::uint32_t>(s.subspan(28), 30);
  store_le<std::uint32_t>(s.subspan(32), 40);
  store_le<std::uint32_t>(s.subspan(40), 2); // resource
  store_le<std::uint32_t>(s.subspan(44), 5); // hot_x
  put(0x800, b.data(), b.size());
  post_out(1, 0x800, b.size());
  kick(1);
  EXPECT_EQ(reap(1), 0); // no response on the cursor queue
  EXPECT_TRUE(disp().cursor_visible);
  EXPECT_EQ(disp().cx, 30u);
  EXPECT_EQ(disp().chot, 5u);

  store_le<std::uint32_t>(s, gpu::cmd_move_cursor);
  store_le<std::uint32_t>(s.subspan(28), 31);
  put(0x800, b.data(), b.size());
  post_out(1, 0x800, b.size());
  kick(1);
  EXPECT_EQ(reap(1), 0);
  EXPECT_EQ(disp().cx, 31u);
  EXPECT_EQ(disp().cy, 40u);
}

// Counts accel calls so the test can see the fill went through the accel ref, not the framebuffer.
struct counting_backend {
  hw::framebuffer<hw::xrgb8888> *fb;
  int clears = 0;
};

} // namespace

template <> struct structo::hw::gpu_accel_traits<counting_backend> {
  static std::size_t width(const counting_backend &b) noexcept { return b.fb->width(); }
  static std::size_t height(const counting_backend &b) noexcept { return b.fb->height(); }
  static void clear(counting_backend &b, rgb_color c) noexcept {
    ++b.clears;
    b.fb->clear(c);
  }
  static void fill_rect(counting_backend &b, std::size_t x, std::size_t y, std::size_t w, std::size_t h,
                        rgb_color c) noexcept {
    b.fb->fill_rect(x, y, w, h, c);
  }
  static void draw_line(counting_backend &b, std::ptrdiff_t x0, std::ptrdiff_t y0, std::ptrdiff_t x1, std::ptrdiff_t y1,
                        rgb_color c) noexcept {
    b.fb->draw_line(x0, y0, x1, y1, c);
  }
};

namespace {

TEST(VirtioGpuFramebufferTest, DisplayConvertsBgrxIntoFramebuffer) {
  reloco::array<std::byte, 4 * 4 * 2> store{};
  auto fb = hw::framebuffer<hw::xrgb8888>::try_create(store.as_span(), 4, 2, 16);
  ASSERT_TRUE(fb.has_value());
  framebuffer_display<hw::xrgb8888> display(*fb);

  auto size = display.try_display_size(0);
  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(size->width, 4u);
  EXPECT_FALSE(display.try_display_size(1).has_value());

  reloco::array<std::byte, 4 * 4 * 2> px{};
  px[0] = std::byte{0x30}; // B
  px[1] = std::byte{0x20}; // G
  px[2] = std::byte{0x10}; // R
  gpu::surface src{reloco::span<const std::byte>(px.data(), px.size()), 4, 2, 16, gpu::format_b8g8r8x8};
  ASSERT_TRUE(display.try_present(0, src, gpu::rect{0, 0, 2, 1}, 2, 1).has_value());
  auto p = fb->get_pixel(2, 1);
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->r, 0x10);
  EXPECT_EQ(p->g, 0x20);
  EXPECT_EQ(p->b, 0x30);
  // Destination overrun is clipped, not an error.
  EXPECT_TRUE(display.try_present(0, src, gpu::rect{0, 0, 4, 2}, 3, 1).has_value());
  EXPECT_FALSE(display.try_present(1, src, gpu::rect{0, 0, 1, 1}, 0, 0).has_value());
  display.scanout_disabled(0);
  auto cleared = fb->get_pixel(2, 1);
  ASSERT_TRUE(cleared.has_value());
  EXPECT_EQ(cleared->r, 0);
}

std::uint8_t blue_at(const hw::framebuffer<hw::xrgb8888> &fb, std::size_t x, std::size_t y) {
  auto p = fb.get_pixel(x, y);
  return p.has_value() ? p->b : std::uint8_t{0xee};
}

TEST(VirtioGpuFramebufferTest, AccelDisplayPresentsToFramebufferAndClearsThroughAccel) {
  reloco::array<std::byte, 4 * 4 * 2> store{};
  auto fb = hw::framebuffer<hw::xrgb8888>::try_create(store.as_span(), 4, 2, 16);
  ASSERT_TRUE(fb.has_value());
  counting_backend backend{&*fb};
  hw::gpu_accel_ref accel(backend);
  framebuffer_accel_display<hw::xrgb8888> display(*fb, accel);

  reloco::array<std::byte, 4 * 4 * 2> px{};
  px[0] = std::byte{0x30};
  px[2] = std::byte{0x10};
  gpu::surface src{reloco::span<const std::byte>(px.data(), px.size()), 4, 2, 16, gpu::format_b8g8r8x8};
  ASSERT_TRUE(display.try_present(0, src, gpu::rect{0, 0, 1, 1}, 1, 1).has_value());
  EXPECT_EQ(blue_at(*fb, 1, 1), 0x30);
  EXPECT_EQ(backend.clears, 0);

  display.scanout_disabled(0);
  EXPECT_EQ(backend.clears, 1);
  EXPECT_EQ(blue_at(*fb, 1, 1), 0);

  // Unbound accel falls back to the framebuffer itself.
  framebuffer_accel_display<hw::xrgb8888> plain(*fb, hw::gpu_accel_ref());
  ASSERT_TRUE(plain.try_present(0, src, gpu::rect{0, 0, 1, 1}, 1, 1).has_value());
  plain.scanout_disabled(0);
  EXPECT_EQ(blue_at(*fb, 1, 1), 0);
}

} // namespace
