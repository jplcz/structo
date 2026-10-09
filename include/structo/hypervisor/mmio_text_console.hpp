// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmio_text_console.hpp
 * @brief `structo::hypervisor::mmio_text_console`: an emulated VGA-like
 * text console for a guest, split exactly the way real VGA text mode
 * is: a plain-memory cell buffer (the "framebuffer") the guest writes
 * glyphs into directly, plus a tiny trapped MMIO register window (the
 * "virtual H/W control" space) for CRTC-style cursor/geometry control.
 *
 * ## Two address ranges, one device
 *
 * Real VGA text mode never traps a single character write: the
 * `0xB8000` text segment is a plain, directly-mapped memory window
 * (2 bytes/cell, see `hw::vga_text_console`), and only the *cursor*
 * is programmed through a separate, much smaller I/O port pair (CRTC
 * index/data, ports `0x3D4`/`0x3D5`). This class mirrors that split
 * for an emulated guest:
 *
 * - @ref framebuffer() returns the raw, allocator-owned cell buffer.
 *   The embedding hypervisor maps this span directly into the guest's
 *   physical address space (stage-2/EPT, out of this header's scope --
 *   see `mmio_device_ref.hpp`'s own docs for why address-space mapping
 *   is deliberately left to the caller) so every guest write lands
 *   straight in memory, no VM exit, no per-character trap.
 * - The `mmio_device_traits<mmio_text_console>` specialization below
 *   models the *separate*, much smaller "virtual CRTC" register window
 *   (@ref control_window_size bytes: `columns`/`rows` read-only, plus
 *   read-write `cursor_x`/`cursor_y`/`cursor_visible`) that *is* meant
 *   to be bound to an `mmio_device_ref` and trapped, exactly like real
 *   CRTC port I/O.
 *
 * Unlike `hw::vga_text_console` (which only wraps a caller-supplied,
 * externally-owned buffer), `mmio_text_console` owns its framebuffer:
 * it allocates it via a `reloco::allocator_ref` at construction and
 * frees it on destruction -- the shape `dynamic_bitmap.hpp` documents
 * as "store an `allocator_ref`, raw `allocate()`/`deallocate()` in a
 * private `try_allocate()`/destructor pair", here composed on top of
 * `vga_text_console` for the cell read/write logic itself rather than
 * reimplementing it.
 *
 * ## Also a `console_traits` backend: the hypervisor's own window
 *
 * Because the hypervisor itself may want to render directly onto this
 * same screen (early boot diagnostics printed before the guest OS has
 * taken over the console, a host-side debug overlay, ...), this class
 * also specializes `hw::console_traits`, so a `hw::console_ref` bound
 * to it works exactly like one bound to `vga_text_console` -- writing
 * through it updates the very same cell buffer the guest can read back
 * through its directly-mapped framebuffer window, and moving the
 * logical cursor updates the very same `cursor_x`/`cursor_y` the
 * guest's MMIO control window exposes: there is exactly one cursor,
 * shared by both the guest's "hardware" view and the hypervisor's own
 * `console_ref` view, precisely as real VGA hardware only has one.
 *
 * @code
 * auto maker = structo::hypervisor::mmio_text_console::try_create(80, 25);
 * if (!maker) { panic("out of memory sizing the virtual console"); }
 * auto screen = std::move(maker.value());
 *
 * // Guest-facing side: map `screen.framebuffer()` directly into guest
 * // physical memory, and bind the small control window to an exit handler:
 * structo::hypervisor::mmio_device_ref control(screen);
 * // control.size() == structo::hypervisor::mmio_text_console::control_window_size
 *
 * // Hypervisor-facing side: render straight onto the same screen.
 * structo::hw::console_ref console(screen);
 * console.write("booting guest...\n");
 * @endcode
 */

#include "mmio_device_ref.hpp"

#include <structo/hw/console_ref.hpp>
#include <structo/hw/vga_text_console.hpp>

#include <cstddef>
#include <cstdint>
#include <reloco/allocator.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <utility>

