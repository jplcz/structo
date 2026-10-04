// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file vfp_context.hpp
 * @brief `structo::arch::arm::vfp_regs`: the AArch32 VFPv3-D32/NEON
 * extension register file -- `D0`-`D31` (32 double-precision
 * registers, 256 bytes) plus `FPSCR` -- as a hardware-register
 * save/restore policy usable standalone or composed into a full
 * `structo::arch::lazy_context<Traits, CpuId>` `Traits` type.
 *
 * `vfp_regs` only implements the register-level half of the `Traits`
 * contract (`state_type`, `is_enabled()`, `enable()`, `disable()`,
 * `save_context()`, `restore_context()`); it deliberately does not pick
 * a `MaxCpus`/`CpuId` or inherit `static_per_cpu_storage` itself, since
 * how many cores to size a residency table for is a whole-system
 * decision the register file has no business making. Compose it with
 * `static_per_cpu_storage` at the call site instead:
 *
 * @code
 * struct my_vfp_traits : structo::arch::arm::vfp_regs,
 *                        structo::arch::static_per_cpu_storage<8, void, std::size_t> {};
 * using vfp_switcher = structo::arch::lazy_context_switcher<my_vfp_traits>;
 * @endcode
 *
 * `FPEXC.EN` (bit 30) is the sole hardware latch gating every VFP/NEON
 * instruction: while clear, any attempt to execute one traps to the
 * "Undefined Instruction" vector instead of executing, regardless of
 * `D16`-`D31`'s architectural presence -- exactly the software-visible
 * signal `is_enabled()`/`enable()`/`disable()` need, with no separate
 * feature-detection step required (VFPv3-D32/NEON's full 32-register
 * file is assumed present). `D0`-`D15` and `D16`-`D31` are saved/
 * restored as two separate 16-register blocks because `VSTMIA`/`VLDMIA`
 * can only address 16 consecutive `D` registers per instruction.
 *
 * Only compiled on a real 32-bit ARM target (`__arm__` without
 * `__aarch64__`); a no-op everywhere else, matching the gating
 * convention established by `structo/arch/arm/irq_guard.hpp` and
 * `structo/arch/arm/hyp_vm_regs.hpp`.
 */

#include <cstdint>

#include <structo/arch/arm/sysregs_generated.hpp>

#if defined(__arm__) && !defined(__aarch64__)

namespace structo::arch::arm {

/**
 * @brief Hardware-register save/restore policy for the AArch32
 * VFPv3-D32/NEON register file. Implements the register-level half of
 * `structo::arch::lazy_context<Traits, CpuId>`'s `Traits` contract; see
 * this file's `@file` doc for how to compose it with
 * `structo::arch::static_per_cpu_storage` into a complete `Traits`.
 */
struct vfp_regs {
  /** @brief `D0`-`D31` (raw 64-bit lanes, no numeric interpretation) plus `FPSCR`. */
  struct state_type {
    std::uint64_t d[32]{};
    sysreg_raw::fpscr fpscr{};
  };

  [[nodiscard]] static bool is_enabled() noexcept { return sysreg_raw::fpexc::read().en(); }

  static void enable() noexcept {
    auto fpexc = sysreg_raw::fpexc::read();
    fpexc.set_en(true);
    fpexc.write();
  }

  static void disable() noexcept {
    auto fpexc = sysreg_raw::fpexc::read();
    fpexc.set_en(false);
    fpexc.write();
  }

  static void save_context(state_type &state) noexcept {
    std::uint64_t *dst = state.d;
    asm volatile("vstmia %0!, {d0-d15}\n\t"
                 "vstmia %0!, {d16-d31}"
                 : "+r"(dst)
                 :
                 : "memory");
    state.fpscr = sysreg_raw::fpscr::read();
  }

  static void restore_context(const state_type &state) noexcept {
    const std::uint64_t *src = state.d;
    asm volatile("vldmia %0!, {d0-d15}\n\t"
                 "vldmia %0!, {d16-d31}"
                 : "+r"(src)
                 :
                 : "memory");
    state.fpscr.write();
  }
};

} // namespace structo::arch::arm

#endif // defined(__arm__) && !defined(__aarch64__)
