// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file uart_pl011.hpp
 * @brief `structo::arch::arm::pl011_uart`: a basic driver for the ARM
 * PrimeCell PL011 UART's memory-mapped register window, adapted to
 * `structo::hw::uart_ref` via a `structo::hw::uart_traits<pl011_uart>`
 * specialization.
 *
 * `uart_ref.hpp` is deliberately chip-agnostic -- no register layout, no
 * clock-to-divisor math. This header supplies exactly that for one very
 * common chip: the PL011's 64-byte-aligned register window (data,
 * flags, integer/fractional baud-rate divisors, line control, control,
 * interrupt mask/status/clear), driven over a caller-supplied
 * `io_space_ref<device_io_space>` -- typically an `mmio_space_backend`
 * (see `mmio_space.hpp`) bound to wherever the PL011 was mapped (a QEMU
 * `virt` machine's `UART0` at `0x0900_0000`, a Raspberry Pi's `PL011`
 * at its SoC-specific peripheral base, a device-tree `reg` property, a
 * hypervisor's trapped-MMIO emulation, ...) -- so this driver stays
 * usable against any backend emulating the same register layout, not
 * just real hardware.
 *
 * Only polled, interrupt-free operation is implemented (every interrupt
 * source stays masked via `UARTIMSC = 0`), matching `uart_ref`'s own
 * documented scope.
 *
 * ## The input clock is caller-supplied, with no default
 *
 * Unlike `ns16550_uart` (a fixed `1.8432 MHz` input clock baked into the
 * real PC hardware), the PL011's `UARTCLK` input is an SoC-integration
 * detail with no universal standard value -- QEMU's `virt` machine
 * models `24 MHz`, real hardware varies widely by board and is often
 * only discoverable from a device tree's `clock-frequency`/`clocks`
 * property. `configure()` therefore takes the clock rate as an explicit
 * parameter rather than assuming one.
 *
 * @code
 * // window already mapped by the caller, e.g. from a device tree `reg`.
 * structo::mmio_space_backend backend(uart_window, 0x1000);
 * structo::io_space_ref<structo::device_io_space> regs(backend);
 * structo::arch::arm::pl011_uart uart_chip(regs);
 * structo::hw::uart_ref uart(uart_chip);
 * uart_chip.set_clock_hz(24'000'000); // QEMU virt's UARTCLK
 * (void)uart.configure(structo::hw::uart_config_115200_8n1);
 * (void)uart.write_string("hello from pl011\n");
 * @endcode
 *
 * ## Validation
 *
 * Like `vga_crtc.hpp`/`uart_ns16550.hpp`, this driver issues real
 * memory-mapped loads/stores (indirectly, through whatever
 * `io_space_ref` it is bound to) and so is validated only by
 * compilation -- standalone multi-standard compile plus the
 * public-header-check build target -- not by a dedicated unit test
 * against a fake backend.
 */

#include "../../hw/uart_ref.hpp"
#include "../../io_space_ref.hpp"

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch::arm {

/**
 * @brief A basic driver for a PL011-compatible UART's memory-mapped
 * register window, bound to a caller-supplied
 * `io_space_ref<device_io_space>`. Adapt to `structo::hw::uart_ref` via
 * the `uart_traits<pl011_uart>` specialization below.
 */
class pl011_uart {
public:
  /** @brief Binds this driver to @p regs and the register window
   * starting at @p base (a byte offset into @p regs's address space --
   * `0` for the common case of one `io_space_ref`/`mmio_space_backend`
   * dedicated to exactly this UART's own page). */
  explicit pl011_uart(structo::io_space_ref<structo::device_io_space> regs, std::uint64_t base = 0) noexcept
      : regs_(regs), base_(base) {}

  /** @brief Sets the `UARTCLK` input clock rate (Hz) used by subsequent
   * `configure()` calls' baud-rate divisor math. Must be called with
   * the correct value for the target board/platform before the first
   * `configure()` -- there is no sane chip-independent default (see the
   * @file-level docs above). */
  constexpr void set_clock_hz(std::uint32_t clock_hz) noexcept { clock_hz_ = clock_hz; }

  /** @brief The `UARTCLK` rate last set via `set_clock_hz`, `0` if never set. */
  [[nodiscard]] constexpr std::uint32_t clock_hz() const noexcept { return clock_hz_; }

  /** @brief Applies @p cfg: word length, parity, stop bits (packed into
   * `UARTLCR_H`) and the baud-rate integer/fractional divisor (`UARTIBRD`/
   * `UARTFBRD`, computed against `clock_hz()`), enables the chip's
   * FIFOs (`UARTLCR_H.FEN`), and masks every interrupt source
   * (`UARTIMSC = 0`, polled-only operation).
   *
   * Per the PL011 TRM, the UART is disabled (`UARTCR.UARTEN = 0`) before
   * touching the divisor/line-control registers -- changing them while
   * enabled can corrupt data already in flight -- then re-enabled
   * (`UARTEN | TXE | RXE`) last.
   *
   * @return `error::invalid_argument` if `clock_hz()` is still `0`
   * (never configured via `set_clock_hz`), @p cfg.baud_rate is zero, or
   * the resulting divisor overflows the 16-bit `UARTIBRD`.
   */
  [[nodiscard]] reloco::result<void> configure(const structo::hw::uart_config &cfg) noexcept {
    if (clock_hz_ == 0 || cfg.baud_rate == 0) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }

    // BAUDDIV = UARTCLK / (16 * baud); IBRD is BAUDDIV's integer part,
    // FBRD its fractional part scaled to 6 bits (* 64, rounded). Doing
    // the whole computation as one `* 4` (i.e. `* 64 / 16`) fixed-point
    // value before splitting avoids a separate, lossy rounding step.
    const std::uint64_t divisor = (static_cast<std::uint64_t>(clock_hz_) * 4) / cfg.baud_rate;
    const std::uint64_t ibrd = divisor >> 6;
    if (ibrd == 0 || ibrd > 0xFFFF) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    const std::uint32_t fbrd = static_cast<std::uint32_t>(divisor & 0x3F);

    const std::uint32_t lcr_h = line_control_word(cfg);

    // Disable before reconfiguring (TRM-recommended sequence).
    if (auto r = write_reg(reg_cr, 0); !r)
      return r;
    if (auto r = write_reg(reg_ibrd, static_cast<std::uint32_t>(ibrd)); !r)
      return r;
    if (auto r = write_reg(reg_fbrd, fbrd); !r)
      return r;
    // UARTLCR_H must be written last of the three -- a single write
    // strobe latches IBRD/FBRD/LCR_H together as one 30-bit register.
    if (auto r = write_reg(reg_lcr_h, lcr_h); !r)
      return r;
    // No interrupts enabled, matching uart_ref's polled-only scope.
    if (auto r = write_reg(reg_imsc, 0); !r)
      return r;
    // Re-enable: UARTEN | TXE | RXE.
    if (auto r = write_reg(reg_cr, cr_uarten | cr_txe | cr_rxe); !r)
      return r;

    cfg_ = cfg;
    return {};
  }

  /** @brief The settings last successfully applied via `configure()`. */
  [[nodiscard]] constexpr const structo::hw::uart_config &current_config() const noexcept { return cfg_; }

  /** @brief Whether the transmit FIFO is not full (`UARTFR.TXFF` clear). */
  [[nodiscard]] reloco::result<bool> tx_ready() noexcept {
    auto fr = read_reg(reg_fr);
    if (!fr)
      return reloco::unexpected(fr.error());
    return (*fr & fr_txff) == 0;
  }

  /** @brief Whether the receive FIFO has a byte waiting (`UARTFR.RXFE` clear). */
  [[nodiscard]] reloco::result<bool> rx_ready() noexcept {
    auto fr = read_reg(reg_fr);
    if (!fr)
      return reloco::unexpected(fr.error());
    return (*fr & fr_rxfe) == 0;
  }

  /** @brief Writes @p value to `UARTDR` if the transmit FIFO is
   * currently not full, otherwise fails with `error::try_again` without
   * blocking (matching `uart_traits::try_put_byte`'s documented
   * contract; `uart_ref::put_byte` is what actually spins on this). */
  [[nodiscard]] reloco::result<void> try_put_byte(std::uint8_t value) noexcept {
    auto ready = tx_ready();
    if (!ready)
      return reloco::unexpected(ready.error());
    if (!*ready)
      return reloco::unexpected(reloco::error::try_again);
    return write_reg(reg_dr, value);
  }

  /** @brief Reads one byte from `UARTDR` if one is waiting, otherwise
   * fails with `error::try_again` without blocking. The upper byte's
   * framing/parity/break/overrun error flags (`UARTDR[11:8]`) are
   * discarded; only the data byte itself is returned. */
  [[nodiscard]] reloco::result<std::uint8_t> try_get_byte() noexcept {
    auto ready = rx_ready();
    if (!ready)
      return reloco::unexpected(ready.error());
    if (!*ready)
      return reloco::unexpected(reloco::error::try_again);
    auto dr = read_reg(reg_dr);
    if (!dr)
      return reloco::unexpected(dr.error());
    return static_cast<std::uint8_t>(*dr & 0xFF);
  }

private:
  // Register byte offsets (ARM DDI 0183, PL011 TRM).
  static constexpr std::uint64_t reg_dr = 0x000;
  static constexpr std::uint64_t reg_fr = 0x018;
  static constexpr std::uint64_t reg_ibrd = 0x024;
  static constexpr std::uint64_t reg_fbrd = 0x028;
  static constexpr std::uint64_t reg_lcr_h = 0x02C;
  static constexpr std::uint64_t reg_cr = 0x030;
  static constexpr std::uint64_t reg_imsc = 0x038;

  // UARTFR bits.
  static constexpr std::uint32_t fr_txff = 1U << 5;
  static constexpr std::uint32_t fr_rxfe = 1U << 4;

  // UARTLCR_H bits.
  static constexpr std::uint32_t lcr_h_fen = 1U << 4;

  // UARTCR bits.
  static constexpr std::uint32_t cr_uarten = 1U << 0;
  static constexpr std::uint32_t cr_txe = 1U << 8;
  static constexpr std::uint32_t cr_rxe = 1U << 9;

  /** @brief Packs @p cfg's word length/parity/stop-bit settings into a
   * `UARTLCR_H` value, with `FEN` (FIFO enable) always set. */
  [[nodiscard]] static std::uint32_t line_control_word(const structo::hw::uart_config &cfg) noexcept {
    std::uint32_t lcr_h = lcr_h_fen;
    switch (cfg.data_bits) {
    case structo::hw::uart_data_bits::five:
      lcr_h |= 0b00 << 5;
      break;
    case structo::hw::uart_data_bits::six:
      lcr_h |= 0b01 << 5;
      break;
    case structo::hw::uart_data_bits::seven:
      lcr_h |= 0b10 << 5;
      break;
    case structo::hw::uart_data_bits::eight:
      lcr_h |= 0b11 << 5;
      break;
    }
    switch (cfg.parity) {
    case structo::hw::uart_parity::none:
      break;
    case structo::hw::uart_parity::odd:
      lcr_h |= 1U << 1; // PEN
      break;
    case structo::hw::uart_parity::even:
      lcr_h |= (1U << 1) | (1U << 2); // PEN | EPS
      break;
    case structo::hw::uart_parity::mark:
      lcr_h |= (1U << 1) | (1U << 7); // PEN | SPS (stuck odd)
      break;
    case structo::hw::uart_parity::space:
      lcr_h |= (1U << 1) | (1U << 2) | (1U << 7); // PEN | EPS | SPS (stuck even)
      break;
    }
    if (cfg.stop_bits != structo::hw::uart_stop_bits::one) {
      lcr_h |= 1U << 3; // STP2 (PL011 has no 1.5-stop-bit mode; treated as 2)
    }
    return lcr_h;
  }

  [[nodiscard]] reloco::result<void> write_reg(std::uint64_t offset, std::uint32_t value) noexcept {
    return regs_.write(structo::io_address<std::uint32_t, structo::device_io_space>{base_ + offset}, value);
  }
  [[nodiscard]] reloco::result<std::uint32_t> read_reg(std::uint64_t offset) noexcept {
    return regs_.read(structo::io_address<std::uint32_t, structo::device_io_space>{base_ + offset});
  }

  structo::io_space_ref<structo::device_io_space> regs_;
  std::uint64_t base_;
  std::uint32_t clock_hz_ = 0;
  structo::hw::uart_config cfg_{};
};

} // namespace structo::arch::arm

namespace structo::hw {

/** @brief Adapts `structo::arch::arm::pl011_uart` to `structo::hw::uart_ref`. */
template <> struct uart_traits<structo::arch::arm::pl011_uart> {
  using backend = structo::arch::arm::pl011_uart;

  static reloco::result<void> configure(backend &b, const uart_config &cfg) noexcept { return b.configure(cfg); }
  static reloco::result<uart_config> current_config(backend &b) noexcept { return b.current_config(); }
  static reloco::result<bool> tx_ready(backend &b) noexcept { return b.tx_ready(); }
  static reloco::result<bool> rx_ready(backend &b) noexcept { return b.rx_ready(); }
  static reloco::result<void> try_put_byte(backend &b, std::uint8_t v) noexcept { return b.try_put_byte(v); }
  static reloco::result<std::uint8_t> try_get_byte(backend &b) noexcept { return b.try_get_byte(); }
};

} // namespace structo::hw
