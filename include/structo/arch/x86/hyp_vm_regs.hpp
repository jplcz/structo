// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hyp_vm_regs.hpp
 * @brief x86/x86-64 hypervisor VM register sets: the handful of
 * per-vCPU registers a VMX/SVM hypervisor must save on VM exit and
 * restore before VM entry that are *not* already auto-switched by the
 * VMCS/VMCB's guest-state area, and are not general-purpose registers
 * (`RAX`-`R15`, `RIP`, `RSP`), which live in the trap frame built by
 * the VM-exit handler stub, not here. `CR0`/`CR3`/`CR4`, `RFLAGS`,
 * `RSP`/`RIP`, the segment registers, `DR7`, and `IA32_EFER` are all
 * ordinary VMCS guest-state fields loaded/stored by hardware itself on
 * every VM entry/exit, so this header deliberately does not model
 * them.
 *
 * Two groups, each a flat struct of `sysreg_raw::*` members plus
 * `save()`/`restore()`:
 *
 * - `vm_debug_state`: `DR0`-`DR3` and `DR6`. Unlike `DR7`, these are
 *   not part of the VMCS guest-state area, so a hypervisor that wants
 *   per-VM breakpoint/watchpoint state to survive a world switch must
 *   read/write them itself with `MOV`.
 * - `vm_misc_sysreg_state`: `XCR0` (the `XGETBV`/`XSETBV`-accessed
 *   extended-state-enable register controlling which parts of the
 *   XSAVE area are active -- not part of the VMCS, swapped with
 *   `XSETBV` on every world switch) and `IA32_KERNEL_GS_BASE` (x86-64
 *   only; the `SWAPGS` target address, likewise not a VMCS field and
 *   so swapped by hand with `RDMSR`/`WRMSR`).
 * - `vm_sysreg_state`: both groups together, the full per-VM register
 *   set a VM-exit/VM-entry path saves/restores as a unit.
 * - `vcpu_sysreg_traits`: a `structo::hypervisor::vcpu_entry_guard<Traits>`
 *   (and `structo::arch::world_switch_guard<Traits>`) policy wrapping
 *   `vm_sysreg_state` as a single register group, so a vCPU's full
 *   non-VMCS-managed register set can be entered/exited with one guard
 *   instead of hand-calling `save()`/`restore()`.
 *
 * Only compiled on a real x86/x86-64 target (`__i386__`/`__x86_64__`);
 * on every other host this header is an intentional no-op so it stays
 * header-check-clean cross-compiled from any machine, matching the
 * gating convention established by `structo/arch/x86/irq_guard.hpp`.
 * `MOV` to/from the debug registers and `XSETBV` both require
 * sufficient privilege (ring 0 and `CR4.OSXSAVE`, respectively) and
 * cannot be exercised from an unprivileged test process.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch::x86;
 *
 * // On VM exit: snapshot the guest's non-VMCS-managed state.
 * vm_sysreg_state guest = vm_sysreg_state::save();
 * host_state.restore();
 *
 * // ... handle the exit ...
 *
 * // On VM entry: restore the guest's state before VMLAUNCH/VMRESUME.
 * guest.restore();
 * @endcode
 */

#include <structo/arch/x86/sysregs_generated.hpp>

#if defined(__i386__) || defined(__x86_64__)

namespace structo::arch::x86 {

/**
 * @brief Per-VM debug-register state (`DR0`-`DR3`, `DR6`). Not part of
 * the VMCS guest-state area -- the hypervisor owns one copy per VM and
 * must reload it itself on every world switch.
 */
struct vm_debug_state {
  sysreg_raw::dr0 dr0{};
  sysreg_raw::dr1 dr1{};
  sysreg_raw::dr2 dr2{};
  sysreg_raw::dr3 dr3{};
  sysreg_raw::dr6 dr6{};

  /** @brief Read every member register's current value. */
  [[nodiscard]] static vm_debug_state save() noexcept {
    vm_debug_state s{};
    s.dr0 = sysreg_raw::dr0::read();
    s.dr1 = sysreg_raw::dr1::read();
    s.dr2 = sysreg_raw::dr2::read();
    s.dr3 = sysreg_raw::dr3::read();
    s.dr6 = sysreg_raw::dr6::read();
    return s;
  }

  /** @brief Write every member register back to hardware. */
  void restore() const noexcept {
    dr0.write();
    dr1.write();
    dr2.write();
    dr3.write();
    dr6.write();
  }
};

/**
 * @brief Per-VM miscellaneous non-VMCS-managed register state (`XCR0`,
 * `IA32_KERNEL_GS_BASE`). Not banked by hardware -- the hypervisor
 * owns one copy per VM and must reload it itself on every world
 * switch.
 */
struct vm_misc_sysreg_state {
  sysreg_raw::xcr0 xcr0{};
#if defined(__x86_64__)
  sysreg_raw::ia32_kernel_gs_base ia32_kernel_gs_base{};
#endif

  /** @brief Read every member register's current value. */
  [[nodiscard]] static vm_misc_sysreg_state save() noexcept {
    vm_misc_sysreg_state s{};
    s.xcr0 = sysreg_raw::xcr0::read();
#if defined(__x86_64__)
    s.ia32_kernel_gs_base = sysreg_raw::ia32_kernel_gs_base::read();
#endif
    return s;
  }

  /** @brief Write every member register back to hardware. */
  void restore() const noexcept {
    xcr0.write();
#if defined(__x86_64__)
    ia32_kernel_gs_base.write();
#endif
  }
};

/** @brief Full per-VM register set: debug registers plus the remaining non-VMCS-managed state. */
struct vm_sysreg_state {
  vm_debug_state debug{};
  vm_misc_sysreg_state misc{};

  [[nodiscard]] static vm_sysreg_state save() noexcept {
    return vm_sysreg_state{vm_debug_state::save(), vm_misc_sysreg_state::save()};
  }

  void restore() const noexcept {
    debug.restore();
    misc.restore();
  }
};

/**
 * @brief `structo::hypervisor::vcpu_entry_guard<Traits>` (and
 * `structo::arch::world_switch_guard<Traits>`) policy wrapping
 * `vm_sysreg_state` as a single register group: this vCPU's full
 * non-VMCS-managed register set, saved/restored as one unit.
 */
struct vcpu_sysreg_traits {
  using state_type = vm_sysreg_state;

  [[nodiscard]] static state_type save() noexcept { return state_type::save(); }

  static void restore(const state_type &state) noexcept { state.restore(); }
};

} // namespace structo::arch::x86

#endif // defined(__i386__) || defined(__x86_64__)
