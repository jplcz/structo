// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtio_gpu.hpp
 * @brief VIRTIO 2D GPU device function (device ID 16) for `virtio_mmio_device`.
 *
 * A 2D-only virtio-gpu: no 3D/virgl, no blob resources, no EDID. It is what Linux's
 * `virtio_gpu` DRM driver needs for a framebuffer console and a software-rendered desktop.
 *
 * ### Data path
 *
 * ```
 *  guest pages --TRANSFER_TO_HOST_2D--> host resource --RESOURCE_FLUSH--> Display (scanout)
 *  (backing, ATTACH_BACKING)            (device-owned)                    (your binding)
 * ```
 *
 * The device owns one host-side pixel buffer per resource (allocated from a `reloco::allocator_ref`
 * and bounded by `gpu::limits`). The guest's backing pages are only ever *read*, and only
 * during `TRANSFER_TO_HOST_2D`, straight into the host copy, so a guest that rewrites its pages
 * mid-transfer can tear its own picture but cannot influence anything the device later trusts.
 * `RESOURCE_FLUSH` presents the host copy: it never touches guest memory.
 *
 * Pixel formats: `B8G8R8A8_UNORM` (1) and `B8G8R8X8_UNORM` (2), i.e. bytes B,G,R,A/X in memory
 * (little-endian `XRGB8888`, what DRM uses). Other formats are refused with `ERR_INVALID_PARAMETER`.
 *
 * ### Display requirements (the "structo binding")
 * @code
 * struct my_display {
 *   // Size of scanout i in pixels, or an error if the output is disconnected.
 *   reloco::result<structo::virtio::gpu::extent> try_display_size(std::uint32_t scanout) noexcept;
 *   // Show @p src_rect of @p src (resource coordinates) at (dst_x, dst_y) of the scanout.
 *   // @p src is only valid during the call.
 *   reloco::result<void> try_present(std::uint32_t scanout, const structo::virtio::gpu::surface &src,
 *                                    const structo::virtio::gpu::rect &src_rect, std::uint32_t dst_x,
 *                                    std::uint32_t dst_y) noexcept;
 *   // Optional: scanout lost its resource (SET_SCANOUT with resource 0, or the resource was destroyed).
 *   void scanout_disabled(std::uint32_t scanout) noexcept;
 *   // Optional: hardware cursor. @p image is null to hide; valid only during the call (copy it).
 *   void cursor_update(std::uint32_t scanout, const structo::virtio::gpu::surface *image, std::uint32_t x,
 *                      std::uint32_t y, std::uint32_t hot_x, std::uint32_t hot_y) noexcept;
 *   void cursor_move(std::uint32_t scanout, std::uint32_t x, std::uint32_t y) noexcept;
 * };
 * @endcode
 * `virtio_gpu_framebuffer.hpp` provides `framebuffer_display<PixelFormat>`, a ready binding over
 * `hw::framebuffer` (so also over `mmio_framebuffer_device::pixels()`).
 *
 * ### Config space and limits
 * Config is `events_read`, `events_clear`, `num_scanouts`, `num_capsets` (16 bytes). The transport's
 * config space is read-only, so the guest cannot write `events_clear`; the device clears
 * `events_read` itself once the guest issues `GET_DISPLAY_INFO` (what a guest does in response).
 *
 * ### Safety
 * Resource ids are guest-chosen and looked up in a fixed table. Size arithmetic is done in 64-bit
 * with overflow checks; total host memory is capped by `limits::max_total_bytes`; backing entry
 * counts are capped by `limits::max_backing_entries`. Every rectangle is bounds-checked against its
 * resource before any pixel is touched. Malformed commands get an `ERR_*` response and are counted;
 * they never wedge the queue.
 */

#include "le_bytes.hpp"
#include "virtq_chain.hpp"
#include "virtq_types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <reloco/allocator.hpp>
#include <reloco/array.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <type_traits>
#include <utility>

