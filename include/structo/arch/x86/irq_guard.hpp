// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file irq_guard.hpp
 * @brief `structo::arch::x86::irq_traits`: a concrete
 * `structo::sync::irq_guard<Traits>` policy for x86/x86-64, masking
 * interrupts via `RFLAGS.IF` (the Interrupt Flag) with `pushf`/`cli`/
 * `popf`.
 *
 * Only compiled on a real x86/x86-64 target (`__i386__`/`__x86_64__`);
 * on every other host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine.
 */

#include <cstdint>

#if defined(__i386__) || defined(__x86_64__)

namespace structo::arch::x86 {

/**
 * @brief `structo::sync::irq_guard<Traits>` policy for x86/x86-64:
 * saves/restores the full flags register, clearing `IF` on entry via
 * `cli`.
 */
struct irq_traits {
#if defined(__x86_64__)
  using flags_type = std::uint64_t;
#else
  using flags_type = std::uint32_t;
#endif

  [[nodiscard]] static flags_type hw_save_irqs() noexcept {
    flags_type flags;
#if defined(__x86_64__)
    asm volatile("pushfq\n\t"
                 "popq %0\n\t"
                 "cli"
                 : "=r"(flags)
                 :
                 : "memory");
#else
    asm volatile("pushfl\n\t"
                 "popl %0\n\t"
                 "cli"
                 : "=r"(flags)
                 :
                 : "memory");
#endif
    return flags;
  }

  static void hw_restore_irqs(flags_type flags) noexcept {
#if defined(__x86_64__)
    asm volatile("pushq %0\n\t"
                 "popfq"
                 :
                 : "r"(flags)
                 : "memory", "cc");
#else
    asm volatile("pushl %0\n\t"
                 "popfl"
                 :
                 : "r"(flags)
                 : "memory", "cc");
#endif
  }
};

} // namespace structo::arch::x86

#endif // defined(__i386__) || defined(__x86_64__)
