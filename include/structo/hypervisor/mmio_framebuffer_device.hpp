// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmio_framebuffer_device.hpp
 * @brief `structo::hypervisor::mmio_framebuffer_device<PixelFormat>`: an
 * emulated linear-framebuffer graphics device, mirroring `hw::framebuffer`
 * into the same "plain-memory data plane, trapped control plane" split
 * `mmio_text_console` already establishes for text consoles -- here for
 * raw pixels instead of character cells.
 *
 * ## Two address ranges, one device (again)
 *
 * - `pixels()` returns the underlying `hw::framebuffer<PixelFormat>`
 *   directly: the embedding hypervisor maps its `raw()` bytes straight
 *   into the guest's physical address space (stage-2/EPT, out of this
 *   header's scope), so guest pixel writes land in memory with no VM
 *   exit, and the *hypervisor* can call every `framebuffer` drawing
 *   helper (`put_pixel`/`fill_rect`/`draw_line`/`blit`/...) directly on
 *   the very same memory -- unlike `mmio_text_console`, there is no
 *   separate shadow-cell state to keep in sync here: a pixel buffer is
 *   both readable and writable in place, so one `framebuffer` object
 *   already serves both sides.
 * - The `mmio_device_traits<mmio_framebuffer_device<PixelFormat>>`
 *   specialization models a small trapped "virtual GPU control"
 *   register window: read-only `width`/`height`/`stride_bytes`/
 *   `bytes_per_pixel`/`pixel_format_id` (so a guest driver can size and
 *   self-identify the surface without any side channel), plus a
 *   write-only `present` doorbell the guest pokes after finishing a
 *   frame -- purely a hint a polling hypervisor main loop can check via
 *   @ref take_dirty() instead of re-blitting every pixel on every tick.
 *
 * ## Ownership: two construction modes
 *
 * - @ref try_allocate / @ref try_create allocate and own a page-aligned
 *   backing block via a `reloco::allocator_ref`, exactly like
 *   `mmio_text_console` (see that header's docs for the full rationale:
 *   page alignment so the whole block maps cleanly into guest physical
 *   memory with no partial trailing page, padding zeroed, ownership
 *   tracked directly as this class's own members rather than recovered
 *   indirectly through the `framebuffer` it hands out).
 * - @ref try_bind_external instead *wraps* caller-supplied memory this
 *   device never allocates and never frees -- e.g. an SDL texture's
 *   locked pixel buffer, already at whatever stride/alignment SDL
 *   picked, with an optional byte @p offset_bytes into it (for an atlas
 *   or a sub-rectangle of a larger surface). This is the intended path
 *   for plugging a `mmio_framebuffer_device` straight into a host
 *   renderer for a demo/debug UI: construct it over `SDL_LockTexture`'s
 *   own buffer, draw guest/hypervisor output directly into exactly the
 *   pixels SDL is about to present, and skip the extra allocate-then-
 *   copy step an owned backing block would otherwise require. A
 *   caller choosing this mode is responsible for the external memory's
 *   lifetime (keeping it locked/valid) and, if it is ever mapped into a
 *   guest at all, for that memory's own alignment/isolation properties
 *   -- this header makes no page-alignment guarantee for it, unlike the
 *   owned-allocation path.
 *
 * @code
 * // Owned, guest-mappable framebuffer:
 * auto maker = structo::hypervisor::mmio_framebuffer_device<structo::hw::xrgb8888>::try_create(640, 480);
 * auto screen = std::move(maker.value());
 * structo::hypervisor::mmio_device_ref control(screen);   // the small trapped register window
 * // map screen.pixels().raw() directly into guest physical memory for the pixel data itself.
 * screen.pixels().clear({0, 0, 0});
 * screen.pixels().fill_rect(10, 10, 100, 40, {0, 128, 255});
 *
 * // Bound to an SDL texture's locked pixels for a host-side demo, no guest involved:
 * void *locked_pixels; int pitch;
 * SDL_LockTexture(texture, nullptr, &locked_pixels, &pitch);
 * auto bound = structo::hypervisor::mmio_framebuffer_device<structo::hw::xrgb8888>::try_bind_external(
 *     reloco::span<std::byte>(static_cast<std::byte *>(locked_pixels), std::size_t(pitch) * 480), 640, 480,
 *     std::size_t(pitch));
 * bound->pixels().draw_line(0, 0, 639, 479, {255, 0, 0});
 * SDL_UnlockTexture(texture);
 * @endcode
 */

#include "mmio_device_ref.hpp"

#include <structo/hw/framebuffer.hpp>

#include <cstddef>
#include <cstdint>
#include <reloco/allocator.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>
#include <utility>