namespace structo::virtio {

namespace gpu {
inline constexpr std::uint32_t device_id = 16;
inline constexpr std::size_t config_size = 16; ///< events_read, events_clear, num_scanouts, num_capsets
inline constexpr std::uint32_t max_scanouts = 16;
inline constexpr std::uint32_t max_dimension = 16384;
inline constexpr std::uint32_t bytes_per_pixel = 4;

inline constexpr std::uint32_t format_b8g8r8a8 = 1;
inline constexpr std::uint32_t format_b8g8r8x8 = 2;

inline constexpr std::uint32_t cmd_get_display_info = 0x100;
inline constexpr std::uint32_t cmd_resource_create_2d = 0x101;
inline constexpr std::uint32_t cmd_resource_unref = 0x102;
inline constexpr std::uint32_t cmd_set_scanout = 0x103;
inline constexpr std::uint32_t cmd_resource_flush = 0x104;
inline constexpr std::uint32_t cmd_transfer_to_host_2d = 0x105;
inline constexpr std::uint32_t cmd_resource_attach_backing = 0x106;
inline constexpr std::uint32_t cmd_resource_detach_backing = 0x107;
inline constexpr std::uint32_t cmd_update_cursor = 0x300;
inline constexpr std::uint32_t cmd_move_cursor = 0x301;

inline constexpr std::uint32_t resp_ok_nodata = 0x1100;
inline constexpr std::uint32_t resp_ok_display_info = 0x1101;
inline constexpr std::uint32_t resp_err_unspec = 0x1200;
inline constexpr std::uint32_t resp_err_out_of_memory = 0x1201;
inline constexpr std::uint32_t resp_err_invalid_scanout_id = 0x1202;
inline constexpr std::uint32_t resp_err_invalid_resource_id = 0x1203;
inline constexpr std::uint32_t resp_err_invalid_parameter = 0x1205;

inline constexpr std::uint32_t flag_fence = 1;
inline constexpr std::uint32_t event_display = 1;

inline constexpr std::size_t header_size = 24;

/** @brief A rectangle in pixels. */
struct rect {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct extent {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

/** @brief Read-only view of a host resource's pixels, bytes B,G,R,A/X per pixel. */
struct surface {
  reloco::span<const std::byte> pixels;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t stride = 0; ///< Bytes per row.
  std::uint32_t format = 0;
};

/** @brief Bounds on what a guest can make the device allocate. */
struct limits {
  std::uint64_t max_total_bytes = std::uint64_t{64} << 20; ///< Sum of all resources' host pixel memory.
  std::uint32_t max_backing_entries = 16384; ///< Scatter entries accepted per `ATTACH_BACKING`.
};

/** @brief Diagnostic counters. */
struct stats {
  std::uint64_t commands = 0;
  std::uint64_t errors = 0; ///< Commands answered with an `ERR_*` code.
  std::uint64_t transfers = 0;
  std::uint64_t flushes = 0;
  std::uint64_t cursor_commands = 0;
};

namespace detail {
template <typename D, typename = void> struct has_scanout_disabled : std::false_type {};
template <typename D>
struct has_scanout_disabled<D, std::void_t<decltype(std::declval<D &>().scanout_disabled(std::uint32_t{}))>>
    : std::true_type {};

template <typename D, typename = void> struct has_cursor : std::false_type {};
template <typename D>
struct has_cursor<D, std::void_t<decltype(std::declval<D &>().cursor_update(
                                      std::uint32_t{}, static_cast<const surface *>(nullptr), std::uint32_t{},
                                      std::uint32_t{}, std::uint32_t{}, std::uint32_t{})),
                                 decltype(std::declval<D &>().cursor_move(std::uint32_t{}, std::uint32_t{},
                                                                          std::uint32_t{}))>> : std::true_type {};

[[nodiscard]] constexpr bool in_bounds(const rect &r, std::uint32_t w, std::uint32_t h) noexcept {
  return r.width != 0 && r.height != 0 && r.x <= w && r.width <= w - r.x && r.y <= h && r.height <= h - r.y;
}
} // namespace detail
} // namespace gpu

/**
 * @brief 2D virtio-gpu function over a `Display`.
 * @tparam GuestSpace Address space of the guest's ring/buffer/backing addresses.
 * @tparam Display Output binding (see the file comment).
 * @tparam MaxResources Size of the resource table.
 * @tparam Scanouts Number of outputs advertised (1..16).
 *
 * Not copyable or movable: it owns allocator blocks and the transport holds a reference to it.
 */
template <typename GuestSpace, typename Display, std::size_t MaxResources = 16, std::uint32_t Scanouts = 1>
class virtio_gpu_function {
public:
  static constexpr std::uint32_t device_id = gpu::device_id;
  static constexpr std::uint32_t queue_count = 2; // 0 = control, 1 = cursor
  static constexpr std::uint32_t queue_max_size = 128;

