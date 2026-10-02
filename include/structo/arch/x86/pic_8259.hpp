// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pic_8259.hpp
 * @brief `structo::arch::x86::pic_8259`: a basic driver for the legacy
 * dual-cascaded Intel 8259A Programmable Interrupt Controller, exposed
 * through `structo::hw::irqc_ref` by specializing
 * `structo::hw::irqc_traits<pic_8259>`.
 *
 * The 8259 is the original PC/AT interrupt controller: two cascaded
 * 8-line chips (a master wired to CPU `INTR` and a slave cascaded into
 * the master's IRQ 2 input) giving 16 flat IRQ lines, `0`-`15`. It has
 * no devicetree/MSI concept (so `irqc_ref::map_intr` only ever resolves
 * `irq_map_kind::gsi`), no CPU affinity (uniprocessor-only hardware --
 * `irqc_traits::assign_cpu` is deliberately not implemented), and no
 * TrustZone-style security domains or IPI (`assign_security_domain`/
 * `send_ipi` likewise not implemented) -- `irqc_ref` reports
 * `reloco::error::unsupported_operation` for all of those, exactly as
 * documented for an optional operation a backend omits.
 *
 * All 16 lines are edge-triggered and active-high by hardware
 * convention (there is no per-line trigger/polarity register on a
 * stock 8259 the way there is on an IOAPIC redirection table entry),
 * so this is a "self-contained" controller in `irqc_ref`'s own
 * terminology: only the optional `irqc_traits::post_filter` hook is
 * implemented (to issue the End-Of-Interrupt), not
 * `pre_ithread`/`post_ithread`.
 *
 * @code
 * structo::arch::x86::port_io_backend backend{};
 * structo::io_space_ref<structo::port_io_space> ports(backend);
 * structo::arch::x86::pic_8259 pic(ports);
 *
 * // Remap IRQ 0-7 to vectors 0x20-0x27 and IRQ 8-15 to 0x28-0x2F,
 * // away from the CPU's own reserved 0x00-0x1F exception vectors.
 * (void)pic.init(0x20, 0x28);
 *
 * structo::hw::irqc_ref irqc(pic);
 * auto timer_irq = irqc.map_intr(structo::hw::irq_map_data::from_gsi(0));
 * auto on_timer = [](structo::hw::irq_source &) { ++ticks; };
 * (void)irqc.setup_intr(timer_irq->get(), {}, on_timer);
 *
 * // From the real vector-0x20 trap entry, after dispatching:
 * (void)irqc.post_filter(timer_irq->get());
 * @endcode
 *
 * ## Validation
 *
 * Like `port_io_space.hpp` this backend issues real `IN`/`OUT`
 * instructions (indirectly, through whatever `structo::io_space_ref`
 * it is bound to) and so is not runtime-exercised by this repository's
 * own test suite from unprivileged userspace -- only header-compiled
 * cross-architecture (standalone compile plus the public-header-check
 * build target). A unit test driven against a fake `io_space_ref`
 * backend would only prove the fake behaves as scripted, not that the
 * real ICW/OCW sequencing against actual 8259 silicon is correct, so
 * deliberately no such test is provided here.
 */

#include "../../hw/irqc_ref.hpp"
#include "../../io_space_ref.hpp"

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace structo::arch::x86 {

/**
 * @brief A basic driver for the legacy dual-cascaded Intel 8259A PIC,
 * implementing `structo::hw::irqc_traits<pic_8259>` for its 16 flat
 * GSI lines `0`-`15`.
 *
 * Bound to a caller-supplied `io_space_ref<port_io_space>` (typically
 * wrapping `port_io_backend`, but any backend emulating the same four
 * 8259 ports works, e.g. a hypervisor's trapped-I/O emulation) rather
 * than hardcoding `port_io_backend` itself, so this driver stays
 * testable against a fake backend.
 */
class pic_8259 {
public:
  /** @brief The 8259 pair's fixed port assignments. */
  static constexpr std::uint16_t master_command_port = 0x20;
  static constexpr std::uint16_t master_data_port = 0x21;
  static constexpr std::uint16_t slave_command_port = 0xA0;
  static constexpr std::uint16_t slave_data_port = 0xA1;

  /** @brief The number of flat IRQ lines this pair manages: 8 per chip, cascaded into one. */
  static constexpr std::size_t num_lines = 16;

  /**
   * @brief Binds this driver to @p ports, the four master/slave
   * command/data ports above. Does not itself touch hardware --
   * call `init()` once before relying on any `irqc_traits` operation.
   */
  explicit pic_8259(structo::io_space_ref<structo::port_io_space> ports) noexcept
      : ports_(ports), lines_(make_lines(std::make_index_sequence<num_lines>{})) {}

  pic_8259(const pic_8259 &) = delete;
  pic_8259 &operator=(const pic_8259 &) = delete;
  pic_8259(pic_8259 &&) = delete;
  pic_8259 &operator=(pic_8259 &&) = delete;

  /**
   * @brief Runs the classic ICW1-ICW4 initialization sequence, remapping
   * IRQ 0-7 to vectors `[master_vector_base, master_vector_base + 8)`
   * and IRQ 8-15 to `[slave_vector_base, slave_vector_base + 8)`, then
   * masks every line (the caller unmasks each one it actually wants via
   * `irqc_ref::setup_intr`/`enable_intr`).
   *
   * Vector bases must be 8-aligned and must not collide with the CPU's
   * own reserved `0x00`-`0x1F` exception vectors; this is the caller's
   * responsibility to get right (the classic `0x20`/`0x28` remap avoids
   * both pitfalls and is the overwhelmingly common choice).
   */
  [[nodiscard]] reloco::result<void> init(std::uint8_t master_vector_base = 0x20,
                                          std::uint8_t slave_vector_base = 0x28) noexcept {
    // ICW1: start initialization, expect ICW4, edge-triggered, cascade mode.
    if (auto r = out8(master_command_port, 0x11); !r)
      return r;
    if (auto r = out8(slave_command_port, 0x11); !r)
      return r;
    // ICW2: vector base for each chip.
    if (auto r = out8(master_data_port, master_vector_base); !r)
      return r;
    if (auto r = out8(slave_data_port, slave_vector_base); !r)
      return r;
    // ICW3: master has a slave cascaded on its IRQ2 input (bit 2); slave's cascade identity is 2.
    if (auto r = out8(master_data_port, 0x04); !r)
      return r;
    if (auto r = out8(slave_data_port, 0x02); !r)
      return r;
    // ICW4: 8086/88 mode.
    if (auto r = out8(master_data_port, 0x01); !r)
      return r;
    if (auto r = out8(slave_data_port, 0x01); !r)
      return r;

    // Mask every line until the caller explicitly enables what it wants.
    mask_ = 0xFFFF;
    if (auto r = out8(master_data_port, static_cast<std::uint8_t>(mask_ & 0xFF)); !r)
      return r;
    if (auto r = out8(slave_data_port, static_cast<std::uint8_t>(mask_ >> 8)); !r)
      return r;

    initialized_ = true;
    for (auto &line : lines_)
      line.set_enabled(false);
    return {};
  }

  /** @brief Whether `init()` has run successfully. */
  [[nodiscard]] constexpr bool is_initialized() const noexcept { return initialized_; }

  /** @brief The `irq_source` for flat IRQ line @p irq (`< num_lines`). */
  [[nodiscard]] structo::hw::irq_source &line(std::size_t irq) noexcept { return lines_[irq]; }
  [[nodiscard]] const structo::hw::irq_source &line(std::size_t irq) const noexcept { return lines_[irq]; }

private:
  template <std::size_t... I>
  [[nodiscard]] static reloco::array<structo::hw::irq_source, sizeof...(I)>
  make_lines(std::index_sequence<I...>) noexcept {
    return {structo::hw::irq_source(static_cast<std::uint32_t>(I))...};
  }

  [[nodiscard]] reloco::result<void> out8(std::uint16_t port, std::uint8_t value) noexcept {
    return ports_.write(structo::io_address<std::uint8_t, structo::port_io_space>{port}, value);
  }

  /** @brief Applies the current `mask_` shadow to both chips' data (IMR) ports. */
  [[nodiscard]] reloco::result<void> apply_mask() noexcept {
    if (auto r = out8(master_data_port, static_cast<std::uint8_t>(mask_ & 0xFF)); !r)
      return r;
    return out8(slave_data_port, static_cast<std::uint8_t>(mask_ >> 8));
  }

  friend struct structo::hw::irqc_traits<pic_8259>;

  structo::io_space_ref<structo::port_io_space> ports_;
  reloco::array<structo::hw::irq_source, num_lines> lines_;
  std::uint16_t mask_ = 0xFFFF;
  bool initialized_ = false;
};

} // namespace structo::arch::x86

namespace structo::hw {

template <> struct irqc_traits<structo::arch::x86::pic_8259> {
  using pic_8259 = structo::arch::x86::pic_8259;

  [[nodiscard]] static reloco::result<void> enable_intr(pic_8259 &pic, irq_source &src) noexcept {
    if (!pic.initialized_)
      return reloco::unexpected(reloco::error::not_initialized);
    pic.mask_ &= static_cast<std::uint16_t>(~(std::uint16_t{1} << src.irq()));
    if (auto r = pic.apply_mask(); !r)
      return r;
    src.set_enabled(true);
    return {};
  }

  [[nodiscard]] static reloco::result<void> disable_intr(pic_8259 &pic, irq_source &src) noexcept {
    if (!pic.initialized_)
      return reloco::unexpected(reloco::error::not_initialized);
    pic.mask_ |= static_cast<std::uint16_t>(std::uint16_t{1} << src.irq());
    if (auto r = pic.apply_mask(); !r)
      return r;
    src.set_enabled(false);
    return {};
  }

  [[nodiscard]] static reloco::result<void> setup_intr(pic_8259 &pic, irq_source &src, irq_config cfg,
                                                       reloco::function_ref<void(irq_source &)> handler) noexcept {
    // Trigger/polarity is fixed in hardware (edge, active-high) on a stock 8259; `cfg`
    // is recorded purely for `irq_source::config()`'s own bookkeeping, same as any
    // other backend that cannot actually vary it.
    src.set_config(cfg);
    src.set_handler(handler);
    return enable_intr(pic, src);
  }

  [[nodiscard]] static reloco::result<void> teardown_intr(pic_8259 &pic, irq_source &src) noexcept {
    if (auto r = disable_intr(pic, src); !r)
      return r;
    src.clear_handler();
    return {};
  }

  [[nodiscard]] static reloco::result<std::reference_wrapper<irq_source>> map_intr(pic_8259 &pic,
                                                                                   const irq_map_data &data) noexcept {
    if (data.kind != irq_map_kind::gsi || data.gsi >= pic_8259::num_lines)
      return reloco::unexpected(reloco::error::not_found);
    return std::reference_wrapper<irq_source>(pic.line(data.gsi));
  }

  /**
   * @brief Edge-triggered lines are "self-contained" in `irqc_ref`'s own
   * terminology: no `pre_ithread`/`post_ithread` masking dance, just an
   * End-Of-Interrupt once the handler has run. Sends EOI to the slave
   * chip first when @p src is one of its 8 lines (IRQ 8-15), then always
   * to the master -- the master never learns a cascaded interrupt
   * happened otherwise.
   */
  [[nodiscard]] static reloco::result<void> post_filter(pic_8259 &pic, irq_source &src) noexcept {
    if (src.irq() >= 8) {
      if (auto r = pic.out8(pic_8259::slave_command_port, 0x20); !r)
        return r;
    }
    return pic.out8(pic_8259::master_command_port, 0x20);
  }

  [[nodiscard]] static reloco::result<irqc_capabilities> capabilities(pic_8259 &) noexcept {
    irqc_capabilities caps;
    caps.max_sources = pic_8259::num_lines;
    caps.supports_gsi_mapping = true;
    return caps;
  }
};

} // namespace structo::hw
