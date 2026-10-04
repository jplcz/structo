// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pac_context.hpp
 * @brief `structo::arch::arm64::pac_regs`: the AArch64 Pointer
 * Authentication (`FEAT_PAuth`) per-task key state -- the five 128-bit
 * keys `APIAKey`/`APIBKey` (instruction-address signing), `APDAKey`/
 * `APDBKey` (data-address signing), and `APGAKey` (generic/"PACGA"
 * signing), each split across a `*KeyLo_EL1`/`*KeyHi_EL1` register
 * pair -- as a hardware-register save/restore policy usable standalone
 * or composed into a full `structo::arch::lazy_context<Traits, CpuId>`
 * `Traits` type, plus `vcpu_pac_traits`, a
 * `structo::hypervisor::vcpu_entry_guard<Traits>` (and
 * `structo::arch::world_switch_guard<Traits>`) adapter for hypervisors
 * that switch a guest's PAC key state directly on every VM-entry/
 * VM-exit rather than deferring to a per-thread lazy_context.
 *
 * `pac_regs` only implements the register-level half of the `Traits`
 * contract (`state_type`, `is_enabled()`, `enable()`, `disable()`,
 * `save_context()`, `restore_context()`); it deliberately does not pick
 * a `MaxCpus`/`CpuId` or inherit `static_per_cpu_storage` itself, since
 * how many cores to size a residency table for is a whole-system
 * decision the register file has no business making. Compose it with
 * `static_per_cpu_storage` at the call site instead:
 *
 * @code
 * struct my_pac_traits : structo::arch::arm64::pac_regs,
 *                        structo::arch::static_per_cpu_storage<8, void, std::size_t> {};
 * using pac_switcher = structo::arch::lazy_context_switcher<my_pac_traits>;
 * @endcode
 *
 * `vcpu_pac_traits` takes the opposite approach: it assumes
 * `SCTLR_EL1.{EnIA, EnIB, EnDA, EnDB}` are already set for the whole
 * lifetime of guest execution (set that up once during vCPU/hypervisor
 * initialization -- see `structo/arch/arm64/hyp_vm_regs.hpp`'s
 * `vcpu_sysreg_traits` for the matching sysreg-level guard this is
 * meant to run alongside) and only saves/restores the five key pairs'
 * content directly on every entry/exit, with no lazy trap-and-restore
 * step:
 * @code
 * using pac_guard = structo::hypervisor::vcpu_entry_guard<structo::arch::arm64::vcpu_pac_traits>;
 * @endcode
 *
 * `SCTLR_EL1.{EnIA, EnIB, EnDA, EnDB}` (bits 31, 30, 27, 13) are the
 * software-visible latches gating the `PACIA`/`AUTIA`, `PACIB`/`AUTIB`,
 * `PACDA`/`AUTDA`, and `PACDB`/`AUTDB` instruction pairs respectively:
 * while a given bit is clear, the matching sign/authenticate
 * instructions execute as `NOP`s (the "combined" hint-space encodings)
 * or trap as undefined (the dedicated encodings), rather than actually
 * signing/authenticating -- exactly the signal `is_enabled()`/
 * `enable()`/`disable()` need. All four bits are treated as a single
 * all-or-nothing gate here since this policy's job is to decide whether
 * *this thread's* key material is live in silicon at all, not to let an
 * individual thread mix-and-match which of its own key pairs are
 * trapped. `PACGA` (the generic-key MAC instruction `APGAKey` feeds) is
 * not gated by any `SCTLR_EL1` bit -- it is unconditionally available
 * whenever `FEAT_PAuth`/`FEAT_PAuth2` is implemented -- so `APGAKey` is
 * saved/restored alongside the other four keys but plays no part in the
 * enable/disable gate.
 *
 * Only compiled on a real AArch64 target (`__aarch64__`); a no-op
 * everywhere else, matching the gating convention established by
 * `structo/arch/arm64/fpsimd_context.hpp`.
 */

#include <cstdint>

#include <structo/arch/arm64/sysregs_generated.hpp>

#if defined(__aarch64__)

