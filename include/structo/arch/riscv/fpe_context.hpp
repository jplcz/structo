// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fpe_context.hpp
 * @brief `structo::arch::riscv::fpe_regs`: the RISC-V `F`/`D` extension
 * register file -- `F0`-`F31` (32 64-bit floating-point registers,
 * assuming the `D` extension per this repository's RV64 scope) plus
 * `FCSR` -- as a hardware-register save/restore policy usable
 * standalone or composed into a full
 * `structo::arch::lazy_context<Traits, CpuId>` `Traits` type.
 *
 * `fpe_regs` only implements the register-level half of the `Traits`
 * contract (`state_type`, `is_enabled()`, `enable()`, `disable()`,
 * `save_context()`, `restore_context()`); it deliberately does not pick
 * a `MaxCpus`/`CpuId` or inherit `static_per_cpu_storage` itself, since
 * how many cores to size a residency table for is a whole-system
 * decision the register file has no business making. Compose it with
 * `static_per_cpu_storage` at the call site instead:
 *
 * @code
 * struct my_fpe_traits : structo::arch::riscv::fpe_regs,
 *                        structo::arch::static_per_cpu_storage<8, void, std::size_t> {};
 * using fpe_switcher = structo::arch::lazy_context_switcher<my_fpe_traits>;
 * @endcode
 *
 * `sstatus.FS` (bits 13-14, a 2-bit field: `0`=Off, `1`=Initial,
 * `2`=Clean, `3`=Dirty) is the software-visible latch gating every
 * `F`/`D`-extension instruction: while `Off`, any attempt to execute
 * one traps as an illegal instruction -- exactly the signal
 * `is_enabled()`/`enable()`/`disable()` need. `enable()` sets `Dirty`
 * since real register content is about to be loaded, not reset state;
 * `disable()` sets `Off`. Unlike ARM's `VSTMIA`/`VLDMIA` or AArch64's
 * `STP`/`LDP`, RISC-V has no load/store-multiple for floating-point
 * registers, so `F0`-`F31` are saved/restored one register at a time
 * via `FSD`/`FLD`. `FCSR` is accessed via the `CSRR`/`CSRW` encoding of
 * the `frcsr`/`fscsr` pseudo-instructions (CSR address `0x003`).
 *
 * RV64 only (matching the rest of this repository's RISC-V scope);
 * only compiled on a real RISC-V target (`__riscv`), a no-op
 * everywhere else, matching the gating convention established by
 * `structo/arch/riscv/hyp_vm_regs.hpp`.
 */

#include <cstdint>

#include <structo/arch/riscv/sysregs_generated.hpp>

#if defined(__riscv)

namespace structo::arch::riscv {

/**
 * @brief Hardware-register save/restore policy for the RISC-V `F`/`D`
 * extension register file. Implements the register-level half of
 * `structo::arch::lazy_context<Traits, CpuId>`'s `Traits` contract; see
 * this file's `@file` doc for how to compose it with
 * `structo::arch::static_per_cpu_storage` into a complete `Traits`.
 */
struct fpe_regs {
  /** @brief `F0`-`F31` (raw 64-bit lanes, no numeric interpretation) plus `FCSR`. */
  struct state_type {
    std::uint64_t f[32]{};
    sysreg_raw::fcsr fcsr{};
  };

  [[nodiscard]] static bool is_enabled() noexcept { return sysreg_raw::sstatus::read().fs() != 0b00U; }

  static void enable() noexcept {
    auto sstatus = sysreg_raw::sstatus::read();
    sstatus.set_fs(0b11U);
    sstatus.write();
  }

  static void disable() noexcept {
    auto sstatus = sysreg_raw::sstatus::read();
    sstatus.set_fs(0b00U);
    sstatus.write();
  }

  static void save_context(state_type &state) noexcept {
    // clang-format off
    asm volatile("fsd f0,  0*8(%0)\n\t"
                 "fsd f1,  1*8(%0)\n\t"
                 "fsd f2,  2*8(%0)\n\t"
                 "fsd f3,  3*8(%0)\n\t"
                 "fsd f4,  4*8(%0)\n\t"
                 "fsd f5,  5*8(%0)\n\t"
                 "fsd f6,  6*8(%0)\n\t"
                 "fsd f7,  7*8(%0)\n\t"
                 "fsd f8,  8*8(%0)\n\t"
                 "fsd f9,  9*8(%0)\n\t"
                 "fsd f10, 10*8(%0)\n\t"
                 "fsd f11, 11*8(%0)\n\t"
                 "fsd f12, 12*8(%0)\n\t"
                 "fsd f13, 13*8(%0)\n\t"
                 "fsd f14, 14*8(%0)\n\t"
                 "fsd f15, 15*8(%0)\n\t"
                 "fsd f16, 16*8(%0)\n\t"
                 "fsd f17, 17*8(%0)\n\t"
                 "fsd f18, 18*8(%0)\n\t"
                 "fsd f19, 19*8(%0)\n\t"
                 "fsd f20, 20*8(%0)\n\t"
                 "fsd f21, 21*8(%0)\n\t"
                 "fsd f22, 22*8(%0)\n\t"
                 "fsd f23, 23*8(%0)\n\t"
                 "fsd f24, 24*8(%0)\n\t"
                 "fsd f25, 25*8(%0)\n\t"
                 "fsd f26, 26*8(%0)\n\t"
                 "fsd f27, 27*8(%0)\n\t"
                 "fsd f28, 28*8(%0)\n\t"
                 "fsd f29, 29*8(%0)\n\t"
                 "fsd f30, 30*8(%0)\n\t"
                 "fsd f31, 31*8(%0)"
                 :
                 : "r"(state.f)
                 : "memory");
    // clang-format on
    state.fcsr = sysreg_raw::fcsr::read();
  }

  static void restore_context(const state_type &state) noexcept {
    // clang-format off
    asm volatile("fld f0,  0*8(%0)\n\t"
                 "fld f1,  1*8(%0)\n\t"
                 "fld f2,  2*8(%0)\n\t"
                 "fld f3,  3*8(%0)\n\t"
                 "fld f4,  4*8(%0)\n\t"
                 "fld f5,  5*8(%0)\n\t"
                 "fld f6,  6*8(%0)\n\t"
                 "fld f7,  7*8(%0)\n\t"
                 "fld f8,  8*8(%0)\n\t"
                 "fld f9,  9*8(%0)\n\t"
                 "fld f10, 10*8(%0)\n\t"
                 "fld f11, 11*8(%0)\n\t"
                 "fld f12, 12*8(%0)\n\t"
                 "fld f13, 13*8(%0)\n\t"
                 "fld f14, 14*8(%0)\n\t"
                 "fld f15, 15*8(%0)\n\t"
                 "fld f16, 16*8(%0)\n\t"
                 "fld f17, 17*8(%0)\n\t"
                 "fld f18, 18*8(%0)\n\t"
                 "fld f19, 19*8(%0)\n\t"
                 "fld f20, 20*8(%0)\n\t"
                 "fld f21, 21*8(%0)\n\t"
                 "fld f22, 22*8(%0)\n\t"
                 "fld f23, 23*8(%0)\n\t"
                 "fld f24, 24*8(%0)\n\t"
                 "fld f25, 25*8(%0)\n\t"
                 "fld f26, 26*8(%0)\n\t"
                 "fld f27, 27*8(%0)\n\t"
                 "fld f28, 28*8(%0)\n\t"
                 "fld f29, 29*8(%0)\n\t"
                 "fld f30, 30*8(%0)\n\t"
                 "fld f31, 31*8(%0)"
                 :
                 : "r"(state.f)
                 : "memory");
    // clang-format on
    state.fcsr.write();
  }
};

} // namespace structo::arch::riscv

#endif // defined(__riscv)
