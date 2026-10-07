// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtio_gpu_framebuffer.hpp
 * @brief `framebuffer_display<PixelFormat>`: a `virtio_gpu_function` `Display` over a single
 * `hw::framebuffer`, so a guest's virtio-gpu output lands in the same pixels the rest of structo
 * draws into (a host window texture, `mmio_framebuffer_device::pixels()`, a real scanout buffer).
 *
 * @code
 * auto fbdev = structo::hypervisor::mmio_framebuffer_device<structo::hw::xrgb8888>::try_create(1024, 768);
 * structo::virtio::framebuffer_display<structo::hw::xrgb8888> display(fbdev->pixels());
 * structo::virtio::virtio_gpu_function<guest_space, decltype(display)> gpu(display);
 * @endcode
 *
 * Scanout 0 only. Guest pixels (B,G,R,A/X bytes) are converted per pixel into `PixelFormat`, so any
 * framebuffer format works; the guest sees the framebuffer's size as its display size.
 * Disabling the scanout clears it to black. The hardware cursor is not drawn (the guest then keeps
 * its cursor in the framebuffer, which Linux's DRM driver handles).
 */

#include "virtio_gpu.hpp"

#include <cstdint>
#include <reloco/error.hpp>
#include <structo/hw/framebuffer.hpp>

namespace structo::virtio {

template <typename PixelFormat> class framebuffer_display {
public:
  explicit framebuffer_display(hw::framebuffer<PixelFormat> &fb) noexcept : fb_(&fb) {}

  [[nodiscard]] reloco::result<gpu::extent> try_display_size(std::uint32_t scanout) noexcept {
    if (scanout != 0)
      return reloco::unexpected(reloco::error::not_found);
    return gpu::extent{static_cast<std::uint32_t>(fb_->width()), static_cast<std::uint32_t>(fb_->height())};
  }

  [[nodiscard]] reloco::result<void> try_present(std::uint32_t scanout, const gpu::surface &src,
                                                 const gpu::rect &src_rect, std::uint32_t dst_x,
                                                 std::uint32_t dst_y) noexcept {
    if (scanout != 0)
      return reloco::unexpected(reloco::error::not_found);
    if (src.stride < src.width * gpu::bytes_per_pixel ||
        src.pixels.size() < std::size_t{src.stride} * src.height)
      return reloco::unexpected(reloco::error::invalid_argument);
    for (std::uint32_t row = 0; row < src_rect.height; ++row) {
      const std::size_t base = std::size_t{src_rect.y + row} * src.stride + std::size_t{src_rect.x} * 4;
      for (std::uint32_t col = 0; col < src_rect.width; ++col) {
        const std::size_t o = base + std::size_t{col} * 4;
        const hw::rgb_color px{static_cast<std::uint8_t>(src.pixels[o + 2]), static_cast<std::uint8_t>(src.pixels[o + 1]),
                               static_cast<std::uint8_t>(src.pixels[o])};
        (void)fb_->put_pixel(dst_x + col, dst_y + row, px); // off-framebuffer pixels are clipped
      }
    }
    return {};
  }

  void scanout_disabled(std::uint32_t scanout) noexcept {
    if (scanout == 0)
      fb_->clear(hw::rgb_color{0, 0, 0});
  }

private:
  hw::framebuffer<PixelFormat> *fb_;
};

} // namespace structo::virtio
