// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmio_print_device.hpp
 * @brief `structo::hypervisor::mmio_print_device`: a complete, concrete
 * `mmio_device_traits` backend -- a one-byte, write-only "print port"
 * MMIO device that forwards every byte the guest writes to it straight
 * to an underlying `structo::hw::console_ref` sink, one `put()` call per
 * write.
 *
 * This is the worked example `mmio_device_ref.hpp`'s own `@file`-level
 * docs point to, promoted to a real, reusable header: exactly the kind
 * of trivial "debug output port" real hypervisors commonly expose to an
 * early guest boot stage, long before a full UART/virtio-console model
 * exists -- modeled on the long-standing Bochs/QEMU `0xE9` debug port
 * hack, just MMIO instead of port I/O. A guest `strb`/`sb`/`mov byte`
 * instruction targeting this device's one mapped byte prints one
 * character; nothing more is modeled (no FIFO, no baud rate, no
 * interrupts).
 *
 * ## Why `console_ref`, not `uart_ref`, as the sink
 *
 * `console_ref::put(char)` already does exactly the "friendly text
 * stream" translation a printed byte stream needs (`'\n'` performs a
 * carriage-return + line-feed, `'\t'` advances to the next tab stop,
 * ...) and is `noexcept`/never-fails (a `put` on an unbound `console_ref`
 * silently does nothing) -- so `try_write` below needs no error
 * handling of its own at all. `hw::uart_ref` would work too (most real
 * debug ports *are* a UART), but would require spinning on `tx_ready`/
 * handling `error::timed_out` for every single guest byte; `console_ref`
 * is the simpler, more apt sink for a port whose whole point is "print
 * this character somewhere a human or test harness can see it".
 *
 * ## Read side: a Bochs-style self-identification probe
 *
 * Reading this device's one byte always returns `probe_magic` (`0xE9`,
 * the same value Bochs's own debug port echoes back) regardless of
 * what was last written -- purely so guest code can distinguish "this
 * address is mapped and really is a print port" from "nothing is
 * mapped here" before blindly writing to it. There is no actual
 * register state to read back.
 *
 * @code
 * structo::hw::vga_text_console screen; // or any other console_traits-adapted backend
 * structo::hw::console_ref console(screen);
 * structo::hypervisor::mmio_print_device print_port(console);
 * structo::hypervisor::mmio_device_ref dev(print_port);
 *
 * // In the VM-exit MMIO handler, once (device, relative offset) has been resolved to print_port's
 * // single byte at offset 0:
 * const std::byte guest_byte = std::byte{'H'};
 * (void)dev.try_write(0, reloco::span<const std::byte>(&guest_byte, 1)); // prints 'H' to `screen`
 * @endcode
 */

#include "mmio_device_ref.hpp"

#include <structo/hw/console_ref.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::hypervisor {

/**
 * @brief A one-byte, write-only emulated "print port" MMIO device:
 * every byte written to its single mapped byte is forwarded to a bound
 * `hw::console_ref` sink via `put()`. See the @file-level docs above
 * for the full rationale and a worked example.
 */
class mmio_print_device {
public:
  /** @brief Fixed readback value (matching Bochs's own `0xE9` debug port), purely a presence probe. */
  static inline constexpr std::byte probe_magic{0xE9};

  /**
   * @brief Binds this device to @p sink; every future guest write prints to it.
   * @param sink May itself be unbound -- `console_ref::put()` silently no-ops, so an unbound sink simply
   * discards every byte written, rather than this device failing outright.
   */
  constexpr explicit mmio_print_device(hw::console_ref sink) noexcept : sink_(sink) {}

  /** @brief The bound sink every write is forwarded to. */
  [[nodiscard]] constexpr hw::console_ref sink() const noexcept { return sink_; }

private:
  hw::console_ref sink_;

  friend struct mmio_device_traits<mmio_print_device>;
};

template <> struct mmio_device_traits<mmio_print_device> {
  /** @brief Exactly one byte: this device models a single print-port register, nothing more. */
  static std::size_t size(mmio_print_device &) noexcept { return 1; }

  /** @brief Always reports @ref mmio_print_device::probe_magic -- see the @file-level docs' "Read side". */
  static result<void> try_read(mmio_print_device &, std::uint64_t, span<std::byte> dst) noexcept {
    dst[0] = mmio_print_device::probe_magic;
    return {};
  }

  /** @brief Forwards the single written byte to the bound sink as one character. Never fails. */
  static result<void> try_write(mmio_print_device &d, std::uint64_t, span<const std::byte> src) noexcept {
    d.sink_.put(static_cast<char>(src[0]));
    return {};
  }

  /** @brief Reflects whether the bound sink is itself actually bound. */
  static bool is_available(mmio_print_device &d) noexcept { return static_cast<bool>(d.sink_); }
};

} // namespace structo::hypervisor
