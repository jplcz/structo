<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `irq_guard<Traits>`

`include/structo/sync/irq_guard.hpp`

An RAII interrupt-disable guard plus the supporting
`critical_section_token`/`with_irq_disabled`/`irq_locked<T>` building
blocks for writing interrupt-safe kernel code without manually pairing
`hw_save_irqs()`/`hw_restore_irqs()` calls.

`Traits` supplies the two architecture hooks (`hw_save_irqs()` /
`hw_restore_irqs(flags)`) plus a `flags_type`; everything else here (the
guard, the proof token, the functional helper, and the
`Mutex<RefCell<T>>`-style data wrapper) is architecture-agnostic and
built purely on top of those two hooks:

- `irq_guard<Traits>` -- move-only RAII guard: disables interrupts on
  construction, restores the previously-saved flags on destruction (or
  early via `unlock()`, which is safe to call more than once).
- `with_irq_disabled<Traits>(callable)` -- runs `callable` with
  interrupts disabled for its duration; `callable` may optionally accept
  a `critical_section_token` or an `irq_guard<Traits>&` if it needs to
  unlock early or prove it is running in a critical section.
- `irq_locked<T, Traits>` -- a value wrapper requiring proof of an active
  critical section (via `lock()`, `borrow(token)`, or `with_lock()`)
  before its wrapped `T` is accessible.

See the header's `@file` block for a complete `arm_irq_traits` example
(ARM-style CPSR save/restore via inline assembly).

## Bundled per-architecture `Traits`

Real, ready-made `Traits` implementations are provided per architecture
(best-effort inline assembly; not exercisable from a normal userspace
test process since they execute privileged instructions -- validated by
header-compilation only, under each architecture's own `#if`/`__arm__`-
style guard so the header is an intentional no-op when built for any
other target):

| Header | Type | Masks |
|---|---|---|
| `arch/arm/irq_guard.hpp` | `structo::arch::arm::irq_traits` | CPSR `A`/`I`/`F` ("AIF") via `cpsid aif` |
| `arch/arm64/irq_guard.hpp` | `structo::arch::arm64::irq_traits` | `DAIF` (Debug/SError/IRQ/FIQ) via `msr daifset, #0xf` |
| `arch/riscv/irq_guard.hpp` | `structo::arch::riscv::irq_traits` | `sstatus.SIE` (supervisor mode) |
| `arch/x86/irq_guard.hpp` | `structo::arch::x86::irq_traits` | `RFLAGS.IF` via `cli`, full flags saved/restored with `pushf`/`popf` |

See also: [`lazy_context.md`](lazy_context.md).