namespace structo::hypervisor {

/**
 * @brief Emulated VGA-like text console: an allocator-owned cell
 * buffer ("framebuffer") plus a small trapped "virtual CRTC" register
 * window. Move-only (owns the allocation). See the @file-level docs.
 */
class mmio_text_console {
public:
  /** @brief Virtual control-register offsets, each a 4-byte register. */
  static constexpr std::uint64_t control_off_columns = 0;
  static constexpr std::uint64_t control_off_rows = 4;
  static constexpr std::uint64_t control_off_cursor_x = 8;
  static constexpr std::uint64_t control_off_cursor_y = 12;
  static constexpr std::uint64_t control_off_cursor_visible = 16;
  /** @brief Size in bytes of the "virtual CRTC" register window -- what `mmio_device_ref` should be sized
   * to, entirely separate from @ref framebuffer()'s own (much larger) size. */
  static constexpr std::size_t control_window_size = 20;

  /** @brief The guest-visible "VM page size" the framebuffer allocation's base address and byte size are
   * aligned/rounded to, so the whole allocation maps cleanly into stage-2/EPT without exposing any
   * unrelated, unowned memory in a partial trailing page. 4 KiB, the smallest granule every mainstream
   * architecture (x86-64, ARM64, RISC-V) supports, picked as the one safe-by-default value a caller not
   * passing its own page size can rely on. */
  static constexpr std::size_t default_page_size = 4096;

  /** @brief Constructs an empty (`columns() == rows() == 0`), storage-less console bound to `alloc`. Never
   * fails. */
  constexpr explicit mmio_text_console(allocator_ref alloc = default_allocator()) noexcept : alloc_(alloc) {}

  mmio_text_console(mmio_text_console &&other) noexcept
      : alloc_(other.alloc_), owned_base_(other.owned_base_), owned_size_(other.owned_size_),
        text_(std::move(other.text_)), cursor_x_(other.cursor_x_), cursor_y_(other.cursor_y_),
        cursor_visible_(other.cursor_visible_) {
    other.owned_base_ = nullptr;
    other.owned_size_ = 0;
    other.text_ = hw::vga_text_console();
    other.cursor_x_ = 0;
    other.cursor_y_ = 0;
  }

  mmio_text_console &operator=(mmio_text_console &&other) noexcept {
    if (this != &other) {
      release();
      alloc_ = other.alloc_;
      owned_base_ = other.owned_base_;
      owned_size_ = other.owned_size_;
      text_ = std::move(other.text_);
      cursor_x_ = other.cursor_x_;
      cursor_y_ = other.cursor_y_;
      cursor_visible_ = other.cursor_visible_;
      other.owned_base_ = nullptr;
      other.owned_size_ = 0;
      other.text_ = hw::vga_text_console();
      other.cursor_x_ = 0;
      other.cursor_y_ = 0;
    }
    return *this;
  }

  // Owns a unique allocation; clone explicitly if ever needed, as `dynamic_bitmap::try_clone()` does.
  mmio_text_console(const mmio_text_console &) = delete;
  mmio_text_console &operator=(const mmio_text_console &) = delete;

  ~mmio_text_console() noexcept { release(); }

