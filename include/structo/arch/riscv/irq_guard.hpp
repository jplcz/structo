// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file irq_guard.hpp
 * @brief `structo::arch::riscv::irq_traits`: a concrete
 * `structo::sync::irq_guard<Traits>` policy for RISC-V supervisor mode,
 * masking interrupts via `sstatus.SIE` (the Supervisor Interrupt
 * Enable bit).
 *
 * Only compiled on a real RISC-V target (`__riscv`); on every other
 * host this header is an intentional no-op so it stays header-check-
 * clean cross-compiled from any machine.
 *
 * A machine-mode (`mstatus.MIE`) equivalent is not provided here -- an
 * M-mode caller needing one can follow this file's pattern with
 * `csrrci`/`csrs` against `mstatus` instead of `sstatus`.
 */

#include <cstdint>

#if defined(__riscv)

namespace structo::arch::riscv {

/**
 * @brief `structo::sync::irq_guard<Traits>` policy for RISC-V
 * supervisor mode: saves/restores `sstatus`, clearing `SIE` on entry.
 */
struct irq_traits {
  using flags_type = unsigned long;

  [[nodiscard]] static flags_type hw_save_irqs() noexcept {
    unsigned long sstatus;
    asm volatile("csrrci %0, sstatus, 0x2" : "=r"(sstatus) : : "memory"); // clear SIE (bit 1), return prior value
    return sstatus;
  }

  static void hw_restore_irqs(flags_type sstatus) noexcept {
    asm volatile("csrs sstatus, %0" : : "r"(sstatus & 0x2ul) : "memory"); // restore SIE only if it was previously set
  }
};

} // namespace structo::arch::riscv

#endif // defined(__riscv)
