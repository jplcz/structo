// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file irq_guard.hpp
 * @brief `structo::arch::arm64::irq_traits`: a concrete
 * `structo::sync::irq_guard<Traits>` policy for AArch64, masking
 * exceptions via the `DAIF` bits (Debug, SError/"A"bort, IRQ, FIQ) --
 * disabling all four exception classes for the duration of the critical
 * section, not just IRQ.
 *
 * Only compiled on a real AArch64 target (`__aarch64__`); on every
 * other host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine.
 */

#include <cstdint>

#if defined(__aarch64__)

namespace structo::arch::arm64 {

/**
 * @brief `structo::sync::irq_guard<Traits>` policy for AArch64: saves/
 * restores the full `DAIF` register, masking all four exception classes
 * (Debug, SError, IRQ, FIQ) on entry via `msr daifset, #0xf`.
 */
struct irq_traits {
  using flags_type = std::uint64_t;

  [[nodiscard]] static flags_type hw_save_irqs() noexcept {
    std::uint64_t daif;
    asm volatile("mrs %0, daif" : "=r"(daif));
    asm volatile("msr daifset, #0xf" ::: "memory");
    return daif;
  }

  static void hw_restore_irqs(flags_type daif) noexcept { asm volatile("msr daif, %0" : : "r"(daif) : "memory"); }
};

} // namespace structo::arch::arm64

#endif // defined(__aarch64__)
