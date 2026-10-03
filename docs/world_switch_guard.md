<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `world_switch_guard<Traits>`

`include/structo/arch/world_switch_guard.hpp`

An RAII guard that saves a `Traits`-defined group of **non-banked**
registers on construction and restores them on destruction -- the same
shape as [`core_pin_guard.md`](core_pin_guard.md)'s guard, but
bracketing a security-domain/world switch (ARM TrustZone Secure/
Non-secure, a Realm Management Extension transition, or any other case
where two privilege domains share one physical register hardware does
not automatically swap for them) instead of a scheduling hazard:

```cpp
{
  structo::arch::world_switch_guard<el1_sysregs_traits> guard; // save() now
  enter_other_world(); // SMC/ERET -- the other world may freely overwrite
                        // every one of these registers for its own purposes
} // destructor: restore() -- our own context is exactly as we left it
```

## Banked vs. non-banked: not every register survives a world switch for free

A TrustZone-capable ARMv7-A/AArch32 core hardware-**banks** several PL1
registers (`SCTLR`, `TTBR0`/`TTBR1`, `TTBCR`, `CONTEXTIDR`, ... -- see
[`mmu_regs.md`](mmu_regs.md)): the Secure and Non-secure worlds each get
their own private copy, and a plain `MRC`/`MCR` always reaches whichever
copy belongs to the world currently executing, so a world switch leaves
them untouched from each world's own point of view -- nothing to save.

AArch64 draws this line very differently: its EL1 system registers
(`SCTLR_EL1`, `TTBR0_EL1`/`TTBR1_EL1`, `TCR_EL1`, `MAIR_EL1`,
`CONTEXTIDR_EL1`, `VBAR_EL1`, `ELR_EL1`, `SPSR_EL1`, `FAR_EL1`,
`ESR_EL1`, `TPIDR_EL0`/`TPIDR_EL1`, ... -- see
[`mmu_regs.md`](mmu_regs.md)'s `scr_el3`) have **no hardware-banked
per-world copy at all**: `SCR_EL3.NS` only ever selects which world the
next lower exception level *runs as*, never which physical register
bank a plain `MRS`/`MSR` reaches. If EL3 firmware does nothing, the
incoming world sees whatever the outgoing world last left in these
registers -- wrong, and often a direct Secure-to-Non-secure information
leak. This is exactly why Arm Trusted Firmware-A's EL3 runtime (BL31)
carries a `cm_el1_sysregs_context_save()`/`cm_el1_sysregs_context_
restore()` pair (see `el1_sysregs_t` in
`include/lib/el3_runtime/context_el1.h`), called on every Secure/
Non-secure transition -- and, separately, an analogous save/restore
pair for the FP/SIMD register file (`fpregs_context_t`), which is
likewise shared, unbanked state. `world_switch_guard<Traits>` is a
generic, reusable customization point for exactly that obligation, for
either register group (or any other shared, non-banked register set a
caller's own world-switch path needs to carry across the boundary).

## The customization point

`Traits` supplies an opaque `state_type` plus two static hooks:

- `static state_type save() noexcept;` -- captures every register this
  group covers into a fresh `state_type`.
- `static void restore(const state_type &) noexcept;` -- writes a
  previously captured snapshot back into those same registers.

Neither hook knows or cares which direction the world switch is going,
or which world is "ours" -- the guard only ever says "whatever is live
right now, keep it safe until I say otherwise", which is symmetric
regardless of which side of the switch is doing the saving. See the
header's `@file` block for a complete `el1_sysregs_traits` example
mirroring Arm Trusted Firmware-A's own register list.

## API

- `world_switch_guard<Traits>` -- move-only RAII guard: captures a
  snapshot via `Traits::save()` on construction, writes it back via
  `Traits::restore()` on destruction (or early via `unlock()`, safe to
  call more than once); `is_armed()` reports whether a restore is still
  pending; `state()` exposes the captured snapshot.
- `with_world_switch<Traits>(callable)` -- runs `callable` with the
  register group captured, restoring it afterwards; `callable` may
  optionally accept the guard by reference.

## Composing several register groups

Composing several independent register groups (e.g. EL1 sysregs and
FP/SIMD) is just nesting two guards, one per `Traits`, each restoring
only its own group -- never one monolithic `Traits` unless a caller
actually wants both saved/restored atomically together. The guard never
issues the world switch itself (no `SMC`/`ERET`/whatever a caller's own
monitor code does) -- exactly as [`tlb_flush.md`](tlb_flush.md)/
[`address_translate.md`](address_translate.md) never decide *when* to
flush or translate, only *how*. The actual transition is a separate,
explicit action the caller performs inside the guard's scope.

See also: [`mmu_regs.md`](mmu_regs.md), [`execution_domain.md`](execution_domain.md), [`core_pin_guard.md`](core_pin_guard.md).
