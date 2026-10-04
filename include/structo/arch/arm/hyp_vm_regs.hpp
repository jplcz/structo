// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hyp_vm_regs.hpp
 * @brief AArch32 (ARMv7-A Virtualization Extensions) hypervisor VM
 * system-register sets: the system registers a type-2 hypervisor must
 * save on VM exit and restore before VM entry -- plus the handful of
 * *banked* registers (`SP`/`LR` of every other exception mode, FIQ's
 * extra `R8`-`R12`, `SP_usr`, `ELR_hyp`) that, unlike the currently-
 * interrupted mode's own `R0`-`R12`/`SP`/`LR` (captured in the GPR trap
 * frame built on Hyp-mode entry, out of scope here), cannot be reached
 * from Hyp mode by a plain register read at all -- only via the
 * ARMv7VE banked-register transfer instruction, exactly like this
 * header's other, more obviously "system register" groups.
 *
 * Four groups, each a flat struct of `sysreg_raw::*` members plus
 * `save()`/`restore()`:
 *
 * - `vm_guest_state`: the guest's own privileged-mode control state
 *   (`SCTLR`, short- *and* LPAE-descriptor `TTBR0`/`TTBR1`, `TTBCR`,
 *   `DACR`, `PRRR`/`NMRR`/`MAIR0`/`MAIR1`, ...).
 * - `vm_hyp_control_state`: the hypervisor's own Hyp-mode
 *   virtualization-control state for this VM (`HCR`, `VTTBR`, `VTCR`,
 *   `HCPTR`, ...) -- not banked by hardware, so the hypervisor tracks
 *   one copy per VM and reloads it on every world switch.
 * - `vm_banked_state`: everything only reachable via the ARMv7VE
 *   *banked-register transfer* instruction (`MRS`/`MSR <Rd>,
 *   <banked_reg>`) -- the current mode's `SPSR` and the guest's exit
 *   PC (`ELR_hyp`), every other mode's banked `SP`/`LR`/`SPSR`
 *   (`SVC`/`ABT`/`UND`/`IRQ`), FIQ's additionally-banked `R8`-`R12`,
 *   and `SP_usr`. **Not** reachable by switching into each mode with
 *   `CPS` (unsafe/undefined from Hyp for some modes) and **not** the
 *   AArch64 EL2 mnemonic form (`mrs x0, spsr_irq`), which does not
 *   exist on ARMv7 and traps as an undefined instruction -- see
 *   `sysreg_raw::spsr_svc` and friends in `sysregs_generated.hpp` for
 *   exactly where that distinction is implemented (the `banked`
 *   encoding kind, gated by `.arch_extension virt` emitted locally per
 *   asm block).
 * - `vm_sysreg_state`: all three groups together, the full per-VM
 *   register set a VM-exit/VM-entry path saves/restores as a unit.
 * - `vcpu_sysreg_traits`: a `structo::hypervisor::vcpu_entry_guard<Traits>`
 *   (and `structo::arch::world_switch_guard<Traits>`) policy wrapping
 *   `vm_sysreg_state` as a single register group, so a vCPU's full
 *   CP15/Hyp-mode/banked register set can be entered/exited with one
 *   guard instead of hand-calling `save()`/`restore()`.
 *
 * Only compiled on a real 32-bit ARM target (`__arm__` without
 * `__aarch64__`); on every other host this header is an intentional
 * no-op so it stays header-check-clean cross-compiled from any
 * machine, matching the gating convention established by
 * `structo/arch/arm/irq_guard.hpp`. `MRC`/`MCR` to the Hyp-mode group
 * is Hyp-privileged and cannot be exercised from an unprivileged test
 * process.
 */

#include <structo/arch/arm/sysregs_generated.hpp>

#if defined(__arm__) && !defined(__aarch64__)

