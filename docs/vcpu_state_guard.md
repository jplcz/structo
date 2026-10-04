<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `vcpu_entry_guard<Traits>`

`include/structo/hypervisor/vcpu_state_guard.hpp`

An RAII guard bracketing a VM-entry/VM-exit pair: it loads a
persistent, caller-owned vCPU register snapshot into the live
registers on construction, restores the host's own registers on
destruction, and saves whatever the live registers hold back into that
same persistent snapshot first -- the asymmetric, two-snapshot
counterpart to [`world_switch_guard.md`](world_switch_guard.md)'s
single, symmetric snapshot:

```cpp
my_vcpu.state_type guest_regs = load_persisted_snapshot();
{
  structo::hypervisor::vcpu_entry_guard<vmx_gpr_traits> guard(guest_regs);
  // host GPRs saved, guest_regs loaded -- VMLAUNCH/VMRESUME, VMRUN, or
  // ERET into guest EL1 happens here
} // destructor: live (guest) registers saved back into guest_regs,
  // host registers restored
```

## Why not just another `world_switch_guard`

`arch/world_switch_guard.hpp` already solves "save a group of live
registers, hand control elsewhere, restore exactly what was here
before" -- but it is symmetric: one snapshot, captured from whichever
side is currently live, written back to that same side on return. A
vCPU entry is not symmetric: the registers loaded on entry (the
guest's) are not the registers restored on exit (the host's) -- two
distinct, independently-owned snapshots are involved, and the guest's
snapshot must outlive any single entry/exit pair (it is read again the
next time this same vCPU is entered, possibly after other vCPUs or the
host itself has run on this same core).

No mainstream virtualization extension swaps the entire register file
on VM-entry/VM-exit: Intel VMX's VMCS does not cover general-purpose
registers at all (KVM's `__vmx_vcpu_run` saves/restores them by hand
around `VMLAUNCH`/`VMRESUME`); AMD SVM's VMCB covers only `RAX`/`RSP`/
`RIP`/`RFLAGS`/segment state, leaving the rest to manual save/restore
around `VMRUN`; Arm EL2's `ERET` into a guest's EL1/EL0 touches only
the banked/system registers EL2 manages explicitly, leaving `X0`-`X30`
as shared, unbanked state a hypervisor's trap-entry/trap-exit assembly
must save and restore by hand.

## `Traits` contract

Same shape as `world_switch_guard`'s `Traits`:

```cpp
struct vmx_gpr_traits {
  struct state_type { std::uint64_t rax, rbx, /* ... */ r15; };
  static state_type save() noexcept;               // capture live registers
  static void restore(const state_type &) noexcept; // load live registers
};
```

## API

- `vcpu_entry_guard(typename Traits::state_type &guest_state)` --
  captures the host's live registers, then loads `guest_state` into the
  live registers. `guest_state` must outlive the guard.
- `~vcpu_entry_guard()` -- if still armed, saves the live registers
  back into `guest_state`, then restores the previously-captured host
  registers.
- `unlock()` -- performs the destructor's save/restore early and
  disarms the guard (idempotent; a disarmed guard's destructor is a
  no-op), mirroring `core_pin_guard`/`world_switch_guard`'s own
  `unlock()`.
- Move-constructible (transfers ownership and disarms the source), not
  copyable.
- `with_vcpu_entry<Traits>(guest_state, f)` -- functional wrapper
  around the same construct/destruct bracket, forwarding `f`'s return
  value; `f` may optionally take the guard by reference (e.g. to call
  `unlock()` early).
