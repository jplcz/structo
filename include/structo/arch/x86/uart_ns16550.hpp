// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file uart_ns16550.hpp
 * @brief `structo::arch::x86::ns16550_uart`: a basic driver for the
 * classic NS16550(A)-compatible UART found at the standard PC `COM1`-
 * `COM4` port addresses, adapted to `structo::hw::uart_ref` via a
 * `structo::hw::uart_traits<ns16550_uart>` specialization.
 *
 * `uart_ref.hpp` is deliberately chip-agnostic -- no register layout, no
 * clock-to-divisor math. This header supplies exactly that for one very
 * common chip: the 8-byte NS16550 register window (receive/transmit
 * buffer, interrupt enable, interrupt identification/FIFO control, line
 * control, modem control, line/modem status, scratch), driven over a
 * caller-supplied `io_space_ref<port_io_space>` exactly like `vga_crtc`
 * and `pic_8259` so it stays usable against any backend emulating the
 * same ports (e.g. a hypervisor's trapped-I/O emulation), not just the
 * real hardware.
 *
 * Only polled, interrupt-free operation is implemented (interrupts stay
 * masked via `IER = 0`), matching `uart_ref`'s own documented scope.
 * The divisor is computed against the chip's standard `1.8432 MHz`
 * input clock (`divisor = 115200 / baud_rate`); `configure()` rejects a
 * `baud_rate` that rounds to a zero or overflowing divisor.
 *
 * @code
 * structo::arch::x86::port_io_backend backend{};
 * structo::io_space_ref<structo::port_io_space> ports(backend);
 * structo::arch::x86::ns16550_uart uart_chip(ports, structo::arch::x86::ns16550_uart::com1_base);
 * structo::hw::uart_ref uart(uart_chip);
 * (void)uart.configure(structo::hw::uart_config_115200_8n1);
 * (void)uart.write_string("hello from ns16550\n");
 * @endcode
 *
 * ## Validation
 *
 * Like `vga_crtc.hpp`/`pic_8259.hpp`/`pit_8253.hpp`, this driver issues
 * real `IN`/`OUT` instructions (indirectly, through whatever
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

namespace structo::arch::x86 {

/**
 * @brief A basic driver for an NS16550(A)-compatible UART's 8-register
 * I/O window, bound to a caller-supplied `io_space_ref<port_io_space>`.
 * Adapt to `structo::hw::uart_ref` via the `uart_traits<ns16550_uart>`
 * specialization below.
 */
class ns16550_uart {
public:
  /** @brief The four standard PC COM port base addresses. */
  static constexpr std::uint16_t com1_base = 0x3F8;
  static constexpr std::uint16_t com2_base = 0x2F8;
  static constexpr std::uint16_t com3_base = 0x3E8;
  static constexpr std::uint16_t com4_base = 0x2E8;

  /** @brief The chip's standard input clock; `divisor = input_clock_hz
   * / (16 * baud_rate)`, i.e. `115200 / baud_rate` once the `/16`
   * sampling-clock factor is folded in. */
  static constexpr std::uint32_t input_clock_hz = 1843200;

  /** @brief Binds this driver to @p ports and the 8-register window
   * starting at @p base (one of the `comN_base` constants, or a custom
   * MMIO-mapped/passthrough base a hypervisor relocates the chip to). */
  explicit ns16550_uart(structo::io_space_ref<structo::port_io_space> ports, std::uint16_t base = com1_base) noexcept
      : ports_(ports), base_(base) {}

  /** @brief Applies @p cfg: word length, parity, stop bits (packed into
   * `LCR`) and the baud-rate divisor (written to `DLL`/`DLM` while
   * `LCR`'s `DLAB` bit is briefly set), then enables the chip's FIFOs
   * (`FCR`) and masks every interrupt source (`IER = 0`, polled-only
   * operation). Also raises `DTR`/`RTS` via `MCR` so a real serial
   * line's remote end sees the port as "ready", harmless/ignored under
   * emulation.
   * @return `error::invalid_argument` if @p cfg.baud_rate is zero or
   * would round to a zero or `>0xFFFF` divisor.
   */
  [[nodiscard]] reloco::result<void> configure(const structo::hw::uart_config &cfg) noexcept {
    if (cfg.baud_rate == 0) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    const std::uint32_t divisor = input_clock_hz / 16 / cfg.baud_rate;
    if (divisor == 0 || divisor > 0xFFFF) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }

    const std::uint8_t lcr = line_control_byte(cfg);

    // DLAB=1 to expose DLL/DLM at offsets 0/1 instead of RBR/THR/IER.
    if (auto r = write_reg(reg_lcr, static_cast<std::uint8_t>(lcr | dlab_bit)); !r)
      return r;
    if (auto r = write_reg(reg_dll, static_cast<std::uint8_t>(divisor & 0xFF)); !r)
      return r;
    if (auto r = write_reg(reg_dlm, static_cast<std::uint8_t>((divisor >> 8) & 0xFF)); !r)
      return r;
    // DLAB=0: back to the normal RBR/THR/IER register view.
    if (auto r = write_reg(reg_lcr, lcr); !r)
      return r;
    // Enable + clear the 14-byte FIFOs.
    if (auto r = write_reg(reg_fcr, 0xC7); !r)
      return r;
    // DTR | RTS, no loopback; no interrupts are enabled (IER left 0),
    // matching uart_ref's polled-only scope.
    if (auto r = write_reg(reg_mcr, 0x03); !r)
      return r;

    cfg_ = cfg;
    return {};
  }

  /** @brief The settings last successfully applied via `configure()`. */
  [[nodiscard]] constexpr const structo::hw::uart_config &current_config() const noexcept { return cfg_; }

  /** @brief Whether the transmit holding register is empty (`LSR` bit 5). */
  [[nodiscard]] reloco::result<bool> tx_ready() noexcept {
    auto lsr = read_reg(reg_lsr);
    if (!lsr)
      return reloco::unexpected(lsr.error());
    return (*lsr & 0x20) != 0;
  }

  /** @brief Whether a received byte is waiting (`LSR` bit 0). */
  [[nodiscard]] reloco::result<bool> rx_ready() noexcept {
    auto lsr = read_reg(reg_lsr);
    if (!lsr)
      return reloco::unexpected(lsr.error());
    return (*lsr & 0x01) != 0;
  }

  /** @brief Writes @p value to `THR` if the transmit holding register
   * is currently empty, otherwise fails with `error::try_again` without
   * blocking (matching `uart_traits::try_put_byte`'s documented
   * contract; `uart_ref::put_byte` is what actually spins on this). */
  [[nodiscard]] reloco::result<void> try_put_byte(std::uint8_t value) noexcept {
    auto ready = tx_ready();
    if (!ready)
      return reloco::unexpected(ready.error());
    if (!*ready)
      return reloco::unexpected(reloco::error::try_again);
    return write_reg(reg_rbr_thr, value);
  }

  /** @brief Reads one byte from `RBR` if one is waiting, otherwise fails
   * with `error::try_again` without blocking. */
  [[nodiscard]] reloco::result<std::uint8_t> try_get_byte() noexcept {
    auto ready = rx_ready();
    if (!ready)
      return reloco::unexpected(ready.error());
    if (!*ready)
      return reloco::unexpected(reloco::error::try_again);
    return read_reg(reg_rbr_thr);
  }

private:
  static constexpr std::uint8_t reg_rbr_thr = 0;
  static constexpr std::uint8_t reg_dll = 0;
  static constexpr std::uint8_t reg_ier = 1;
  static constexpr std::uint8_t reg_dlm = 1;
  static constexpr std::uint8_t reg_fcr = 2;
  static constexpr std::uint8_t reg_lcr = 3;
  static constexpr std::uint8_t reg_mcr = 4;
  static constexpr std::uint8_t reg_lsr = 5;
  static constexpr std::uint8_t dlab_bit = 0x80;

  /** @brief Packs @p cfg's word length/parity/stop-bit settings into an
   * `LCR` byte (DLAB left clear; the caller ORs it in while switching to
   * the divisor-latch view). */
  [[nodiscard]] static std::uint8_t line_control_byte(const structo::hw::uart_config &cfg) noexcept {
    std::uint8_t lcr = 0;
    switch (cfg.data_bits) {
    case structo::hw::uart_data_bits::five:
      lcr |= 0x00;
      break;
    case structo::hw::uart_data_bits::six:
      lcr |= 0x01;
      break;
    case structo::hw::uart_data_bits::seven:
      lcr |= 0x02;
      break;
    case structo::hw::uart_data_bits::eight:
      lcr |= 0x03;
      break;
    }
    switch (cfg.parity) {
    case structo::hw::uart_parity::none:
      break;
    case structo::hw::uart_parity::odd:
      lcr |= 0x08;
      break;
    case structo::hw::uart_parity::even:
      lcr |= 0x18;
      break;
    case structo::hw::uart_parity::mark:
      lcr |= 0x28;
      break;
    case structo::hw::uart_parity::space:
      lcr |= 0x38;
      break;
    }
    if (cfg.stop_bits != structo::hw::uart_stop_bits::one) {
      lcr |= 0x04;
    }
    return lcr;
  }

  [[nodiscard]] reloco::result<void> write_reg(std::uint8_t offset, std::uint8_t value) noexcept {
    return ports_.write(
        structo::io_address<std::uint8_t, structo::port_io_space>{static_cast<std::uint16_t>(base_ + offset)}, value);
  }
  [[nodiscard]] reloco::result<std::uint8_t> read_reg(std::uint8_t offset) noexcept {
    return ports_.read(
        structo::io_address<std::uint8_t, structo::port_io_space>{static_cast<std::uint16_t>(base_ + offset)});
  }

  structo::io_space_ref<structo::port_io_space> ports_;
  std::uint16_t base_;
  structo::hw::uart_config cfg_{};
};

} // namespace structo::arch::x86

namespace structo::hw {

/** @brief Adapts `structo::arch::x86::ns16550_uart` to `structo::hw::uart_ref`. */
template <> struct uart_traits<structo::arch::x86::ns16550_uart> {
  using backend = structo::arch::x86::ns16550_uart;

  static reloco::result<void> configure(backend &b, const uart_config &cfg) noexcept { return b.configure(cfg); }
  static reloco::result<uart_config> current_config(backend &b) noexcept { return b.current_config(); }
  static reloco::result<bool> tx_ready(backend &b) noexcept { return b.tx_ready(); }
  static reloco::result<bool> rx_ready(backend &b) noexcept { return b.rx_ready(); }
  static reloco::result<void> try_put_byte(backend &b, std::uint8_t v) noexcept { return b.try_put_byte(v); }
  static reloco::result<std::uint8_t> try_get_byte(backend &b) noexcept { return b.try_get_byte(); }
};

} // namespace structo::hw