namespace structo::arch::arm {

/**
 * @brief Guest privileged-mode system-register state (sysregs only, no
 * GPRs). One instance per VM; reloaded every time that VM is scheduled.
 */
struct vm_guest_state {
  sysreg_raw::sctlr sctlr{};
  sysreg_raw::actlr actlr{};
  sysreg_raw::cpacr cpacr{};
  sysreg_raw::ttbcr ttbcr{};
  sysreg_raw::short_ttbr0 short_ttbr0{};
  sysreg_raw::short_ttbr1 short_ttbr1{};
  sysreg_raw::lpae_ttbr0 lpae_ttbr0{};
  sysreg_raw::lpae_ttbr1 lpae_ttbr1{};
  sysreg_raw::dacr dacr{};
  sysreg_raw::prrr prrr{};
  sysreg_raw::nmrr nmrr{};
  sysreg_raw::mair0 mair0{};
  sysreg_raw::mair1 mair1{};
  sysreg_raw::amair0 amair0{};
  sysreg_raw::amair1 amair1{};
  sysreg_raw::vbar vbar{};
  sysreg_raw::contextidr contextidr{};
  sysreg_raw::csselr csselr{};
  sysreg_raw::short_par short_par{};
  sysreg_raw::lpae_par lpae_par{};
  sysreg_raw::dfsr dfsr{};
  sysreg_raw::ifsr ifsr{};
  sysreg_raw::adfsr adfsr{};
  sysreg_raw::aifsr aifsr{};
  sysreg_raw::dfar dfar{};
  sysreg_raw::ifar ifar{};
  sysreg_raw::cntkctl cntkctl{};
  sysreg_raw::cntp_tval cntp_tval{};
  sysreg_raw::cntp_ctl cntp_ctl{};
  sysreg_raw::cntv_tval cntv_tval{};
  sysreg_raw::cntv_ctl cntv_ctl{};
  sysreg_raw::tpidrurw tpidrurw{};
  sysreg_raw::tpidruro tpidruro{};
  sysreg_raw::tpidrprw tpidrprw{};

  /** @brief Read every member register's current value. */
  [[nodiscard]] static vm_guest_state save() noexcept {
    vm_guest_state s{};
    s.sctlr = sysreg_raw::sctlr::read();
    s.actlr = sysreg_raw::actlr::read();
    s.cpacr = sysreg_raw::cpacr::read();
    s.ttbcr = sysreg_raw::ttbcr::read();
    s.short_ttbr0 = sysreg_raw::short_ttbr0::read();
    s.short_ttbr1 = sysreg_raw::short_ttbr1::read();
    s.lpae_ttbr0 = sysreg_raw::lpae_ttbr0::read();
    s.lpae_ttbr1 = sysreg_raw::lpae_ttbr1::read();
    s.dacr = sysreg_raw::dacr::read();
    s.prrr = sysreg_raw::prrr::read();
    s.nmrr = sysreg_raw::nmrr::read();
    s.mair0 = sysreg_raw::mair0::read();
    s.mair1 = sysreg_raw::mair1::read();
    s.amair0 = sysreg_raw::amair0::read();
    s.amair1 = sysreg_raw::amair1::read();
    s.vbar = sysreg_raw::vbar::read();
    s.contextidr = sysreg_raw::contextidr::read();
    s.csselr = sysreg_raw::csselr::read();
    s.short_par = sysreg_raw::short_par::read();
    s.lpae_par = sysreg_raw::lpae_par::read();
    s.dfsr = sysreg_raw::dfsr::read();
    s.ifsr = sysreg_raw::ifsr::read();
    s.adfsr = sysreg_raw::adfsr::read();
    s.aifsr = sysreg_raw::aifsr::read();
    s.dfar = sysreg_raw::dfar::read();
    s.ifar = sysreg_raw::ifar::read();
    s.cntkctl = sysreg_raw::cntkctl::read();
    s.cntp_tval = sysreg_raw::cntp_tval::read();
    s.cntp_ctl = sysreg_raw::cntp_ctl::read();
    s.cntv_tval = sysreg_raw::cntv_tval::read();
    s.cntv_ctl = sysreg_raw::cntv_ctl::read();
    s.tpidrurw = sysreg_raw::tpidrurw::read();
    s.tpidruro = sysreg_raw::tpidruro::read();
    s.tpidrprw = sysreg_raw::tpidrprw::read();
    return s;
  }

  /** @brief Write every member register back to hardware. */
  void restore() const noexcept {
    sctlr.write();
    actlr.write();
    cpacr.write();
    ttbcr.write();
    short_ttbr0.write();
    short_ttbr1.write();
    lpae_ttbr0.write();
    lpae_ttbr1.write();
    dacr.write();
    prrr.write();
    nmrr.write();
    mair0.write();
    mair1.write();
    amair0.write();
    amair1.write();
    vbar.write();
    contextidr.write();
    csselr.write();
    short_par.write();
    lpae_par.write();
    dfsr.write();
    ifsr.write();
    adfsr.write();
    aifsr.write();
    dfar.write();
    ifar.write();
    cntkctl.write();
    cntp_tval.write();
    cntp_ctl.write();
    cntv_tval.write();
    cntv_ctl.write();
    tpidrurw.write();
    tpidruro.write();
    tpidrprw.write();
  }
};

/**
 * @brief Per-VM Hyp-mode virtualization-control state (sysregs only).
 * Not banked by hardware -- the hypervisor owns one copy per VM and
 * must reload it itself on every world switch.
 */
struct vm_hyp_control_state {
  sysreg_raw::hcr hcr{};
  sysreg_raw::hdcr hdcr{};
  sysreg_raw::hcptr hcptr{};
  sysreg_raw::hstr hstr{};
  sysreg_raw::htcr htcr{};
  sysreg_raw::vtcr vtcr{};
  sysreg_raw::hmair0 hmair0{};
  sysreg_raw::hmair1 hmair1{};
  sysreg_raw::httbr httbr{};
  sysreg_raw::vttbr vttbr{};
  sysreg_raw::vpidr vpidr{};
  sysreg_raw::vmpidr vmpidr{};
  sysreg_raw::hdfar hdfar{};
  sysreg_raw::hifar hifar{};
  sysreg_raw::hpfar hpfar{};
  sysreg_raw::htpidr htpidr{};
  sysreg_raw::hvbar hvbar{};
  sysreg_raw::cnthctl cnthctl{};
  sysreg_raw::cnthp_tval cnthp_tval{};
  sysreg_raw::cnthp_ctl cnthp_ctl{};
  sysreg_raw::cnthp_cval cnthp_cval{};
  sysreg_raw::cntvoff cntvoff{};

