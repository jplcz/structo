// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vga_crtc.hpp
 * @brief `structo::arch::x86::vga_crtc`: a basic driver for the real
 * VGA CRT Controller's index/data register pair (ports `0x3D4`/`0x3D5`
 * for color/CGA-compatible mode, `0x3B4`/`0x3B5` for monochrome), used
 * here only to drive the hardware text-mode cursor.
 *
 * `structo::hw::vga_text_console` (see `hw/vga_text_console.hpp`)
 * deliberately has no I/O port dependency of its own: it only forwards
 * cursor moves to an optional, caller-supplied
 * `vga_text_console::cursor_sink` callback. This header is that
 * callback's real-hardware implementation -- it writes the cursor's
 * linear cell offset (`y * columns + x`) into CRTC registers `0x0E`
 * (cursor location high byte) and `0x0F` (cursor location low byte),
 * and optionally shapes/enables the cursor via register `0x0A` (cursor
 * start / cursor-disable bit 5) and `0x0B` (cursor end).
 *
 * @code
 * structo::arch::x86::port_io_backend backend{};
 * structo::io_space_ref<structo::port_io_space> ports(backend);
 * structo::arch::x86::vga_crtc crtc(ports);
 * (void)crtc.enable_cursor(14, 15); // underline-style cursor shape
 *
 * std::byte vga_memory[80 * 25 * 2];
 * auto console = structo::hw::vga_text_console::try_create(
 *     reloco::span<std::byte>(vga_memory, sizeof(vga_memory)), 80, 25,
 *     structo::hw::vga_text_console::cursor_sink(
 *         [&crtc](std::size_t x, std::size_t y) { (void)crtc.move_cursor(x, y, 80); }));
 * @endcode
 *
 * ## Validation
 *
 * Like `pic_8259.hpp` and `port_io_space.hpp`, this driver issues real
 * `OUT` instructions (indirectly, through whatever `io_space_ref` it is
 * bound to) and so is validated only by compilation -- standalone
 * multi-standard compile plus the public-header-check build target --
 * not by a dedicated unit test against a fake backend, which would only
 * prove the fake behaves as scripted rather than that the real CRTC
 * register sequencing is correct.
 */

#include "../../io_space_ref.hpp"

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch::x86 {

/**
 * @brief A basic driver for the VGA CRT Controller's index/data
 * register pair, used here to position (and optionally shape/enable)
 * the hardware text-mode cursor.
 *
 * Bound to a caller-supplied `io_space_ref<port_io_space>` rather than
 * hardcoding `port_io_backend` itself, so this driver stays usable
 * against any backend emulating the same two ports (e.g. a
 * hypervisor's trapped-I/O emulation), matching `pic_8259`'s approach.
 */
class vga_crtc {
public:
  /** @brief Standard color/CGA-compatible CRTC port pair. */
  static constexpr std::uint16_t color_index_port = 0x3D4;
  static constexpr std::uint16_t color_data_port = 0x3D5;
  /** @brief Monochrome-adapter CRTC port pair. */
  static constexpr std::uint16_t mono_index_port = 0x3B4;
  static constexpr std::uint16_t mono_data_port = 0x3B5;

  /** @brief CRTC register indices this driver touches. */
  static constexpr std::uint8_t reg_cursor_start = 0x0A;
  static constexpr std::uint8_t reg_cursor_end = 0x0B;
  static constexpr std::uint8_t reg_cursor_location_high = 0x0E;
  static constexpr std::uint8_t reg_cursor_location_low = 0x0F;
  /** @brief Bit 5 of `reg_cursor_start`: set to disable the cursor entirely. */
  static constexpr std::uint8_t cursor_disable_bit = 0x20;

  /**
   * @brief Binds this driver to @p ports and the given index/data
   * port pair (defaults to the color/CGA-compatible pair; pass
   * `mono_index_port`/`mono_data_port` for a monochrome adapter).
   */
  explicit vga_crtc(structo::io_space_ref<structo::port_io_space> ports, std::uint16_t index_port = color_index_port,
                    std::uint16_t data_port = color_data_port) noexcept
      : ports_(ports), index_port_(index_port), data_port_(data_port) {}

  /**
   * @brief Moves the hardware text-mode cursor to cell `(x, y)` of a
   * text buffer that is @p columns cells wide, by writing its linear
   * offset `y * columns + x` into `reg_cursor_location_high`/`_low`.
   * Matches `structo::hw::vga_text_console::cursor_sink`'s `(x, y)`
   * signature when @p columns is bound ahead of time (e.g. via a
   * lambda capture), so it can be passed directly as that callback.
   */
  [[nodiscard]] reloco::result<void> move_cursor(std::size_t x, std::size_t y, std::size_t columns) noexcept {
    std::uint16_t offset = static_cast<std::uint16_t>(y * columns + x);
    if (auto r = write_reg(reg_cursor_location_high, static_cast<std::uint8_t>(offset >> 8)); !r)
      return r;
    return write_reg(reg_cursor_location_low, static_cast<std::uint8_t>(offset & 0xFF));
  }

  /**
   * @brief Enables the hardware cursor (clears `cursor_disable_bit`)
   * and shapes it as the scan-line range `[start, end]` out of the
   * glyph's 8/16 scan lines (e.g. `(14, 15)` for a thin underline
   * cursor, `(0, 15)` for a full-height block cursor).
   */
  [[nodiscard]] reloco::result<void> enable_cursor(std::uint8_t start, std::uint8_t end) noexcept {
    if (auto r = write_reg(reg_cursor_start, static_cast<std::uint8_t>(start & 0x1F)); !r)
      return r;
    return write_reg(reg_cursor_end, static_cast<std::uint8_t>(end & 0x1F));
  }

  /** @brief Disables the hardware cursor entirely (sets `cursor_disable_bit`). */
  [[nodiscard]] reloco::result<void> disable_cursor() noexcept {
    return write_reg(reg_cursor_start, cursor_disable_bit);
  }

private:
  [[nodiscard]] reloco::result<void> write_reg(std::uint8_t index, std::uint8_t value) noexcept {
    if (auto r = ports_.write(structo::io_address<std::uint8_t, structo::port_io_space>{index_port_}, index); !r)
      return r;
    return ports_.write(structo::io_address<std::uint8_t, structo::port_io_space>{data_port_}, value);
  }

  structo::io_space_ref<structo::port_io_space> ports_;
  std::uint16_t index_port_;
  std::uint16_t data_port_;
};

} // namespace structo::arch::x86