  static_assert(Scanouts >= 1 && Scanouts <= gpu::max_scanouts, "virtio-gpu supports 1..16 scanouts");
  static_assert(MaxResources >= 1, "need at least one resource slot");

  explicit virtio_gpu_function(Display &display, reloco::allocator_ref alloc = reloco::default_allocator(),
                               gpu::limits lim = {}) noexcept
      : display_(&display), alloc_(alloc), limits_(lim) {}

  virtio_gpu_function(const virtio_gpu_function &) = delete;
  virtio_gpu_function &operator=(const virtio_gpu_function &) = delete;

  ~virtio_gpu_function() {
    for (auto &r : res_)
      release(r);
  }

  [[nodiscard]] std::uint64_t device_features() const noexcept { return 0; }
  [[nodiscard]] std::size_t config_size() const noexcept { return gpu::config_size; }

  [[nodiscard]] reloco::result<void> try_read_config(std::uint64_t offset, reloco::span<std::byte> dst) noexcept {
    if (offset > gpu::config_size || dst.size() > gpu::config_size - offset)
      return reloco::unexpected(reloco::error::out_of_range);
    reloco::array<std::byte, gpu::config_size> raw{};
    auto rs = raw.as_span();
    store_le<std::uint32_t>(rs, events_);
    store_le<std::uint32_t>(rs.subspan(8), Scanouts);
    for (std::size_t i = 0; i < dst.size(); ++i)
      dst[i] = raw[static_cast<std::size_t>(offset) + i];
    return {};
  }

  /** @brief Marks the display configuration changed; follow with the transport's `notify_config_changed()`. */
  void notify_display_changed() noexcept { events_ |= gpu::event_display; }

  [[nodiscard]] const gpu::stats &stats() const noexcept { return stats_; }

  /** @brief Live resources. */
  [[nodiscard]] std::size_t resource_count() const noexcept {
    std::size_t n = 0;
    for (const auto &r : res_)
      n += r.id != 0 ? 1 : 0;
    return n;
  }

  /** @brief Host pixel bytes currently allocated. */
  [[nodiscard]] std::uint64_t host_bytes() const noexcept { return used_bytes_; }

  /** @brief Services queue @p qidx (0 = control, 1 = cursor). */
  template <typename QueueView, typename Mem>
  [[nodiscard]] reloco::result<void> process(Mem &mem, std::uint32_t qidx, QueueView &q) noexcept {
    for (;;) {
      auto popped = q.try_pop(reloco::span<typename QueueView::segment>(segs_.data(), segs_.size()));
      if (!popped)
        return reloco::unexpected(popped.error());
      if (!popped->has_value())
        return {};
      const auto &c = **popped;
      const std::uint32_t written = qidx == 0 ? handle_control(mem, c) : handle_cursor(mem, c);
      if (auto r = q.try_push_used(c, written); !r)
        return r;
    }
  }

private:
  struct backing_entry {
    std::uint64_t addr = 0;
    std::uint32_t len = 0;
  };

  struct resource {
    std::uint32_t id = 0; // 0 = free slot
    std::uint32_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::byte *pixels = nullptr;
    std::size_t pixels_alloc = 0;
    std::uint64_t pixel_bytes = 0;
    backing_entry *entries = nullptr;
    std::size_t entries_alloc = 0;
    std::uint32_t entry_count = 0;
    std::uint64_t backing_bytes = 0;
  };

  struct scanout_binding {
    std::uint32_t res = 0;
    gpu::rect r{};
  };

  using u32span = reloco::span<const std::byte>;

  static std::uint32_t u32(u32span s, std::size_t off) noexcept { return load_le<std::uint32_t>(s.subspan(off)); }

  void release(resource &r) noexcept {
    if (r.pixels != nullptr)
      alloc_.deallocate(r.pixels, r.pixels_alloc);
    if (r.entries != nullptr)
      alloc_.deallocate(r.entries, r.entries_alloc);
    used_bytes_ -= r.pixel_bytes;
    r = resource{};
  }

  resource *find(std::uint32_t id) noexcept {
    if (id == 0)
      return nullptr;
    for (auto &r : res_)
      if (r.id == id)
        return &r;
    return nullptr;
  }