  /** @brief Read every member register's current value. */
  [[nodiscard]] static vm_hyp_control_state save() noexcept {
    vm_hyp_control_state s{};
    s.hcr = sysreg_raw::hcr::read();
    s.hdcr = sysreg_raw::hdcr::read();
    s.hcptr = sysreg_raw::hcptr::read();
    s.hstr = sysreg_raw::hstr::read();
    s.htcr = sysreg_raw::htcr::read();
    s.vtcr = sysreg_raw::vtcr::read();
    s.hmair0 = sysreg_raw::hmair0::read();
    s.hmair1 = sysreg_raw::hmair1::read();
    s.httbr = sysreg_raw::httbr::read();
    s.vttbr = sysreg_raw::vttbr::read();
    s.vpidr = sysreg_raw::vpidr::read();
    s.vmpidr = sysreg_raw::vmpidr::read();
    s.hdfar = sysreg_raw::hdfar::read();
    s.hifar = sysreg_raw::hifar::read();
    s.hpfar = sysreg_raw::hpfar::read();
    s.htpidr = sysreg_raw::htpidr::read();
    s.hvbar = sysreg_raw::hvbar::read();
    s.cnthctl = sysreg_raw::cnthctl::read();
    s.cnthp_tval = sysreg_raw::cnthp_tval::read();
    s.cnthp_ctl = sysreg_raw::cnthp_ctl::read();
    s.cnthp_cval = sysreg_raw::cnthp_cval::read();
    s.cntvoff = sysreg_raw::cntvoff::read();
    return s;
  }

  /** @brief Write every member register back to hardware. */
  void restore() const noexcept {
    hcr.write();
    hdcr.write();
    hcptr.write();
    hstr.write();
    htcr.write();
    vtcr.write();
    hmair0.write();
    hmair1.write();
    httbr.write();
    vttbr.write();
    vpidr.write();
    vmpidr.write();
    hdfar.write();
    hifar.write();
    hpfar.write();
    htpidr.write();
    hvbar.write();
    cnthctl.write();
    cnthp_tval.write();
    cnthp_ctl.write();
    cnthp_cval.write();
    cntvoff.write();
  }
};

/**
 * @brief The guest's banked-register-transfer-only state: the current
 * mode's `SPSR` and the guest's exit PC (`ELR_hyp`), plus every other
 * exception mode's banked `SP`/`LR`/`SPSR` (and FIQ's additionally-
 * banked `R8`-`R12`) and `SP_usr` -- all of it read/written from Hyp
 * mode via the ARMv7VE banked-register transfer instruction, the only
 * architecturally correct way to reach another mode's banked state
 * without switching into it. Switching mode with `CPS` to read, say,
 * `SP_irq`, is not a safe substitute from Hyp mode, and the AArch64 EL2
 * mnemonic form (`mrs x0, spsr_irq`) is not a valid ARMv7 instruction at
 * all (undefined-instruction trap) -- see `sysreg_raw::spsr_svc` et al.
 * On a real trap-entry path these registers are *not* part of the GPR
 * trap frame (the trap frame only captures the mode that was actually
 * interrupted), so they need this same banked-register-transfer
 * mechanism, not a plain GPR save, to reach them from Hyp mode.
 */
struct vm_banked_state {
  sysreg_raw::sp_usr sp_usr{};
  sysreg_raw::elr_hyp elr_hyp{}; // guest PC at the point of the trap into Hyp mode
  sysreg_raw::spsr spsr{};       // guest CPSR at the point of the trap into Hyp mode
  sysreg_raw::sp_svc sp_svc{};
  sysreg_raw::lr_svc lr_svc{};
  sysreg_raw::spsr_svc spsr_svc{};
  sysreg_raw::sp_abt sp_abt{};
  sysreg_raw::lr_abt lr_abt{};
  sysreg_raw::spsr_abt spsr_abt{};
  sysreg_raw::sp_und sp_und{};
  sysreg_raw::lr_und lr_und{};
  sysreg_raw::spsr_und spsr_und{};
  sysreg_raw::sp_irq sp_irq{};
  sysreg_raw::lr_irq lr_irq{};
  sysreg_raw::spsr_irq spsr_irq{};
  sysreg_raw::r8_fiq r8_fiq{};
  sysreg_raw::r9_fiq r9_fiq{};
  sysreg_raw::r10_fiq r10_fiq{};
  sysreg_raw::r11_fiq r11_fiq{};
  sysreg_raw::r12_fiq r12_fiq{};
  sysreg_raw::sp_fiq sp_fiq{};
  sysreg_raw::lr_fiq lr_fiq{};
  sysreg_raw::spsr_fiq spsr_fiq{};

