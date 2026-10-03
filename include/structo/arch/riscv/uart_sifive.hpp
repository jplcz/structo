// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file uart_sifive.hpp
 * @brief `structo::arch::riscv::sifive_uart`: a basic driver for the
 * SiFive UART IP block's memory-mapped register window (as found in
 * the FE310/FU540 SoCs and most SiFive-derived RISC-V platforms, e.g.
 * HiFive1/Unleashed/Unmatched and QEMU's `sifive_u`/`sifive_e`
 * machines), adapted to `structo::hw::uart_ref` via a
 * `structo::hw::uart_traits<sifive_uart>` specialization.
 *
 * `uart_ref.hpp` is deliberately chip-agnostic -- no register layout, no
 * clock-to-divisor math. This header supplies exactly that for one
 * RISC-V-ecosystem chip: the SiFive UART's compact 7-register window
 * (`txdata`/`rxdata`/`txctrl`/`rxctrl`/`ie`/`ip`/`div`), driven over a
 * caller-supplied `io_space_ref<device_io_space>` -- typically an
 * `mmio_space_backend` (see `mmio_space.hpp`) bound to wherever the UART
 * was mapped (a device-tree `reg` property, a hypervisor's
 * trapped-MMIO emulation, ...) -- so this driver stays usable against
 * any backend emulating the same register layout, not just real
 * hardware. Unlike `uart_ns16550.hpp`/`uart_pl011.hpp`, this is not an
 * x86/ARM chip pulled in incidentally -- it is specific to (and only
 * found on) RISC-V platforms, hence living under `arch/riscv/`.
 *
 * Only polled, interrupt-free operation is implemented (`ie` left `0`),
 * matching `uart_ref`'s own documented scope.
 *
 * ## Hardware is simpler -- and a little stranger -- than a 16550/PL011
 *
 * The SiFive UART IP has no parity support and no hardware flow
 * control at all (`uart_config::parity != none` or
 * `uart_config::flow_control != none` is rejected by `configure()` with
 * `error::invalid_argument` -- there is no register bit to even attempt
 * it), and always frames 8 data bits (`uart_config::data_bits != eight`
 * is likewise rejected). It also has no separate line-status register:
 * `txdata`/`rxdata` each double as both the data port *and* their own
 * readiness flag (`txdata` bit 31 = FIFO full, read-only and read-safe;
 * `rxdata` bit 31 = FIFO empty) -- but reading `rxdata` always pops the
 * receive FIFO if non-empty, data byte included, so there is no
 * non-destructive way to "peek" at `rxdata` in isolation. To still
 * satisfy `uart_traits`'s independent `rx_ready()`/`try_get_byte()`
 * contract without silently dropping a byte, `rx_ready()` caches
 * whatever it pops in that single read; a following `try_get_byte()`
 * returns the cached byte instead of issuing a second, FIFO-consuming
 * read.
 *
 * ## The input clock is caller-supplied, with no default
 *
 * Exactly as with `pl011_uart`, the UART's input clock is an
 * SoC-integration detail (the FE310's `tlclk`, the FU540's peripheral
 * clock, ...) with no universal value, so `configure()`'s baud-rate
 * divisor math is against an explicit, caller-set `set_clock_hz()`
 * rather than an assumed default.
 *
 * @code
 * structo::mmio_space_backend backend(uart_window, 0x1000);
 * structo::io_space_ref<structo::device_io_space> regs(backend);
 * structo::arch::riscv::sifive_uart uart_chip(regs);
 * structo::hw::uart_ref uart(uart_chip);
 * uart_chip.set_clock_hz(32'500'000); // e.g. HiFive1's tlclk
 * (void)uart.configure(structo::hw::uart_config_115200_8n1);
 * (void)uart.write_string("hello from sifive uart\n");
 * @endcode
 *
 * ## Validation
 *
 * Like `vga_crtc.hpp`/`uart_ns16550.hpp`/`uart_pl011.hpp`, this driver
 * issues real memory-mapped loads/stores (indirectly, through whatever
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

namespace structo::arch::riscv {

/**
 * @brief A basic driver for a SiFive UART IP block's memory-mapped
 * register window, bound to a caller-supplied
 * `io_space_ref<device_io_space>`. Adapt to `structo::hw::uart_ref` via
 * the `uart_traits<sifive_uart>` specialization below.
 */
class sifive_uart {
public:
  /** @brief Binds this driver to @p regs and the register window
   * starting at @p base (a byte offset into @p regs's address space --
   * `0` for the common case of one `io_space_ref`/`mmio_space_backend`
   * dedicated to exactly this UART's own page). */
  explicit sifive_uart(structo::io_space_ref<structo::device_io_space> regs, std::uint64_t base = 0) noexcept
      : regs_(regs), base_(base) {}

  /** @brief Sets the input clock rate (Hz) used by subsequent
   * `configure()` calls' baud-rate divisor math. Must be called with
   * the correct value for the target board/platform before the first
   * `configure()` -- there is no sane chip-independent default (see the
   * @file-level docs above). */
  constexpr void set_clock_hz(std::uint32_t clock_hz) noexcept { clock_hz_ = clock_hz; }

  /** @brief The input clock rate last set via `set_clock_hz`, `0` if never set. */
  [[nodiscard]] constexpr std::uint32_t clock_hz() const noexcept { return clock_hz_; }

  /** @brief Applies @p cfg's baud rate (`div = clock_hz() / baud_rate -
   * 1`) and stop-bit count (`txctrl.nstop`), then enables both the
   * transmitter and receiver (`txctrl.txen` / `rxctrl.rxen`) and masks
   * every interrupt source (`ie = 0`, polled-only operation).
   *
   * @return `error::invalid_argument` if `clock_hz()` is still `0`
   * (never configured via `set_clock_hz`), @p cfg.baud_rate is zero or
   * too high for the clock to represent (`div` would be `0`), or
   * @p cfg requests something this chip cannot do at all: a
   * `data_bits` other than `eight`, a `parity` other than `none`, or a
   * `flow_control` other than `none`.
   */
  [[nodiscard]] reloco::result<void> configure(const structo::hw::uart_config &cfg) noexcept {
    if (clock_hz_ == 0 || cfg.baud_rate == 0) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    if (cfg.data_bits != structo::hw::uart_data_bits::eight || cfg.parity != structo::hw::uart_parity::none ||
        cfg.flow_control != structo::hw::uart_flow_control::none) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }

    const std::uint32_t div = clock_hz_ / cfg.baud_rate;
    if (div == 0) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }

    if (auto r = write_reg(reg_div, div - 1); !r)
      return r;

    std::uint32_t txctrl = txctrl_txen;
    if (cfg.stop_bits != structo::hw::uart_stop_bits::one) {
      txctrl |= txctrl_nstop; // 2 stop bits; PL011 has no 1.5-stop-bit mode either.
    }
    if (auto r = write_reg(reg_txctrl, txctrl); !r)
      return r;
    if (auto r = write_reg(reg_rxctrl, rxctrl_rxen); !r)
      return r;
    // No interrupts enabled, matching uart_ref's polled-only scope.
    if (auto r = write_reg(reg_ie, 0); !r)
      return r;

    cfg_ = cfg;
    return {};
  }

  /** @brief The settings last successfully applied via `configure()`. */
  [[nodiscard]] constexpr const structo::hw::uart_config &current_config() const noexcept { return cfg_; }

  /** @brief Whether the transmit FIFO is not full (`txdata` bit 31 clear). */
  [[nodiscard]] reloco::result<bool> tx_ready() noexcept {
    auto txdata = read_reg(reg_txdata);
    if (!txdata)
      return reloco::unexpected(txdata.error());
    return (*txdata & txdata_full) == 0;
  }

  /** @brief Whether a received byte is available. See the @file-level
   * docs above: this destructively reads `rxdata` (the only way the
   * hardware exposes an "is a byte waiting" flag) and caches the
   * popped byte, if any, so a following `try_get_byte()` returns it
   * without a second, FIFO-consuming read. */
  [[nodiscard]] reloco::result<bool> rx_ready() noexcept {
    if (cached_rx_valid_) {
      return true;
    }
    auto rxdata = read_reg(reg_rxdata);
    if (!rxdata)
      return reloco::unexpected(rxdata.error());
    if ((*rxdata & rxdata_empty) != 0) {
      return false;
    }
    cached_rx_byte_ = static_cast<std::uint8_t>(*rxdata & 0xFF);
    cached_rx_valid_ = true;
    return true;
  }

  /** @brief Writes @p value to `txdata` if the transmit FIFO is
   * currently not full, otherwise fails with `error::try_again` without
   * blocking (matching `uart_traits::try_put_byte`'s documented
   * contract; `uart_ref::put_byte` is what actually spins on this). */
  [[nodiscard]] reloco::result<void> try_put_byte(std::uint8_t value) noexcept {
    auto ready = tx_ready();
    if (!ready)
      return reloco::unexpected(ready.error());
    if (!*ready)
      return reloco::unexpected(reloco::error::try_again);
    return write_reg(reg_txdata, value);
  }

  /** @brief Returns the byte `rx_ready()` already popped and cached, if
   * any; otherwise performs that same destructive `rxdata` read itself,
   * failing with `error::try_again` without blocking if the receive
   * FIFO is empty. */
  [[nodiscard]] reloco::result<std::uint8_t> try_get_byte() noexcept {
    if (cached_rx_valid_) {
      cached_rx_valid_ = false;
      return cached_rx_byte_;
    }
    auto rxdata = read_reg(reg_rxdata);
    if (!rxdata)
      return reloco::unexpected(rxdata.error());
    if ((*rxdata & rxdata_empty) != 0) {
      return reloco::unexpected(reloco::error::try_again);
    }
    return static_cast<std::uint8_t>(*rxdata & 0xFF);
  }

private:
  // Register byte offsets (SiFive FE310/FU540 UART IP).
  static constexpr std::uint64_t reg_txdata = 0x00;
  static constexpr std::uint64_t reg_rxdata = 0x04;
  static constexpr std::uint64_t reg_txctrl = 0x08;
  static constexpr std::uint64_t reg_rxctrl = 0x0C;
  static constexpr std::uint64_t reg_ie = 0x10;
  static constexpr std::uint64_t reg_div = 0x18;

  static constexpr std::uint32_t txdata_full = 1U << 31;
  static constexpr std::uint32_t rxdata_empty = 1U << 31;
  static constexpr std::uint32_t txctrl_txen = 1U << 0;
  static constexpr std::uint32_t txctrl_nstop = 1U << 1;
  static constexpr std::uint32_t rxctrl_rxen = 1U << 0;

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
  bool cached_rx_valid_ = false;
  std::uint8_t cached_rx_byte_ = 0;
};

} // namespace structo::arch::riscv

namespace structo::hw {

/** @brief Adapts `structo::arch::riscv::sifive_uart` to `structo::hw::uart_ref`. */
template <> struct uart_traits<structo::arch::riscv::sifive_uart> {
  using backend = structo::arch::riscv::sifive_uart;

  static reloco::result<void> configure(backend &b, const uart_config &cfg) noexcept { return b.configure(cfg); }
  static reloco::result<uart_config> current_config(backend &b) noexcept { return b.current_config(); }
  static reloco::result<bool> tx_ready(backend &b) noexcept { return b.tx_ready(); }
  static reloco::result<bool> rx_ready(backend &b) noexcept { return b.rx_ready(); }
  static reloco::result<void> try_put_byte(backend &b, std::uint8_t v) noexcept { return b.try_put_byte(v); }
  static reloco::result<std::uint8_t> try_get_byte(backend &b) noexcept { return b.try_get_byte(); }
};

} // namespace structo::hw