namespace structo::hypervisor {

/** @brief Maps a `hw::framebuffer` `PixelFormat` to a stable, wire-format identifier a guest driver can
 * read back from @ref mmio_framebuffer_device's control window (`control_off_pixel_format_id`) to
 * self-identify the surface's pixel encoding. Unrecognized/caller-supplied formats report @ref
 * pixel_format_id_custom; specialize this for a custom `PixelFormat` to report a project-specific id
 * instead. */
template <typename PixelFormat> struct pixel_format_id {
  static constexpr std::uint32_t value = 0xFFFFFFFFu;
};

/** @brief Reserved id reported for any `PixelFormat` without its own @ref pixel_format_id specialization. */
inline constexpr std::uint32_t pixel_format_id_custom = 0xFFFFFFFFu;

template <> struct pixel_format_id<hw::rgb888> { static constexpr std::uint32_t value = 1; };
template <> struct pixel_format_id<hw::bgr888> { static constexpr std::uint32_t value = 2; };
template <> struct pixel_format_id<hw::xrgb8888> { static constexpr std::uint32_t value = 3; };
template <> struct pixel_format_id<hw::rgba8888> { static constexpr std::uint32_t value = 4; };
template <> struct pixel_format_id<hw::rgb565> { static constexpr std::uint32_t value = 5; };
template <> struct pixel_format_id<hw::gray8> { static constexpr std::uint32_t value = 6; };

/**
 * @brief Emulated linear-framebuffer graphics device: a plain `hw::framebuffer<PixelFormat>` (either
 * allocator-owned and page-aligned, or bound over caller-supplied external memory) plus a small trapped
 * "virtual GPU control" register window. Move-only. See the @file-level docs.
 */
template <typename PixelFormat> class mmio_framebuffer_device {
public:
  using pixel_format = PixelFormat;
  static constexpr std::size_t bytes_per_pixel = PixelFormat::bytes_per_pixel;

  /** @brief Default VM page size used by @ref try_allocate/@ref try_create; see `mmio_text_console`'s docs
   * for why the owned allocation path rounds up to this. */
  static constexpr std::size_t default_page_size = 4096;

  /** @brief Virtual GPU control-register offsets, each a 4-byte register (`present` is write-only). */
  static constexpr std::uint64_t control_off_width = 0;
  static constexpr std::uint64_t control_off_height = 4;
  static constexpr std::uint64_t control_off_stride_bytes = 8;
  static constexpr std::uint64_t control_off_bytes_per_pixel = 12;
  static constexpr std::uint64_t control_off_pixel_format_id = 16;
  static constexpr std::uint64_t control_off_present = 20;
  /** @brief Size in bytes of the "virtual GPU control" register window -- what `mmio_device_ref` should be
   * sized to, entirely separate from `pixels().raw()`'s own (much larger) size. */
  static constexpr std::size_t control_window_size = 24;

  /** @brief Constructs an empty (`width() == height() == 0`), storage-less device bound to @p alloc for a
   * future @ref try_allocate-style use. Never fails. */
  constexpr explicit mmio_framebuffer_device(allocator_ref alloc = default_allocator()) noexcept : alloc_(alloc) {}

  mmio_framebuffer_device(mmio_framebuffer_device &&other) noexcept
      : alloc_(other.alloc_), owned_base_(other.owned_base_), owned_size_(other.owned_size_),
        fb_(std::move(other.fb_)), dirty_(other.dirty_) {
    other.owned_base_ = nullptr;
    other.owned_size_ = 0;
    other.fb_ = hw::framebuffer<PixelFormat>();
    other.dirty_ = false;
  }

  mmio_framebuffer_device &operator=(mmio_framebuffer_device &&other) noexcept {
    if (this != &other) {
      release();
      alloc_ = other.alloc_;
      owned_base_ = other.owned_base_;
      owned_size_ = other.owned_size_;
      fb_ = std::move(other.fb_);
      dirty_ = other.dirty_;
      other.owned_base_ = nullptr;
      other.owned_size_ = 0;
      other.fb_ = hw::framebuffer<PixelFormat>();
      other.dirty_ = false;
    }
    return *this;
  }

  // Owns at most one unique allocation (when constructed via try_allocate/try_create); not copyable, same
  // as mmio_text_console/dynamic_bitmap.
  mmio_framebuffer_device(const mmio_framebuffer_device &) = delete;
  mmio_framebuffer_device &operator=(const mmio_framebuffer_device &) = delete;

  ~mmio_framebuffer_device() noexcept { release(); }

  /**
   * @brief Fallible allocation factory: allocates and owns a page-aligned backing block.
   * @param alloc Allocator backing the framebuffer's storage.
   * @param width,height Pixel dimensions; the backing block needs `width * height * bytes_per_pixel` bytes
   * (tightly packed, no row padding), rounded up to a whole number of @p page_size bytes.
   * @param page_size VM page size the allocation's base address and byte size are aligned/rounded to; must
   * be a power of two. Defaults to @ref default_page_size.
   * @return The device (all pixels black, cleared padding), or `error::invalid_argument` if
   * `width`/`height`/`page_size` are zero or `page_size` is not a power of two, or
   * `error::allocation_failed` if @p alloc could not provide the backing storage.
   */
  [[nodiscard]] static result<mmio_framebuffer_device> try_allocate(allocator_ref alloc, std::size_t width,
                                                                    std::size_t height,
                                                                    std::size_t page_size = default_page_size) noexcept {
    if (width == 0 || height == 0 || page_size == 0 || (page_size & (page_size - 1)) != 0) {
      return unexpected(error::invalid_argument);
    }

    mmio_framebuffer_device dev(alloc);
    const std::size_t stride = width * bytes_per_pixel;
    const std::size_t pixel_bytes = stride * height;
    const std::size_t aligned_bytes = (pixel_bytes + page_size - 1) & ~(page_size - 1);

    // allocate() returns a block of at least aligned_bytes, which is only checked for failure here.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    auto res = alloc.allocate(aligned_bytes, page_size);
    RELOCO_END_UNSAFE_BUFFER_USAGE
    if (!res) {
      return unexpected(res.error());
    }

    // Sole owner of res->ptr/res->size, recorded directly here -- never recovered indirectly through fb_
    // (a non-owning view over a leading sub-span of it). See mmio_text_console.hpp's docs for why.
    dev.owned_base_ = static_cast<std::byte *>(res->ptr);
    dev.owned_size_ = res->size; // the allocator's absorbed size -- what deallocate() must be called with
    for (std::byte &b : span<std::byte>(dev.owned_base_, dev.owned_size_)) {
      b = std::byte{0};
    }

    auto fb = hw::framebuffer<PixelFormat>::try_create(span<std::byte>(dev.owned_base_, pixel_bytes), width, height,
                                                       stride);
    // Cannot fail here: width/height are already non-zero and stride == width * bytes_per_pixel exactly, and
    // the span passed in is exactly stride * height bytes, well within owned_size_.
    dev.fb_ = std::move(*fb);
    return dev;
  }

  /** @brief `try_allocate()` using `reloco::default_allocator()`. */
  [[nodiscard]] static result<mmio_framebuffer_device> try_create(std::size_t width, std::size_t height,
                                                                   std::size_t page_size = default_page_size) noexcept {
    return try_allocate(default_allocator(), width, height, page_size);
  }

  /**
   * @brief Binds this device over externally-owned memory instead of allocating its own -- e.g. an SDL
   * texture's locked pixel buffer. This device never allocates or frees @p external_memory; the caller
   * must keep it valid (and, for a locked texture, locked) for as long as this device is used, and is
   * responsible for its own alignment/sharing properties if ever mapped into a guest -- unlike @ref
   * try_allocate, no page-alignment guarantee is made here.
   * @param external_memory The caller-owned backing storage.
   * @param width,height Pixel dimensions.
   * @param stride_bytes Bytes between the start of one row and the next (may exceed `width *
   * bytes_per_pixel`, e.g. a host renderer's own row padding).
   * @param offset_bytes Byte offset into @p external_memory the pixel data starts at (for binding a
   * sub-rectangle/atlas region of a larger surface). Defaults to `0`.
   * @return `error::invalid_argument` if `width`/`height` are zero or `stride_bytes` is smaller than
   * `width * bytes_per_pixel`; `error::out_of_range` if @p offset_bytes/@p stride_bytes * @p height does
   * not fit within @p external_memory.
   */
  [[nodiscard]] static result<mmio_framebuffer_device> try_bind_external(span<std::byte> external_memory,
                                                                         std::size_t width, std::size_t height,
                                                                         std::size_t stride_bytes,
                                                                         std::size_t offset_bytes = 0) noexcept {
    auto region = external_memory.try_subspan(offset_bytes);
    if (!region) {
      return unexpected(region.error());
    }
    auto fb = hw::framebuffer<PixelFormat>::try_create(*region, width, height, stride_bytes);
    if (!fb) {
      return unexpected(fb.error());
    }

    mmio_framebuffer_device dev;
    dev.fb_ = std::move(*fb); // owned_base_ stays null: this device never frees external_memory
    return dev;
  }

  [[nodiscard]] std::size_t width() const noexcept { return fb_.width(); }
  [[nodiscard]] std::size_t height() const noexcept { return fb_.height(); }
  [[nodiscard]] std::size_t stride_bytes() const noexcept { return fb_.stride_bytes(); }

  /** @brief The underlying pixel buffer: both the hypervisor-side rendering surface (every `hw::framebuffer`
   * drawing helper works directly on it) and, for the owned-allocation path, the span the embedding
   * hypervisor should map directly into guest physical memory. */
  [[nodiscard]] hw::framebuffer<PixelFormat> &pixels() noexcept { return fb_; }
  [[nodiscard]] const hw::framebuffer<PixelFormat> &pixels() const noexcept { return fb_; }

  /** @brief Whether this device owns its backing allocation (`try_allocate`/`try_create`) as opposed to
   * being bound over external memory (`try_bind_external`). */
  [[nodiscard]] bool owns_allocation() const noexcept { return owned_base_ != nullptr; }

  /** @brief Reports, then clears, whether the guest has written to `control_off_present` since the last
   * call -- a hint a polling hypervisor main loop can use to only re-blit/present when the guest actually
   * finished a frame, instead of unconditionally every tick. Purely advisory: nothing stops a hypervisor
   * from reading @ref pixels() at any time regardless of this flag. */
  bool take_dirty() noexcept {
    bool was_dirty = dirty_;
    dirty_ = false;
    return was_dirty;
  }

  /** @brief Marks the surface dirty, as if the guest had just written `control_off_present` -- lets the
   * hypervisor side force a present (e.g. right after its own direct `pixels()` draw calls) without going
   * through the MMIO control window itself. */
  void mark_dirty() noexcept { dirty_ = true; }

private:
  void release() noexcept {
    if (owned_base_ != nullptr) {
      // owned_base_/owned_size_ are exactly the block returned by allocate().
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
      alloc_.deallocate(owned_base_, owned_size_);
      RELOCO_END_UNSAFE_BUFFER_USAGE
      owned_base_ = nullptr;
      owned_size_ = 0;
      fb_ = hw::framebuffer<PixelFormat>();
    }
  }

  allocator_ref alloc_;
  std::byte *owned_base_ = nullptr; // null when bound over external memory; sole owner otherwise
  std::size_t owned_size_ = 0;      // the allocator's absorbed size -- what deallocate() must be called with
  hw::framebuffer<PixelFormat> fb_{};
  bool dirty_ = false;
};

} // namespace structo::hypervisor

