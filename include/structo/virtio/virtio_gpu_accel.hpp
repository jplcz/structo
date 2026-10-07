// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtio_gpu_accel.hpp
 * @brief `framebuffer_accel_display<PixelFormat>`: a `virtio_gpu_function` `Display` for a device
 * that has both a scanout `hw::framebuffer` and a `hw::gpu_accel_ref` over it.
 *
 * - **Pixels** (guest `RESOURCE_FLUSH`) are converted straight into the framebuffer: `gpu_accel_ref`
 *   has no raw-pixel upload, and the framebuffer is the memory the scanout actually reads.
 * - **Fills** (disabling the scanout) go through the accel ref, so a hardware-accelerated backend
 *   does the work; an unbound ref falls back to `framebuffer::clear`.
 *
 * Only what both types offer today is used; nothing in `gpu_accel_ref` is extended. No hardware
 * cursor hooks (neither type can draw an image overlay), so guests keep their cursor in the
 * framebuffer. If a backend renders asynchronously, flush it from the embedding's own present step.
 *
 * @code
 * structo::hw::gpu_accel_ref accel(fb);
 * structo::virtio::framebuffer_accel_display<structo::hw::xrgb8888> display(fb, accel);
 * structo::virtio::virtio_gpu_function<guest_space, decltype(display)> gpu(display);
 * @endcode
 */

#include "virtio_gpu_framebuffer.hpp"

#include <cstdint>
#include <structo/hw/gpu_accel_ref.hpp>

namespace structo::virtio {

template <typename PixelFormat> class framebuffer_accel_display : public framebuffer_display<PixelFormat> {
public:
  framebuffer_accel_display(hw::framebuffer<PixelFormat> &fb, hw::gpu_accel_ref accel) noexcept
      : framebuffer_display<PixelFormat>(fb), fb_(&fb), accel_(accel) {}

  void scanout_disabled(std::uint32_t scanout) noexcept {
    if (scanout != 0)
      return;
    if (accel_)
      accel_.clear(hw::rgb_color{0, 0, 0});
    else
      fb_->clear(hw::rgb_color{0, 0, 0});
  }

private:
  hw::framebuffer<PixelFormat> *fb_;
  hw::gpu_accel_ref accel_;
};

} // namespace structo::virtio