  /**
   * @brief Fallible allocation factory.
   * @param alloc Allocator backing the framebuffer's storage.
   * @param columns,rows Text-grid dimensions; the cell data needs `columns * rows * 2` bytes (2 bytes/cell,
   * matching `vga_text_console`'s own layout), but the actual allocation -- see @ref framebuffer() -- is
   * rounded up to a whole number of @p page_size bytes, so the entire owned block is safe to map into guest
   * physical memory with no partial, unrelated trailing page exposed.
   * @param page_size The VM page size to align the framebuffer allocation's base address and byte size to;
   * must be a power of two. Defaults to @ref default_page_size.
   * @return The console (all cells space, default colors, cursor at `(0, 0)` and visible; any page padding
   * past the cell data is zeroed), or `error::invalid_argument` if `columns`/`rows`/`page_size` are zero or
   * `page_size` is not a power of two, or `error::allocation_failed` if `alloc` could not provide the
   * backing storage.
   */
  [[nodiscard]] static result<mmio_text_console> try_allocate(allocator_ref alloc, std::size_t columns,
                                                              std::size_t rows,
                                                              std::size_t page_size = default_page_size) noexcept {
    if (columns == 0 || rows == 0 || page_size == 0 || (page_size & (page_size - 1)) != 0) {
      return unexpected(error::invalid_argument);
    }

    mmio_text_console console(alloc);
    const std::size_t cell_bytes = columns * rows * 2;
    const std::size_t aligned_bytes = (cell_bytes + page_size - 1) & ~(page_size - 1);

    // allocate() returns a block of at least aligned_bytes, which is only checked for failure here.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    auto res = alloc.allocate(aligned_bytes, page_size);
    RELOCO_END_UNSAFE_BUFFER_USAGE
    if (!res) {
      return unexpected(res.error());
    }

    // This console is the sole owner of `res->ptr`/`res->size`: both are recorded directly as its own
    // members below, never recovered indirectly through `text_` (a non-owning view over a sub-span of it) --
    // two classes each believing they might be responsible for freeing the same pointer is exactly the kind
    // of double-free/use-after-free hazard an owning type must not create for itself.
    console.owned_base_ = static_cast<std::byte *>(res->ptr);
    console.owned_size_ = res->size; // the allocator's own *absorbed* size, not `aligned_bytes` -- see
                                     // reloco/docs/allocator-capacity-absorption.md: `deallocate()` must be
                                     // called back with this exact size, not the originally requested one.
    for (std::byte &b : span<std::byte>(console.owned_base_, console.owned_size_)) {
      b = std::byte{0}; // no stale allocator memory leaks into the guest-mapped padding
    }

    auto text = hw::vga_text_console::try_create(span<std::byte>(console.owned_base_, cell_bytes), columns, rows);
    // `try_create` cannot fail here: `columns`/`rows` were already checked non-zero above, and the span
    // passed in is exactly `columns * rows * 2` bytes, well within `owned_size_`.
    console.text_ = std::move(*text);
    return console;
  }

  /** @brief `try_allocate()` using `reloco::default_allocator()`. */
  [[nodiscard]] static result<mmio_text_console> try_create(std::size_t columns, std::size_t rows,
                                                            std::size_t page_size = default_page_size) noexcept {
    return try_allocate(default_allocator(), columns, rows, page_size);
  }

  [[nodiscard]] std::size_t columns() const noexcept { return text_.columns(); }
  [[nodiscard]] std::size_t rows() const noexcept { return text_.rows(); }

  /** @brief The raw, allocator-owned, page-aligned/page-sized block backing the console -- the
   * "framebuffer" the embedding hypervisor should map directly into guest physical memory. Its size may
   * exceed `columns() * rows() * 2` by up to one page's worth of zeroed padding; only the leading
   * `columns() * rows() * 2` bytes are live cell data. See the @file-level docs' "Two address ranges, one
   * device". */
  [[nodiscard]] span<std::byte> framebuffer() const noexcept { return span<std::byte>(owned_base_, owned_size_); }

  [[nodiscard]] std::size_t cursor_x() const noexcept { return cursor_x_; }
  [[nodiscard]] std::size_t cursor_y() const noexcept { return cursor_y_; }
  [[nodiscard]] bool cursor_visible() const noexcept { return cursor_visible_; }

  /** @brief Writes `ch`/`fg`/`bg` at `(x, y)` directly, bypassing the logical cursor (what a guest's own
   * writes through the mapped @ref framebuffer() amount to). UB if out of bounds. */
  void put_cell(std::size_t x, std::size_t y, char ch, hw::console_color fg, hw::console_color bg) noexcept {
    text_.put_cell(x, y, ch, fg, bg);
  }
  /** @brief Reads the cell at `(x, y)`. UB if out of bounds. */
  [[nodiscard]] hw::console_cell get_cell(std::size_t x, std::size_t y) const noexcept { return text_.get_cell(x, y); }

  /** @brief Moves the one shared hardware cursor, clamping to the current grid -- the single piece of
   * state both the guest's MMIO control window and a hypervisor-bound `console_ref` read/write. */
  void move_cursor(std::size_t x, std::size_t y) noexcept {
    std::size_t cols = columns();
    std::size_t r = rows();
    cursor_x_ = (cols == 0) ? 0 : (x >= cols ? cols - 1 : x);
    cursor_y_ = (r == 0) ? 0 : (y >= r ? r - 1 : y);
  }
  void set_cursor_visible(bool visible) noexcept { cursor_visible_ = visible; }

private:
  // Frees exactly the block this console itself recorded as owned_base_/owned_size_ at allocation time --
  // never anything recovered from `text_` (see try_allocate()'s comment on why ownership is not shared
  // that way).
  void release() noexcept {
    if (owned_base_ != nullptr) {
      // owned_base_/owned_size_ are exactly the block returned by allocate().
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
      alloc_.deallocate(owned_base_, owned_size_);
      RELOCO_END_UNSAFE_BUFFER_USAGE
      owned_base_ = nullptr;
      owned_size_ = 0;
      text_ = hw::vga_text_console();
    }
  }

