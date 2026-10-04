// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hyp_vm_regs.hpp
 * @brief AArch64 hypervisor VM system-register sets: the system
 * registers a type-2 hypervisor must save on VM exit and restore before
 * VM entry so a guest's EL1/EL0 view of the machine survives across a
 * trap to EL2. *System registers only* -- `X0`-`X30`, `SP_EL0`, and
 * `PC` live in the GPR trap frame built by the exception vector, not
 * here, but `SP_EL1` is **not** part of that trap frame: it is its own
 * named EL2-accessible system register (`MRS`/`MSR <Rd>, SP_EL1`, only
 * encodable with `op1=4`, i.e. from EL2 or higher), so it needs the
 * same explicit `read()`/`write()` treatment as every other member
 * below, not a GPR-frame copy.
 *
 * Three groups, each a flat struct of `sysreg_raw::*` members plus
 * `save()`/`restore()`:
 *
 * - `vm_guest_el1_state`: the guest's own EL1/EL0 control state
 *   (`SCTLR_EL1`, `TTBR0_EL1`/`TTBR1_EL1`, `TCR_EL1`, `MAIR_EL1`,
 *   `SP_EL1`, ...) -- banked per `vttbr_el2`/VMID by the MMU hardware
 *   itself, but still software-visible as ordinary EL1 registers that
 *   must be swapped when switching which guest (or the host) is
 *   scheduled at EL1.
 * - `vm_hyp_el2_state`: the hypervisor's own EL2 virtualization-control
 *   state for this VM (`HCR_EL2`, `VTTBR_EL2`, `VTCR_EL2`, `CPTR_EL2`,
 *   ...) -- not banked by hardware, so the hypervisor must track one
 *   copy per VM itself and reload it on every world switch.
 * - `vm_sysreg_state`: both of the above together, the full per-VM
 *   sysreg set a VM-exit/VM-entry path saves/restores as a unit.
 * - `vcpu_sysreg_traits`: a `structo::hypervisor::vcpu_entry_guard<Traits>`
 *   (and `structo::arch::world_switch_guard<Traits>`) policy wrapping
 *   `vm_sysreg_state` as a single register group, so a vCPU's full
 *   EL1/EL2 sysreg set can be entered/exited with one guard instead of
 *   hand-calling `save()`/`restore()`.
 *
 * Only compiled on a real AArch64 target (`__aarch64__`); on every
 * other host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine, matching the
 * gating convention established by `structo/arch/arm64/irq_guard.hpp`.
 * `MRS`/`MSR` to the EL2 group is EL2-privileged and cannot be
 * exercised from an unprivileged test process.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch::arm64;
 *
 * // On VM exit: snapshot the guest's EL1 state, restore the host's.
 * vm_guest_el1_state guest = vm_guest_el1_state::save();
 * host_el1.restore();
 *
 * // ... handle the exit ...
 *
 * // On VM entry: restore the guest's EL1 state and EL2 VM controls.
 * guest.restore();
 * vm_hcr.restore();
 * @endcode
 */

#include <structo/arch/arm64/sysregs_generated.hpp>

#if defined(__aarch64__)

