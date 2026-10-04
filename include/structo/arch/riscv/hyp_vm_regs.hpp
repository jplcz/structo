// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hyp_vm_regs.hpp
 * @brief RISC-V Hypervisor-extension (`H`) VM CSR sets: the control and
 * status registers a type-2 hypervisor must save on VM exit and
 * restore before VM entry -- *CSRs only*, never general-purpose
 * registers (`x1`-`x31`), which live in the trap frame built by the
 * trap handler, not here.
 *
 * Two groups, each a flat struct of `sysreg_raw::*` members plus
 * `save()`/`restore()`:
 *
 * - `vm_guest_vs_state`: the guest's own VS-level (virtualized
 *   supervisor) shadow CSRs (`vsstatus`, `vstvec`, `vsscratch`,
 *   `vsepc`, `vscause`, `vstval`, `vsatp`, `vsie`, `vsip`,
 *   `vstimecmp`) -- the hardware-shadowed registers the guest OS
 *   actually executes against while `hstatus.VSBE`/`VGEIN`-style
 *   trap-and-emulate is in effect.
 * - `vm_hyp_control_state`: the hypervisor's own per-VM H-extension
 *   control CSRs (`hstatus`, `hedeleg`, `hideleg`, `hie`, `hip`,
 *   `hvip`, `htval`, `htinst`, `hgatp`, `henvcfg`, `htimedelta`,
 *   `hcounteren`, `hstateen0`, plus the read-only `hgeip`) -- not
 *   banked per-VM by hardware, so the hypervisor tracks one copy per
 *   guest and reloads it on every world switch. `hgeip` (address
 *   `0xE12`, whose top two address bits mark it architecturally
 *   read-only) is captured by `save()` for diagnostics but
 *   deliberately *not* written back by `restore()` -- a `CSRRW` to a
 *   read-only CSR address traps as an illegal instruction.
 * - `vm_sysreg_state`: both groups together, the full per-VM CSR set a
 *   VM-exit/VM-entry path saves/restores as a unit.
 * - `vcpu_sysreg_traits`: a `structo::hypervisor::vcpu_entry_guard<Traits>`
 *   (and `structo::arch::world_switch_guard<Traits>`) policy wrapping
 *   `vm_sysreg_state` as a single register group, so a vCPU's full
 *   VS-level/H-extension CSR set can be entered/exited with one guard
 *   instead of hand-calling `save()`/`restore()`.
 *
 * RV64 only (matching the rest of this repository's RISC-V scope);
 * `_h`-suffixed high-half CSRs that only exist on RV32 are out of
 * scope. The newer AIA-only CSRs (`hvien`, `hvictl`, `hviprio1/2`,
 * `vsiselect`, `vsireg`, `vstopei`, `vstopi`) are likewise out of scope
 * here -- add them if/when AIA support is needed.
 *
 * Only compiled on a real RISC-V target (`__riscv`); on every other
 * host this header is an intentional no-op so it stays header-check-
 * clean cross-compiled from any machine, matching the gating
 * convention established by `structo/arch/riscv/irq_guard.hpp`.
 * `CSRRW`/`CSRR` to the `H`-extension CSRs is HS-privileged and cannot
 * be exercised from an unprivileged test process.
 */

#include <structo/arch/riscv/sysregs_generated.hpp>

#if defined(__riscv)

namespace structo::arch::riscv {

/**
 * @brief Guest VS-level shadow CSR state (CSRs only, no GPRs). One
 * instance per VM; reloaded every time that VM is scheduled.
 */
struct vm_guest_vs_state {
  sysreg_raw::vsstatus vsstatus{};
  sysreg_raw::vstvec vstvec{};
  sysreg_raw::vsscratch vsscratch{};
  sysreg_raw::vsepc vsepc{};
  sysreg_raw::vscause vscause{};
  sysreg_raw::vstval vstval{};
  sysreg_raw::vsatp vsatp{};
  sysreg_raw::vsie vsie{};
  sysreg_raw::vsip vsip{};
  sysreg_raw::vstimecmp vstimecmp{};

  /** @brief Read every member CSR's current value. */
  [[nodiscard]] static vm_guest_vs_state save() noexcept {
    vm_guest_vs_state s{};
    s.vsstatus = sysreg_raw::vsstatus::read();
    s.vstvec = sysreg_raw::vstvec::read();
    s.vsscratch = sysreg_raw::vsscratch::read();
    s.vsepc = sysreg_raw::vsepc::read();
    s.vscause = sysreg_raw::vscause::read();
    s.vstval = sysreg_raw::vstval::read();
    s.vsatp = sysreg_raw::vsatp::read();
    s.vsie = sysreg_raw::vsie::read();
    s.vsip = sysreg_raw::vsip::read();
    s.vstimecmp = sysreg_raw::vstimecmp::read();
    return s;
  }

  /** @brief Write every member CSR back to hardware. */
  void restore() const noexcept {
    vsstatus.write();
    vstvec.write();
    vsscratch.write();
    vsepc.write();
    vscause.write();
    vstval.write();
    vsatp.write();
    vsie.write();
    vsip.write();
    vstimecmp.write();
  }
};

/**
 * @brief Per-VM `H`-extension control CSR state. Not banked by
 * hardware -- the hypervisor owns one copy per VM and must reload it
 * itself on every world switch.
 */
struct vm_hyp_control_state {
  sysreg_raw::hstatus hstatus{};
  sysreg_raw::hedeleg hedeleg{};
  sysreg_raw::hideleg hideleg{};
  sysreg_raw::hie hie{};
  sysreg_raw::hip hip{};
  sysreg_raw::hvip hvip{};
  sysreg_raw::htval htval{};
  sysreg_raw::htinst htinst{};
  sysreg_raw::hgatp hgatp{};
  sysreg_raw::hgeip hgeip{}; //!< Read-only; captured by save(), never written by restore().
  sysreg_raw::henvcfg henvcfg{};
  sysreg_raw::htimedelta htimedelta{};
  sysreg_raw::hcounteren hcounteren{};
  sysreg_raw::hstateen0 hstateen0{};

  /** @brief Read every member CSR's current value. */
  [[nodiscard]] static vm_hyp_control_state save() noexcept {
    vm_hyp_control_state s{};
    s.hstatus = sysreg_raw::hstatus::read();
    s.hedeleg = sysreg_raw::hedeleg::read();
    s.hideleg = sysreg_raw::hideleg::read();
    s.hie = sysreg_raw::hie::read();
    s.hip = sysreg_raw::hip::read();
    s.hvip = sysreg_raw::hvip::read();
    s.htval = sysreg_raw::htval::read();
    s.htinst = sysreg_raw::htinst::read();
    s.hgatp = sysreg_raw::hgatp::read();
    s.hgeip = sysreg_raw::hgeip::read();
    s.henvcfg = sysreg_raw::henvcfg::read();
    s.htimedelta = sysreg_raw::htimedelta::read();
    s.hcounteren = sysreg_raw::hcounteren::read();
    s.hstateen0 = sysreg_raw::hstateen0::read();
    return s;
  }

  /** @brief Write every member CSR back to hardware. */
  void restore() const noexcept {
    hstatus.write();
    hedeleg.write();
    hideleg.write();
    hie.write();
    hip.write();
    hvip.write();
    htval.write();
    htinst.write();
    hgatp.write();
    // hgeip intentionally not written back -- read-only CSR, see save()'s comment.
    henvcfg.write();
    htimedelta.write();
    hcounteren.write();
    hstateen0.write();
  }
};

/** @brief Full per-VM CSR set: guest VS-level shadow state plus this VM's H-extension controls. */
struct vm_sysreg_state {
  vm_guest_vs_state guest_vs{};
  vm_hyp_control_state hyp{};

  [[nodiscard]] static vm_sysreg_state save() noexcept {
    return vm_sysreg_state{vm_guest_vs_state::save(), vm_hyp_control_state::save()};
  }

  void restore() const noexcept {
    guest_vs.restore();
    hyp.restore();
  }
};

/**
 * @brief `structo::hypervisor::vcpu_entry_guard<Traits>` (and
 * `structo::arch::world_switch_guard<Traits>`) policy wrapping
 * `vm_sysreg_state` as a single register group: this vCPU's full
 * VS-level/H-extension CSR set, saved/restored as one unit.
 */
struct vcpu_sysreg_traits {
  using state_type = vm_sysreg_state;

  [[nodiscard]] static state_type save() noexcept { return state_type::save(); }

  static void restore(const state_type &state) noexcept { state.restore(); }
};

} // namespace structo::arch::riscv

#endif // defined(__riscv)
