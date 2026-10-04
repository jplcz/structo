// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fpsimd_context.hpp
 * @brief `structo::arch::arm64::fpsimd_regs`: the AArch64 FPSIMD
 * extension register file -- `V0`-`V31` (32 128-bit vector/scalar-FP
 * registers, 512 bytes) plus `FPSR`/`FPCR` -- as a hardware-register
 * save/restore policy usable standalone or composed into a full
 * `structo::arch::lazy_context<Traits, CpuId>` `Traits` type.
 *
 * `fpsimd_regs` only implements the register-level half of the
 * `Traits` contract (`state_type`, `is_enabled()`, `enable()`,
 * `disable()`, `save_context()`, `restore_context()`); it deliberately
 * does not pick a `MaxCpus`/`CpuId` or inherit `static_per_cpu_storage`
 * itself, since how many cores to size a residency table for is a
 * whole-system decision the register file has no business making.
 * Compose it with `static_per_cpu_storage` at the call site instead:
 *
 * @code
 * struct my_fpsimd_traits : structo::arch::arm64::fpsimd_regs,
 *                           structo::arch::static_per_cpu_storage<8, void, std::size_t> {};
 * using fpsimd_switcher = structo::arch::lazy_context_switcher<my_fpsimd_traits>;
 * @endcode
 *
 * `CPACR_EL1.FPEN` (bits 20-21) is the EL1-self-trap control gating
 * every FPSIMD instruction executed at EL0 or EL1: `0b11` grants both
 * exception levels unconditional access, while `0b00` traps any
 * attempt from either level to the "Advanced SIMD and floating-point"
 * exception, routed through the EL1 exception vector exactly like an
 * undefined instruction -- exactly the software-visible latch
 * `is_enabled()`/`enable()`/`disable()` need. `V0`-`V31` are saved/
 * restored as 16 `STP`/`LDP <Qn>, <Qn+1>` pairs, the widest transfer
 * granularity a single load/store-pair instruction can address.
 *
 * Only compiled on a real AArch64 target (`__aarch64__`); a no-op
 * everywhere else, matching the gating convention established by
 * `structo/arch/arm64/hyp_vm_regs.hpp`.
 */

#include <cstdint>

#include <structo/arch/arm64/sysregs_generated.hpp>

#if defined(__aarch64__)

namespace structo::arch::arm64 {

/**
 * @brief Hardware-register save/restore policy for the AArch64 FPSIMD
 * register file. Implements the register-level half of
 * `structo::arch::lazy_context<Traits, CpuId>`'s `Traits` contract; see
 * this file's `@file` doc for how to compose it with
 * `structo::arch::static_per_cpu_storage` into a complete `Traits`.
 */
struct fpsimd_regs {
  /** @brief One 128-bit vector register's raw storage (no numeric interpretation). */
  struct alignas(16) qreg {
    std::uint64_t lo{0};
    std::uint64_t hi{0};
  };

  /** @brief `V0`-`V31` plus `FPSR`/`FPCR`. */
  struct state_type {
    qreg v[32]{};
    sysreg_raw::fpsr fpsr{};
    sysreg_raw::fpcr fpcr{};
  };

  [[nodiscard]] static bool is_enabled() noexcept { return sysreg_raw::cpacr_el1::read().fpen() == 0b11U; }

  static void enable() noexcept {
    auto cpacr = sysreg_raw::cpacr_el1::read();
    cpacr.set_fpen(0b11U);
    cpacr.write();
  }

  static void disable() noexcept {
    auto cpacr = sysreg_raw::cpacr_el1::read();
    cpacr.set_fpen(0b00U);
    cpacr.write();
  }

  static void save_context(state_type &state) noexcept {
    void *base = state.v;
    asm volatile("stp q0, q1,   [%0, #16 * 0]\n\t"
                 "stp q2, q3,   [%0, #16 * 2]\n\t"
                 "stp q4, q5,   [%0, #16 * 4]\n\t"
                 "stp q6, q7,   [%0, #16 * 6]\n\t"
                 "stp q8, q9,   [%0, #16 * 8]\n\t"
                 "stp q10, q11, [%0, #16 * 10]\n\t"
                 "stp q12, q13, [%0, #16 * 12]\n\t"
                 "stp q14, q15, [%0, #16 * 14]\n\t"
                 "stp q16, q17, [%0, #16 * 16]\n\t"
                 "stp q18, q19, [%0, #16 * 18]\n\t"
                 "stp q20, q21, [%0, #16 * 20]\n\t"
                 "stp q22, q23, [%0, #16 * 22]\n\t"
                 "stp q24, q25, [%0, #16 * 24]\n\t"
                 "stp q26, q27, [%0, #16 * 26]\n\t"
                 "stp q28, q29, [%0, #16 * 28]\n\t"
                 "stp q30, q31, [%0, #16 * 30]"
                 :
                 : "r"(base)
                 : "memory");
    state.fpsr = sysreg_raw::fpsr::read();
    state.fpcr = sysreg_raw::fpcr::read();
  }

  static void restore_context(const state_type &state) noexcept {
    const void *base = state.v;
    asm volatile("ldp q0, q1,   [%0, #16 * 0]\n\t"
                 "ldp q2, q3,   [%0, #16 * 2]\n\t"
                 "ldp q4, q5,   [%0, #16 * 4]\n\t"
                 "ldp q6, q7,   [%0, #16 * 6]\n\t"
                 "ldp q8, q9,   [%0, #16 * 8]\n\t"
                 "ldp q10, q11, [%0, #16 * 10]\n\t"
                 "ldp q12, q13, [%0, #16 * 12]\n\t"
                 "ldp q14, q15, [%0, #16 * 14]\n\t"
                 "ldp q16, q17, [%0, #16 * 16]\n\t"
                 "ldp q18, q19, [%0, #16 * 18]\n\t"
                 "ldp q20, q21, [%0, #16 * 20]\n\t"
                 "ldp q22, q23, [%0, #16 * 22]\n\t"
                 "ldp q24, q25, [%0, #16 * 24]\n\t"
                 "ldp q26, q27, [%0, #16 * 26]\n\t"
                 "ldp q28, q29, [%0, #16 * 28]\n\t"
                 "ldp q30, q31, [%0, #16 * 30]"
                 :
                 : "r"(base)
                 : "memory");
    state.fpsr.write();
    state.fpcr.write();
  }
};

} // namespace structo::arch::arm64

#endif // defined(__aarch64__)