  [[nodiscard]] gpu::surface surface_of(const resource &r) const noexcept {
    return gpu::surface{reloco::span<const std::byte>(r.pixels, static_cast<std::size_t>(r.pixel_bytes)), r.width,
                        r.height, r.width * gpu::bytes_per_pixel, r.format};
  }

  void disable_scanout(std::uint32_t i) noexcept {
    scan_[i] = scanout_binding{};
    if constexpr (gpu::detail::has_scanout_disabled<Display>::value)
      display_->scanout_disabled(i);
  }

  // Writes the response header (echoing a fence if requested) and returns the used length.
  template <typename Mem, typename Chain>
  std::uint32_t reply(Mem &mem, const Chain &c, u32span req, std::uint32_t code, std::size_t total) noexcept {
    if (code >= gpu::resp_err_unspec)
      ++stats_.errors;
    if (c.writable_bytes < total)
      return 0;
    auto rs = resp_.as_span();
    store_le<std::uint32_t>(rs, code);
    const bool fenced = req.size() >= gpu::header_size && (u32(req, 4) & gpu::flag_fence) != 0;
    store_le<std::uint32_t>(rs.subspan(4), fenced ? gpu::flag_fence : 0);
    store_le<std::uint64_t>(rs.subspan(8), fenced ? load_le<std::uint64_t>(req.subspan(8)) : 0);
    store_le<std::uint32_t>(rs.subspan(16), fenced ? u32(req, 16) : 0);
    store_le<std::uint32_t>(rs.subspan(20), fenced ? u32(req, 20) : 0);
    if (!try_write_chain(mem, c.writable, 0, reloco::span<const std::byte>(resp_.data(), total)))
      return 0;
    return static_cast<std::uint32_t>(total);
  }

  template <typename Mem, typename Chain> std::uint32_t handle_control(Mem &mem, const Chain &c) noexcept {
    ++stats_.commands;
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(c.readable_bytes, cmd_.size()));
    if (n < gpu::header_size || !try_read_chain(mem, c.readable, 0, reloco::span<std::byte>(cmd_.data(), n)))
      return reply(mem, c, u32span(), gpu::resp_err_invalid_parameter, gpu::header_size);
    const u32span req(cmd_.data(), n);
    const std::uint32_t type = u32(req, 0);

    std::size_t resp_len = gpu::header_size;
    std::uint32_t code = gpu::resp_err_unspec;
    switch (type) {
    case gpu::cmd_get_display_info:
      code = cmd_display_info(resp_len);
      break;
    case gpu::cmd_resource_create_2d:
      code = n >= gpu::header_size + 16 ? cmd_create(req) : gpu::resp_err_invalid_parameter;
      break;
    case gpu::cmd_resource_unref:
      code = n >= gpu::header_size + 8 ? cmd_unref(req) : gpu::resp_err_invalid_parameter;
      break;
    case gpu::cmd_set_scanout:
      code = n >= gpu::header_size + 24 ? cmd_set_scanout(req) : gpu::resp_err_invalid_parameter;
      break;
    case gpu::cmd_resource_flush:
      code = n >= gpu::header_size + 24 ? cmd_flush(req) : gpu::resp_err_invalid_parameter;
      break;
    case gpu::cmd_transfer_to_host_2d:
      code = n >= gpu::header_size + 32 ? cmd_transfer(mem, req) : gpu::resp_err_invalid_parameter;
      break;
    case gpu::cmd_resource_attach_backing:
      code = n >= gpu::header_size + 8 ? cmd_attach(mem, c, req) : gpu::resp_err_invalid_parameter;
      break;
    case gpu::cmd_resource_detach_backing:
      code = n >= gpu::header_size + 8 ? cmd_detach(req) : gpu::resp_err_invalid_parameter;
      break;
    default:
      break; // unsupported (capsets, EDID, 3D, ...): ERR_UNSPEC
    }
    return reply(mem, c, req, code, resp_len);
  }

