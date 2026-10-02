// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file irq_guard.hpp
 * @brief `structo::arch::arm::irq_traits`: a concrete
 * `structo::sync::irq_guard<Traits>` policy for 32-bit ARM (ARMv7-A and
 * earlier), masking interrupts via the CPSR `A`/`I`/`F` ("AIF") bits
 * rather than `I` alone -- disabling Imprecise Data Aborts and FIQ
 * alongside IRQ for the duration of the critical section, not just IRQ.
 *
 * Only compiled on a real 32-bit ARM target (`__arm__` without
 * `__aarch64__`); on every other host this header is an intentional
 * no-op so it stays header-check-clean cross-compiled from any machine.
 */

#include <cstdint>

#if defined(__arm__) && !defined(__aarch64__)

namespace structo::arch::arm {

/**
 * @brief `structo::sync::irq_guard<Traits>` policy for 32-bit ARM:
 * saves/restores the full CPSR, masking `A`/`I`/`F` (Imprecise Abort,
 * IRQ, FIQ) on entry via `cpsid aif`.
 */
struct irq_traits {
  using flags_type = std::uint32_t;

  [[nodiscard]] static flags_type hw_save_irqs() noexcept {
    std::uint32_t cpsr;
    asm volatile("mrs %0, cpsr" : "=r"(cpsr));
    asm volatile("cpsid aif" ::: "memory");
    return cpsr;
  }

  static void hw_restore_irqs(flags_type cpsr) noexcept { asm volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory"); }
};

} // namespace structo::arch::arm

#endif // defined(__arm__) && !defined(__aarch64__)