  /** @brief Read every banked register via the Virtualization-Extensions banked-register transfer instruction. */
  [[nodiscard]] static vm_banked_state save() noexcept {
    vm_banked_state s{};
    s.sp_usr = sysreg_raw::sp_usr::read();
    s.elr_hyp = sysreg_raw::elr_hyp::read();
    s.spsr = sysreg_raw::spsr::read();
    s.sp_svc = sysreg_raw::sp_svc::read();
    s.lr_svc = sysreg_raw::lr_svc::read();
    s.spsr_svc = sysreg_raw::spsr_svc::read();
    s.sp_abt = sysreg_raw::sp_abt::read();
    s.lr_abt = sysreg_raw::lr_abt::read();
    s.spsr_abt = sysreg_raw::spsr_abt::read();
    s.sp_und = sysreg_raw::sp_und::read();
    s.lr_und = sysreg_raw::lr_und::read();
    s.spsr_und = sysreg_raw::spsr_und::read();
    s.sp_irq = sysreg_raw::sp_irq::read();
    s.lr_irq = sysreg_raw::lr_irq::read();
    s.spsr_irq = sysreg_raw::spsr_irq::read();
    s.r8_fiq = sysreg_raw::r8_fiq::read();
    s.r9_fiq = sysreg_raw::r9_fiq::read();
    s.r10_fiq = sysreg_raw::r10_fiq::read();
    s.r11_fiq = sysreg_raw::r11_fiq::read();
    s.r12_fiq = sysreg_raw::r12_fiq::read();
    s.sp_fiq = sysreg_raw::sp_fiq::read();
    s.lr_fiq = sysreg_raw::lr_fiq::read();
    s.spsr_fiq = sysreg_raw::spsr_fiq::read();
    return s;
  }

  /** @brief Write every banked register back via the same banked-register transfer instruction. */
  void restore() const noexcept {
    sp_usr.write();
    elr_hyp.write();
    spsr.write();
    sp_svc.write();
    lr_svc.write();
    spsr_svc.write();
    sp_abt.write();
    lr_abt.write();
    spsr_abt.write();
    sp_und.write();
    lr_und.write();
    spsr_und.write();
    sp_irq.write();
    lr_irq.write();
    spsr_irq.write();
    r8_fiq.write();
    r9_fiq.write();
    r10_fiq.write();
    r11_fiq.write();
    r12_fiq.write();
    sp_fiq.write();
    lr_fiq.write();
    spsr_fiq.write();
  }
};

/** @brief Full per-VM register set: guest state, this VM's Hyp-mode virtualization controls, and the guest's
 * banked-register-transfer-only state. */
struct vm_sysreg_state {
  vm_guest_state guest{};
  vm_hyp_control_state hyp{};
  vm_banked_state banked{};

  [[nodiscard]] static vm_sysreg_state save() noexcept {
    return vm_sysreg_state{vm_guest_state::save(), vm_hyp_control_state::save(), vm_banked_state::save()};
  }

  void restore() const noexcept {
    guest.restore();
    hyp.restore();
    banked.restore();
  }
};

/**
 * @brief `structo::hypervisor::vcpu_entry_guard<Traits>` (and
 * `structo::arch::world_switch_guard<Traits>`) policy wrapping
 * `vm_sysreg_state` as a single register group: this vCPU's full
 * CP15/Hyp-mode sysreg set, saved/restored as one unit.
 */
struct vcpu_sysreg_traits {
  using state_type = vm_sysreg_state;

  [[nodiscard]] static state_type save() noexcept { return state_type::save(); }

  static void restore(const state_type &state) noexcept { state.restore(); }
};

} // namespace structo::arch::arm

#endif // defined(__arm__) && !defined(__aarch64__)