  std::uint32_t cmd_display_info(std::size_t &resp_len) noexcept {
    constexpr std::size_t body = gpu::max_scanouts * 24;
    resp_len = gpu::header_size + body;
    auto rs = resp_.as_span();
    for (std::size_t i = gpu::header_size; i < resp_len; ++i)
      resp_[i] = std::byte{0};
    for (std::uint32_t i = 0; i < Scanouts; ++i) {
      auto size = display_->try_display_size(i);
      if (!size || size->width == 0 || size->height == 0)
        continue;
      auto e = rs.subspan(gpu::header_size + i * 24);
      store_le<std::uint32_t>(e.subspan(8), size->width);
      store_le<std::uint32_t>(e.subspan(12), size->height);
      store_le<std::uint32_t>(e.subspan(16), 1);
    }
    events_ = 0; // the guest has re-read the display state
    return gpu::resp_ok_display_info;
  }

  std::uint32_t cmd_create(u32span req) noexcept {
    const std::uint32_t id = u32(req, 24);
    const std::uint32_t format = u32(req, 28);
    const std::uint32_t w = u32(req, 32);
    const std::uint32_t h = u32(req, 36);
    if (id == 0 || (format != gpu::format_b8g8r8a8 && format != gpu::format_b8g8r8x8) || w == 0 || h == 0 ||
        w > gpu::max_dimension || h > gpu::max_dimension)
      return gpu::resp_err_invalid_parameter;
    if (find(id) != nullptr)
      return gpu::resp_err_invalid_resource_id;
    const std::uint64_t bytes = std::uint64_t{w} * h * gpu::bytes_per_pixel; // <= 2^30, no overflow
    if (bytes > limits_.max_total_bytes - std::min(limits_.max_total_bytes, used_bytes_))
      return gpu::resp_err_out_of_memory;
    resource *slot = nullptr;
    for (auto &r : res_)
      if (r.id == 0) {
        slot = &r;
        break;
      }
    if (slot == nullptr)
      return gpu::resp_err_out_of_memory;
    auto block = alloc_.allocate(static_cast<std::size_t>(bytes), 64);
    if (!block)
      return gpu::resp_err_out_of_memory;
    auto mem_span = reloco::span<std::byte>(static_cast<std::byte *>(block->ptr), block->size);
    std::fill(mem_span.begin(), mem_span.end(), std::byte{0});
    *slot = resource{};
    slot->id = id;
    slot->format = format;
    slot->width = w;
    slot->height = h;
    slot->pixels = static_cast<std::byte *>(block->ptr);
    slot->pixels_alloc = block->size;
    slot->pixel_bytes = bytes;
    used_bytes_ += bytes;
    return gpu::resp_ok_nodata;
  }

  std::uint32_t cmd_unref(u32span req) noexcept {
    resource *r = find(u32(req, 24));
    if (r == nullptr)
      return gpu::resp_err_invalid_resource_id;
    for (std::uint32_t i = 0; i < Scanouts; ++i)
      if (scan_[i].res == r->id)
        disable_scanout(i);
    release(*r);
    return gpu::resp_ok_nodata;
  }

  std::uint32_t cmd_set_scanout(u32span req) noexcept {
    const gpu::rect rc{u32(req, 24), u32(req, 28), u32(req, 32), u32(req, 36)};
    const std::uint32_t scanout = u32(req, 40);
    const std::uint32_t id = u32(req, 44);
    if (scanout >= Scanouts)
      return gpu::resp_err_invalid_scanout_id;
    if (id == 0) {
      disable_scanout(scanout);
      return gpu::resp_ok_nodata;
    }
    resource *r = find(id);
    if (r == nullptr)
      return gpu::resp_err_invalid_resource_id;
    if (!gpu::detail::in_bounds(rc, r->width, r->height))
      return gpu::resp_err_invalid_parameter;
    scan_[scanout] = scanout_binding{id, rc};
    return gpu::resp_ok_nodata;
  }

  std::uint32_t cmd_flush(u32span req) noexcept {
    const gpu::rect rc{u32(req, 24), u32(req, 28), u32(req, 32), u32(req, 36)};
    resource *r = find(u32(req, 40));
    if (r == nullptr)
      return gpu::resp_err_invalid_resource_id;
    if (!gpu::detail::in_bounds(rc, r->width, r->height))
      return gpu::resp_err_invalid_parameter;
    ++stats_.flushes;
    const gpu::surface src = surface_of(*r);
    std::uint32_t code = gpu::resp_ok_nodata;
    for (std::uint32_t i = 0; i < Scanouts; ++i) {
      const auto &b = scan_[i];
      if (b.res != r->id)
        continue;
      const std::uint32_t x0 = std::max(rc.x, b.r.x);
      const std::uint32_t y0 = std::max(rc.y, b.r.y);
      const std::uint32_t x1 = std::min(rc.x + rc.width, b.r.x + b.r.width);
      const std::uint32_t y1 = std::min(rc.y + rc.height, b.r.y + b.r.height);
      if (x0 >= x1 || y0 >= y1)
        continue;
      if (!display_->try_present(i, src, gpu::rect{x0, y0, x1 - x0, y1 - y0}, x0 - b.r.x, y0 - b.r.y))
        code = gpu::resp_err_unspec;
    }
    return code;
  }