namespace structo::hypervisor {

/** @brief The "virtual GPU control" register window: `width`/`height`/`stride_bytes`/`bytes_per_pixel`/
 * `pixel_format_id` read-only, `present` write-only (and also readable, reporting the current dirty
 * state). See the @file-level docs' "Two address ranges, one device (again)". */
template <typename PixelFormat> struct mmio_device_traits<mmio_framebuffer_device<PixelFormat>> {
  using device = mmio_framebuffer_device<PixelFormat>;

  static std::size_t size(device &) noexcept { return device::control_window_size; }

  static result<void> try_read(device &d, std::uint64_t offset, span<std::byte> dst) noexcept {
    std::uint32_t value;
    switch (offset) {
    case device::control_off_width:
      value = static_cast<std::uint32_t>(d.width());
      break;
    case device::control_off_height:
      value = static_cast<std::uint32_t>(d.height());
      break;
    case device::control_off_stride_bytes:
      value = static_cast<std::uint32_t>(d.stride_bytes());
      break;
    case device::control_off_bytes_per_pixel:
      value = static_cast<std::uint32_t>(device::bytes_per_pixel);
      break;
    case device::control_off_pixel_format_id:
      value = pixel_format_id<PixelFormat>::value;
      break;
    case device::control_off_present:
      value = d.take_dirty() ? 1U : 0U; // reading also acknowledges the present, like `take_dirty()` itself
      break;
    default:
      return unexpected(error::invalid_argument);
    }
    if (dst.size() != sizeof(value)) {
      return unexpected(error::invalid_argument);
    }
    // dst.size() was checked equal to sizeof(value) above.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    __builtin_memcpy(dst.data(), &value, sizeof(value));
    RELOCO_END_UNSAFE_BUFFER_USAGE
    return {};
  }

  static result<void> try_write(device &d, std::uint64_t offset, span<const std::byte> src) noexcept {
    if (offset != device::control_off_present) {
      return unexpected(error::permission_denied); // every other register is read-only
    }
    if (src.size() != sizeof(std::uint32_t)) {
      return unexpected(error::invalid_argument);
    }
    d.mark_dirty(); // the written value itself carries no meaning -- this is a pure doorbell
    return {};
  }
};

} // namespace structo::hypervisor