namespace structo::arch::arm64 {

/**
 * @brief Guest EL1/EL0 system-register state (sysregs only, no GPRs).
 * One instance per VM; reloaded every time that VM's EL1 is scheduled.
 */
struct vm_guest_el1_state {
  sysreg_raw::sctlr_el1 sctlr_el1{};
  sysreg_raw::actlr_el1 actlr_el1{};
  sysreg_raw::cpacr_el1 cpacr_el1{};
  sysreg_raw::ttbr0_el1 ttbr0_el1{};
  sysreg_raw::ttbr1_el1 ttbr1_el1{};
  sysreg_raw::tcr_el1 tcr_el1{};
  sysreg_raw::mair_el1 mair_el1{};
  sysreg_raw::amair_el1 amair_el1{};
  sysreg_raw::vbar_el1 vbar_el1{};
  sysreg_raw::contextidr_el1 contextidr_el1{};
  sysreg_raw::csselr_el1 csselr_el1{};
  sysreg_raw::par_el1 par_el1{};
  sysreg_raw::afsr0_el1 afsr0_el1{};
  sysreg_raw::afsr1_el1 afsr1_el1{};
  sysreg_raw::esr_el1 esr_el1{};
  sysreg_raw::far_el1 far_el1{};
  sysreg_raw::elr_el1 elr_el1{};
  sysreg_raw::spsr_el1 spsr_el1{};
  sysreg_raw::mdscr_el1 mdscr_el1{};
  sysreg_raw::cntkctl_el1 cntkctl_el1{};
  sysreg_raw::cntv_cval_el0 cntv_cval_el0{};
  sysreg_raw::cntv_ctl_el0 cntv_ctl_el0{};
  sysreg_raw::cntp_cval_el0 cntp_cval_el0{};
  sysreg_raw::cntp_ctl_el0 cntp_ctl_el0{};
  sysreg_raw::tpidr_el0 tpidr_el0{};
  sysreg_raw::tpidr_el1 tpidr_el1{};
  sysreg_raw::sp_el1 sp_el1{};

  /** @brief Read every member register's current value. */
  [[nodiscard]] static vm_guest_el1_state save() noexcept {
    vm_guest_el1_state s{};
    s.sctlr_el1 = sysreg_raw::sctlr_el1::read();
    s.actlr_el1 = sysreg_raw::actlr_el1::read();
    s.cpacr_el1 = sysreg_raw::cpacr_el1::read();
    s.ttbr0_el1 = sysreg_raw::ttbr0_el1::read();
    s.ttbr1_el1 = sysreg_raw::ttbr1_el1::read();
    s.tcr_el1 = sysreg_raw::tcr_el1::read();
    s.mair_el1 = sysreg_raw::mair_el1::read();
    s.amair_el1 = sysreg_raw::amair_el1::read();
    s.vbar_el1 = sysreg_raw::vbar_el1::read();
    s.contextidr_el1 = sysreg_raw::contextidr_el1::read();
    s.csselr_el1 = sysreg_raw::csselr_el1::read();
    s.par_el1 = sysreg_raw::par_el1::read();
    s.afsr0_el1 = sysreg_raw::afsr0_el1::read();
    s.afsr1_el1 = sysreg_raw::afsr1_el1::read();
    s.esr_el1 = sysreg_raw::esr_el1::read();
    s.far_el1 = sysreg_raw::far_el1::read();
    s.elr_el1 = sysreg_raw::elr_el1::read();
    s.spsr_el1 = sysreg_raw::spsr_el1::read();
    s.mdscr_el1 = sysreg_raw::mdscr_el1::read();
    s.cntkctl_el1 = sysreg_raw::cntkctl_el1::read();
    s.cntv_cval_el0 = sysreg_raw::cntv_cval_el0::read();
    s.cntv_ctl_el0 = sysreg_raw::cntv_ctl_el0::read();
    s.cntp_cval_el0 = sysreg_raw::cntp_cval_el0::read();
    s.cntp_ctl_el0 = sysreg_raw::cntp_ctl_el0::read();
    s.tpidr_el0 = sysreg_raw::tpidr_el0::read();
    s.tpidr_el1 = sysreg_raw::tpidr_el1::read();
    s.sp_el1 = sysreg_raw::sp_el1::read();
    return s;
  }