namespace structo::arch::arm64 {

/**
 * @brief Hardware-register save/restore policy for the AArch64 Pointer
 * Authentication extension's per-task key state. Implements the
 * register-level half of `structo::arch::lazy_context<Traits,
 * CpuId>`'s `Traits` contract; see this file's `@file` doc for how to
 * compose it with `structo::arch::static_per_cpu_storage` into a
 * complete `Traits`.
 */
struct pac_regs {
  /** @brief One 128-bit pointer-authentication key's raw storage (no numeric interpretation). */
  struct key {
    std::uint64_t lo{0};
    std::uint64_t hi{0};
  };

  /** @brief `APIAKey`, `APIBKey`, `APDAKey`, `APDBKey`, and `APGAKey`. */
  struct state_type {
    key apia{};
    key apib{};
    key apda{};
    key apdb{};
    key apga{};
  };

  [[nodiscard]] static bool is_enabled() noexcept {
    const auto sctlr = sysreg_raw::sctlr_el1::read();
    return sctlr.enia() && sctlr.enib() && sctlr.enda() && sctlr.endb();
  }

  static void enable() noexcept {
    auto sctlr = sysreg_raw::sctlr_el1::read();
    sctlr.set_enia(true);
    sctlr.set_enib(true);
    sctlr.set_enda(true);
    sctlr.set_endb(true);
    sctlr.write();
  }

  static void disable() noexcept {
    auto sctlr = sysreg_raw::sctlr_el1::read();
    sctlr.set_enia(false);
    sctlr.set_enib(false);
    sctlr.set_enda(false);
    sctlr.set_endb(false);
    sctlr.write();
  }

  static void save_context(state_type &state) noexcept {
    state.apia.lo = sysreg_raw::apiakey_lo_el1::read().raw;
    state.apia.hi = sysreg_raw::apiakey_hi_el1::read().raw;
    state.apib.lo = sysreg_raw::apibkey_lo_el1::read().raw;
    state.apib.hi = sysreg_raw::apibkey_hi_el1::read().raw;
    state.apda.lo = sysreg_raw::apdakey_lo_el1::read().raw;
    state.apda.hi = sysreg_raw::apdakey_hi_el1::read().raw;
    state.apdb.lo = sysreg_raw::apdbkey_lo_el1::read().raw;
    state.apdb.hi = sysreg_raw::apdbkey_hi_el1::read().raw;
    state.apga.lo = sysreg_raw::apgakey_lo_el1::read().raw;
    state.apga.hi = sysreg_raw::apgakey_hi_el1::read().raw;
  }

  static void restore_context(const state_type &state) noexcept {
    sysreg_raw::apiakey_lo_el1{state.apia.lo}.write();
    sysreg_raw::apiakey_hi_el1{state.apia.hi}.write();
    sysreg_raw::apibkey_lo_el1{state.apib.lo}.write();
    sysreg_raw::apibkey_hi_el1{state.apib.hi}.write();
    sysreg_raw::apdakey_lo_el1{state.apda.lo}.write();
    sysreg_raw::apdakey_hi_el1{state.apda.hi}.write();
    sysreg_raw::apdbkey_lo_el1{state.apdb.lo}.write();
    sysreg_raw::apdbkey_hi_el1{state.apdb.hi}.write();
    sysreg_raw::apgakey_lo_el1{state.apga.lo}.write();
    sysreg_raw::apgakey_hi_el1{state.apga.hi}.write();
  }
};

/**
 * @brief `structo::hypervisor::vcpu_entry_guard<Traits>` (and
 * `structo::arch::world_switch_guard<Traits>`) adapter wrapping
 * `pac_regs` as a single directly-switched register group: assumes
 * `SCTLR_EL1.{EnIA, EnIB, EnDA, EnDB}` are already set for the duration
 * of guest execution and only saves/restores the five key pairs'
 * content, with no lazy trap-and-restore step.
 */
struct vcpu_pac_traits {
  using state_type = pac_regs::state_type;

  [[nodiscard]] static state_type save() noexcept {
    state_type state{};
    pac_regs::save_context(state);
    return state;
  }

  static void restore(const state_type &state) noexcept { pac_regs::restore_context(state); }
};

} // namespace structo::arch::arm64

#endif // defined(__aarch64__)