  allocator_ref alloc_;
  std::byte *owned_base_ = nullptr; // sole owner; freed only by release(), never derived from text_
  std::size_t owned_size_ = 0;      // the allocator's absorbed size -- what deallocate() must be called with
  hw::vga_text_console text_{};     // a non-owning *view* over a leading sub-span of owned_base_/owned_size_
  std::size_t cursor_x_ = 0;
  std::size_t cursor_y_ = 0;
  bool cursor_visible_ = true;
};

} // namespace structo::hypervisor

/** @brief Adapts @ref structo::hypervisor::mmio_text_console to `console_ref`: the hypervisor's own
 * rendering window onto the same cell buffer the guest's mapped framebuffer exposes. */
template <> struct structo::hw::console_traits<structo::hypervisor::mmio_text_console> {
  static void put_cell(structo::hypervisor::mmio_text_console &c, std::size_t x, std::size_t y, char ch,
                       console_color fg, console_color bg) noexcept {
    c.put_cell(x, y, ch, fg, bg);
  }
  static result<console_cell> get_cell(const structo::hypervisor::mmio_text_console &c, std::size_t x,
                                       std::size_t y) noexcept {
    return c.get_cell(x, y);
  }
  static std::size_t columns(const structo::hypervisor::mmio_text_console &c) noexcept { return c.columns(); }
  static std::size_t rows(const structo::hypervisor::mmio_text_console &c) noexcept { return c.rows(); }
  static void move_cursor(structo::hypervisor::mmio_text_console &c, std::size_t x, std::size_t y) noexcept {
    c.move_cursor(x, y);
  }
  static void set_cursor_visible(structo::hypervisor::mmio_text_console &c, bool visible) noexcept {
    c.set_cursor_visible(visible);
  }
};

namespace structo::hypervisor {

/** @brief The "virtual CRTC" register window: `columns`/`rows` read-only, `cursor_x`/`cursor_y`/
 * `cursor_visible` read-write. See the @file-level docs' "Two address ranges, one device". */
template <> struct mmio_device_traits<mmio_text_console> {
  static std::size_t size(mmio_text_console &) noexcept { return mmio_text_console::control_window_size; }

  static result<void> try_read(mmio_text_console &c, std::uint64_t offset, span<std::byte> dst) noexcept {
    std::uint32_t value;
    switch (offset) {
    case mmio_text_console::control_off_columns:
      value = static_cast<std::uint32_t>(c.columns());
      break;
    case mmio_text_console::control_off_rows:
      value = static_cast<std::uint32_t>(c.rows());
      break;
    case mmio_text_console::control_off_cursor_x:
      value = static_cast<std::uint32_t>(c.cursor_x());
      break;
    case mmio_text_console::control_off_cursor_y:
      value = static_cast<std::uint32_t>(c.cursor_y());
      break;
    case mmio_text_console::control_off_cursor_visible:
      value = c.cursor_visible() ? 1U : 0U;
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

  static result<void> try_write(mmio_text_console &c, std::uint64_t offset, span<const std::byte> src) noexcept {
    if (offset == mmio_text_console::control_off_columns || offset == mmio_text_console::control_off_rows) {
      return unexpected(error::permission_denied);
    }
    if (src.size() != sizeof(std::uint32_t)) {
      return unexpected(error::invalid_argument);
    }
    std::uint32_t value;
    // src.size() was checked equal to sizeof(value) above.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    __builtin_memcpy(&value, src.data(), sizeof(value));
    RELOCO_END_UNSAFE_BUFFER_USAGE
    switch (offset) {
    case mmio_text_console::control_off_cursor_x:
      c.move_cursor(value, c.cursor_y());
      return {};
    case mmio_text_console::control_off_cursor_y:
      c.move_cursor(c.cursor_x(), value);
      return {};
    case mmio_text_console::control_off_cursor_visible:
      c.set_cursor_visible(value != 0);
      return {};
    default:
      return unexpected(error::invalid_argument);
    }
  }
};

} // namespace structo::hypervisor