  /** @brief Write every member register back to hardware. */
  void restore() const noexcept {
    sctlr_el1.write();
    actlr_el1.write();
    cpacr_el1.write();
    ttbr0_el1.write();
    ttbr1_el1.write();
    tcr_el1.write();
    mair_el1.write();
    amair_el1.write();
    vbar_el1.write();
    contextidr_el1.write();
    csselr_el1.write();
    par_el1.write();
    afsr0_el1.write();
    afsr1_el1.write();
    esr_el1.write();
    far_el1.write();
    elr_el1.write();
    spsr_el1.write();
    mdscr_el1.write();
    cntkctl_el1.write();
    cntv_cval_el0.write();
    cntv_ctl_el0.write();
    cntp_cval_el0.write();
    cntp_ctl_el0.write();
    tpidr_el0.write();
    tpidr_el1.write();
    sp_el1.write();
  }
};

/**
 * @brief Per-VM EL2 virtualization-control state (sysregs only). Not
 * banked by hardware -- the hypervisor owns one copy per VM and must
 * reload it itself on every world switch.
 */
struct vm_hyp_el2_state {
  sysreg_raw::hcr_el2 hcr_el2{};
  sysreg_raw::mdcr_el2 mdcr_el2{};
  sysreg_raw::cptr_el2 cptr_el2{};
  sysreg_raw::hstr_el2 hstr_el2{};
  sysreg_raw::vttbr_el2 vttbr_el2{};
  sysreg_raw::vtcr_el2 vtcr_el2{};
  sysreg_raw::vpidr_el2 vpidr_el2{};
  sysreg_raw::vmpidr_el2 vmpidr_el2{};
  sysreg_raw::cnthctl_el2 cnthctl_el2{};
  sysreg_raw::cntvoff_el2 cntvoff_el2{};
  sysreg_raw::tpidr_el2 tpidr_el2{};

  /** @brief Read every member register's current value. */
  [[nodiscard]] static vm_hyp_el2_state save() noexcept {
    vm_hyp_el2_state s{};
    s.hcr_el2 = sysreg_raw::hcr_el2::read();
    s.mdcr_el2 = sysreg_raw::mdcr_el2::read();
    s.cptr_el2 = sysreg_raw::cptr_el2::read();
    s.hstr_el2 = sysreg_raw::hstr_el2::read();
    s.vttbr_el2 = sysreg_raw::vttbr_el2::read();
    s.vtcr_el2 = sysreg_raw::vtcr_el2::read();
    s.vpidr_el2 = sysreg_raw::vpidr_el2::read();
    s.vmpidr_el2 = sysreg_raw::vmpidr_el2::read();
    s.cnthctl_el2 = sysreg_raw::cnthctl_el2::read();
    s.cntvoff_el2 = sysreg_raw::cntvoff_el2::read();
    s.tpidr_el2 = sysreg_raw::tpidr_el2::read();
    return s;
  }

  /** @brief Write every member register back to hardware. */
  void restore() const noexcept {
    hcr_el2.write();
    mdcr_el2.write();
    cptr_el2.write();
    hstr_el2.write();
    vttbr_el2.write();
    vtcr_el2.write();
    vpidr_el2.write();
    vmpidr_el2.write();
    cnthctl_el2.write();
    cntvoff_el2.write();
    tpidr_el2.write();
  }
};

/** @brief Full per-VM sysreg set: guest EL1/EL0 state plus this VM's EL2 virtualization controls. */
struct vm_sysreg_state {
  vm_guest_el1_state el1{};
  vm_hyp_el2_state el2{};

  [[nodiscard]] static vm_sysreg_state save() noexcept {
    return vm_sysreg_state{vm_guest_el1_state::save(), vm_hyp_el2_state::save()};
  }

  void restore() const noexcept {
    el1.restore();
    el2.restore();
  }
};

/**
 * @brief `structo::hypervisor::vcpu_entry_guard<Traits>` (and
 * `structo::arch::world_switch_guard<Traits>`) policy wrapping
 * `vm_sysreg_state` as a single register group: this vCPU's full
 * EL1/EL2 sysreg set, saved/restored as one unit.
 */
struct vcpu_sysreg_traits {
  using state_type = vm_sysreg_state;

  [[nodiscard]] static state_type save() noexcept { return state_type::save(); }

  static void restore(const state_type &state) noexcept { state.restore(); }
};

} // namespace structo::arch::arm64

#endif // defined(__aarch64__)