  // Copies guest backing bytes [off, off + dst.size()) into dst by walking the scatter entries.
  template <typename Mem>
  [[nodiscard]] static reloco::result<void> read_backing(Mem &mem, const resource &r, std::uint64_t off,
                                                         reloco::span<std::byte> dst) noexcept {
    std::size_t done = 0;
    const reloco::span<const backing_entry> entries(r.entries, r.entry_count);
    for (const auto &e : entries) {
      if (done == dst.size())
        break;
      if (off >= e.len) {
        off -= e.len;
        continue;
      }
      const std::size_t n = std::min<std::size_t>(e.len - static_cast<std::size_t>(off), dst.size() - done);
      auto at = phys_addr<void, GuestSpace>{e.addr}.try_add(off);
      if (!at)
        return reloco::unexpected(at.error());
      if (auto rd = virtq_memory_traits<Mem, GuestSpace>::try_read(mem, *at, dst.subspan(done, n)); !rd)
        return rd;
      done += n;
      off = 0;
    }
    if (done != dst.size())
      return reloco::unexpected(reloco::error::out_of_range);
    return {};
  }

  template <typename Mem> std::uint32_t cmd_transfer(Mem &mem, u32span req) noexcept {
    const gpu::rect rc{u32(req, 24), u32(req, 28), u32(req, 32), u32(req, 36)};
    const std::uint64_t offset = load_le<std::uint64_t>(req.subspan(40));
    resource *r = find(u32(req, 48));
    if (r == nullptr)
      return gpu::resp_err_invalid_resource_id;
    if (r->entry_count == 0)
      return gpu::resp_err_unspec; // no backing attached
    if (!gpu::detail::in_bounds(rc, r->width, r->height))
      return gpu::resp_err_invalid_parameter;

    // The guest's rows are packed at the resource's own stride, `offset` addresses the rect's first pixel.
    const std::uint64_t stride = std::uint64_t{r->width} * gpu::bytes_per_pixel;
    const std::uint64_t row_bytes = std::uint64_t{rc.width} * gpu::bytes_per_pixel;
    const std::uint64_t span_bytes = stride * (rc.height - 1) + row_bytes; // <= 2^30
    if (offset > r->backing_bytes || span_bytes > r->backing_bytes - offset)
      return gpu::resp_err_invalid_parameter;

    const std::uint64_t dst0 = std::uint64_t{rc.y} * stride + std::uint64_t{rc.x} * gpu::bytes_per_pixel;
    const reloco::span<std::byte> pix(r->pixels, static_cast<std::size_t>(r->pixel_bytes));
    ++stats_.transfers;
    if (rc.x == 0 && rc.width == r->width) { // full-width rows are contiguous on both sides
      if (!read_backing(mem, *r, offset, pix.subspan(static_cast<std::size_t>(dst0),
                                                    static_cast<std::size_t>(stride * rc.height))))
        return gpu::resp_err_unspec;
      return gpu::resp_ok_nodata;
    }
    for (std::uint32_t row = 0; row < rc.height; ++row) {
      if (!read_backing(mem, *r, offset + stride * row,
                        pix.subspan(static_cast<std::size_t>(dst0 + stride * row), static_cast<std::size_t>(row_bytes))))
        return gpu::resp_err_unspec;
    }
    return gpu::resp_ok_nodata;
  }

