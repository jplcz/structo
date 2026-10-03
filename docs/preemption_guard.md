<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `preemption_guard<Traits>`

`include/structo/sync/preemption_guard.hpp`

An RAII preemption-disable guard plus the supporting
`preemption_disabled_token`/`with_preemption_disabled`/
`preempt_locked<T>` building blocks -- the same shape as
[`irq_guard.md`](irq_guard.md)'s toolkit, but for scheduler preemption
rather than interrupts.

`Traits` supplies two scheduler hooks (`disable_preemption()` /
`enable_preemption()`); everything else here (the guard, the proof
token, the functional helper, and the `Mutex<RefCell<T>>`-style data
wrapper) is scheduler-agnostic and built purely on top of those two
hooks:

- `preemption_guard<Traits>` -- move-only RAII guard: disables
  preemption on construction, re-enables it on destruction (or early via
  `unlock()`, which is safe to call more than once).
- `with_preemption_disabled<Traits>(callable)` -- runs `callable` with
  preemption disabled for its duration; `callable` may optionally accept
  a `preemption_disabled_token` or a `preemption_guard<Traits>&` if it
  needs to unlock early or prove it is running with preemption disabled.
- `preempt_locked<T, Traits>` -- a value wrapper requiring proof of an
  active preemption-disabled section (via `lock()`, `borrow(token)`, or
  `with_lock()`) before its wrapped `T` is accessible.

Unlike `irq_guard::Traits`, there is no `flags_type` to save and
restore: disabling/enabling preemption is expected to be a simple
nestable counter (Linux's per-CPU `preempt_count`, FreeBSD's
`td_critnest`), so `Traits::enable_preemption()` takes no argument --
the scheduler's own counter tracks nesting, not this guard.

Note that disabling preemption does **not** by itself disable
interrupts, and vice versa; combine with `irq_guard` (and/or
[`core_pin_guard`](core_pin_guard.md)) when stronger exclusion is
required.

See the header's `@file` block for a complete `kernel_preempt_traits`
example (a per-CPU nesting counter plus a reschedule check on the
outermost re-enable).

See also: [`irq_guard.md`](irq_guard.md), [`core_pin_guard.md`](core_pin_guard.md), [`spinlock_entry_guard.md`](spinlock_entry_guard.md).
