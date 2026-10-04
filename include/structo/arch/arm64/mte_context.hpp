// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mte_context.hpp
 * @brief `structo::arch::arm64::mte_regs`: the AArch64 Memory Tagging
 * Extension (`FEAT_MTE2`) per-task register state -- `RGSR_EL1`
 * (random tag seed), `GCR_EL1` (tag-generation control), `TFSR_EL1`/
 * `TFSRE0_EL1` (tag-fault status for EL1/EL0 accesses) -- as a
 * hardware-register save/restore policy usable standalone or composed
 * into a full `structo::arch::lazy_context<Traits, CpuId>` `Traits`
 * type, plus `vcpu_mte_traits`, a
 * `structo::hypervisor::vcpu_entry_guard<Traits>` (and
 * `structo::arch::world_switch_guard<Traits>`) adapter for hypervisors
 * that switch a guest's MTE tag-related register state directly on
 * every VM-entry/VM-exit rather than deferring to a per-thread
 * lazy_context.
 *
 * `mte_regs` only implements the register-level half of the `Traits`
 * contract (`state_type`, `is_enabled()`, `enable()`, `disable()`,
 * `save_context()`, `restore_context()`); it deliberately does not pick
 * a `MaxCpus`/`CpuId` or inherit `static_per_cpu_storage` itself, since
 * how many cores to size a residency table for is a whole-system
 * decision the register file has no business making. Compose it with
 * `static_per_cpu_storage` at the call site instead:
 *
 * @code
 * struct my_mte_traits : structo::arch::arm64::mte_regs,
 *                        structo::arch::static_per_cpu_storage<8, void, std::size_t> {};
 * using mte_switcher = structo::arch::lazy_context_switcher<my_mte_traits>;
 * @endcode
 *
 * `vcpu_mte_traits` takes the opposite approach: it saves/restores
 * `RGSR_EL1`/`GCR_EL1`/`TFSR_EL1`/`TFSRE0_EL1`/`SCTLR_EL1.TCF0`
 * directly on every entry/exit, with no lazy trap-and-restore step.
 * Its `SCTLR_EL1.TCF0` read-modify-write overlaps the full `SCTLR_EL1`
 * already carried by `structo/arch/arm64/hyp_vm_regs.hpp`'s
 * `vm_guest_el1_state`/`vcpu_sysreg_traits` -- harmless (both end up
 * writing the same bits) but redundant; a caller free to restructure
 * its own vCPU state may prefer to drop the `TCF0` field here and rely
 * on `vcpu_sysreg_traits` alone for it:
 * @code
 * using mte_guard = structo::hypervisor::vcpu_entry_guard<structo::arch::arm64::vcpu_mte_traits>;
 * @endcode
 *
 * `SCTLR_EL1.ATA0` (bit 42) is the software-visible latch gating every
 * EL0 tag-manipulating instruction (`IRG`, `ADDG`/`SUBG`, `GMI`, `STG`/
 * `STZG`/`ST2G`/`STZ2G`, `LDG`): while clear, any attempt to execute one
 * from EL0 traps as an illegal instruction -- exactly the signal
 * `is_enabled()`/`enable()`/`disable()` need (`ATA`, bit 43, is the
 * equivalent gate for EL1 and is left untouched here: this policy only
 * concerns itself with the EL0 state a `lazy_context` is meant to
 * switch per-thread). `SCTLR_EL1.TCF0` (bits 38-39) -- the tag-check-
 * fault reporting mode applied to EL0 accesses (synchronous,
 * asynchronous, or asymmetric) -- is saved/restored alongside the other
 * MTE registers as part of `state_type` rather than being folded into
 * the enable/disable gate, since it is a reporting-mode choice
 * orthogonal to whether tagged addressing is permitted at all.
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
 * @brief Hardware-register save/restore policy for the AArch64 Memory
 * Tagging Extension's per-task register state. Implements the
 * register-level half of `structo::arch::lazy_context<Traits,
 * CpuId>`'s `Traits` contract; see this file's `@file` doc for how to
 * compose it with `structo::arch::static_per_cpu_storage` into a
 * complete `Traits`.
 */
struct mte_regs {
  /** @brief `RGSR_EL1`, `GCR_EL1`, `TFSR_EL1`, `TFSRE0_EL1`, and the `TCF0` reporting-mode bits of `SCTLR_EL1`. */
  struct state_type {
    sysreg_raw::rgsr_el1 rgsr{};
    sysreg_raw::gcr_el1 gcr{};
    sysreg_raw::tfsr_el1 tfsr{};
    sysreg_raw::tfsre0_el1 tfsre0{};
    std::uint64_t tcf0{0};
  };

  [[nodiscard]] static bool is_enabled() noexcept { return sysreg_raw::sctlr_el1::read().ata0(); }

  static void enable() noexcept {
    auto sctlr = sysreg_raw::sctlr_el1::read();
    sctlr.set_ata0(true);
    sctlr.write();
  }

  static void disable() noexcept {
    auto sctlr = sysreg_raw::sctlr_el1::read();
    sctlr.set_ata0(false);
    sctlr.write();
  }

  static void save_context(state_type &state) noexcept {
    state.rgsr = sysreg_raw::rgsr_el1::read();
    state.gcr = sysreg_raw::gcr_el1::read();
    state.tfsr = sysreg_raw::tfsr_el1::read();
    state.tfsre0 = sysreg_raw::tfsre0_el1::read();
    state.tcf0 = sysreg_raw::sctlr_el1::read().tcf0();
  }

  static void restore_context(const state_type &state) noexcept {
    state.rgsr.write();
    state.gcr.write();
    state.tfsr.write();
    state.tfsre0.write();
    auto sctlr = sysreg_raw::sctlr_el1::read();
    sctlr.set_tcf0(state.tcf0);
    sctlr.write();
  }
};

/**
 * @brief `structo::hypervisor::vcpu_entry_guard<Traits>` (and
 * `structo::arch::world_switch_guard<Traits>`) adapter wrapping
 * `mte_regs` as a single directly-switched register group: saves/
 * restores `RGSR_EL1`/`GCR_EL1`/`TFSR_EL1`/`TFSRE0_EL1`/
 * `SCTLR_EL1.TCF0` content directly, with no lazy trap-and-restore
 * step.
 */
struct vcpu_mte_traits {
  using state_type = mte_regs::state_type;

  [[nodiscard]] static state_type save() noexcept {
    state_type state{};
    mte_regs::save_context(state);
    return state;
  }

  static void restore(const state_type &state) noexcept { mte_regs::restore_context(state); }
};

} // namespace structo::arch::arm64

#endif // defined(__aarch64__)