  template <typename Mem, typename Chain> std::uint32_t cmd_attach(Mem &mem, const Chain &c, u32span req) noexcept {
    resource *r = find(u32(req, 24));
    if (r == nullptr)
      return gpu::resp_err_invalid_resource_id;
    const std::uint32_t count = u32(req, 28);
    constexpr std::uint64_t entry_wire = 16;
    constexpr std::uint64_t fixed = gpu::header_size + 8;
    if (count == 0 || count > limits_.max_backing_entries || c.readable_bytes < fixed + entry_wire * count)
      return gpu::resp_err_invalid_parameter;
    if (r->entry_count != 0)
      return gpu::resp_err_unspec; // already has backing

    const std::size_t bytes = std::size_t{count} * sizeof(backing_entry);
    auto block = alloc_.allocate(bytes, alignof(backing_entry));
    if (!block)
      return gpu::resp_err_out_of_memory;
    auto *entries = static_cast<backing_entry *>(block->ptr);
    const reloco::span<backing_entry> out(entries, count);

    std::uint64_t total = 0;
    reloco::array<std::byte, 16 * 16> chunk{};
    std::uint32_t done = 0;
    bool ok = true;
    while (done < count && ok) {
      const std::uint32_t n = std::min<std::uint32_t>(count - done, 16);
      if (!try_read_chain(mem, c.readable, fixed + entry_wire * done, reloco::span<std::byte>(chunk.data(), n * 16))) {
        ok = false;
        break;
      }
      for (std::uint32_t i = 0; i < n; ++i) {
        const u32span e(chunk.data() + std::size_t{i} * 16, 16);
        const std::uint64_t addr = load_le<std::uint64_t>(e);
        const std::uint32_t len = u32(e, 8);
        if (len > ~std::uint64_t{0} - addr) { // addr + len would wrap
          ok = false;
          break;
        }
        out[done + i] = backing_entry{addr, len};
        total += len;
      }
      done += n;
    }
    if (!ok) {
      alloc_.deallocate(block->ptr, block->size);
      return gpu::resp_err_invalid_parameter;
    }
    r->entries = entries;
    r->entries_alloc = block->size;
    r->entry_count = count;
    r->backing_bytes = total;
    return gpu::resp_ok_nodata;
  }

  std::uint32_t cmd_detach(u32span req) noexcept {
    resource *r = find(u32(req, 24));
    if (r == nullptr)
      return gpu::resp_err_invalid_resource_id;
    if (r->entries != nullptr)
      alloc_.deallocate(r->entries, r->entries_alloc);
    r->entries = nullptr;
    r->entries_alloc = 0;
    r->entry_count = 0;
    r->backing_bytes = 0;
    return gpu::resp_ok_nodata;
  }

  // Cursor commands have no response.
  template <typename Mem, typename Chain> std::uint32_t handle_cursor(Mem &mem, const Chain &c) noexcept {
    ++stats_.cursor_commands;
    if constexpr (gpu::detail::has_cursor<Display>::value) {
      constexpr std::size_t need = gpu::header_size + 32;
      if (c.readable_bytes < need || !try_read_chain(mem, c.readable, 0, reloco::span<std::byte>(cmd_.data(), need)))
        return 0;
      const u32span req(cmd_.data(), need);
      const std::uint32_t type = u32(req, 0);
      const std::uint32_t scanout = u32(req, 24);
      const std::uint32_t x = u32(req, 28);
      const std::uint32_t y = u32(req, 32);
      if (scanout >= Scanouts)
        return 0;
      if (type == gpu::cmd_move_cursor) {
        display_->cursor_move(scanout, x, y);
      } else if (type == gpu::cmd_update_cursor) {
        const std::uint32_t id = u32(req, 40);
        const std::uint32_t hot_x = u32(req, 44);
        const std::uint32_t hot_y = u32(req, 48);
        if (id == 0) {
          display_->cursor_update(scanout, nullptr, x, y, hot_x, hot_y);
        } else if (resource *r = find(id)) {
          const gpu::surface s = surface_of(*r);
          display_->cursor_update(scanout, &s, x, y, hot_x, hot_y);
        }
      }
    } else {
      (void)mem;
      (void)c;
    }
    return 0;
  }

  Display *display_;
  reloco::allocator_ref alloc_;
  gpu::limits limits_;
  std::uint32_t events_ = 0;
  std::uint64_t used_bytes_ = 0;
  gpu::stats stats_{};
  reloco::array<resource, MaxResources> res_{};
  reloco::array<scanout_binding, Scanouts> scan_{};
  reloco::array<chain_segment<GuestSpace>, queue_max_size> segs_{};
  reloco::array<std::byte, 64> cmd_{};
  reloco::array<std::byte, gpu::header_size + gpu::max_scanouts * 24> resp_{};
};

} // namespace structo::virtio
