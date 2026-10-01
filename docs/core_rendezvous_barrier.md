<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `core_rendezvous_barrier<Traits>`

`include/structo/sync/core_rendezvous_barrier.hpp`

A reusable, spin-only SMP rendezvous point: "wait until exactly
`num_cores` cores have arrived, then release them all together" --
for contexts where blocking/parking a core is not an option (early
boot, an IPI/NMI handler, a hypervisor monitor with no scheduler to
park against):

```cpp
structo::sync::core_rendezvous_barrier<my_rendezvous_traits> barrier(num_online_cpus);

// On every participating core:
const bool is_leader = barrier.wait([] {
  // Invoked once per spin iteration on every core that arrives before
  // the rest -- drain pending IPIs, pet a watchdog, poll for an abort.
  service_pending_ipis();
});
if (is_leader) {
  // Exactly one arbitrarily-chosen core per wave runs this once.
  advance_shootdown_generation();
}
```

This is deliberately *not* `reloco::barrier`: `reloco::barrier` blocks
each waiting thread via `futex_wait` (parking it with the OS
scheduler) -- the right default for ordinary userspace/kernel-thread
code, but unusable in the same contexts
[`spin_lock.hpp`](https://github.com/jplcz/reloco/blob/master/include/reloco/spin_lock.hpp)
already explains are unusable for a mutex: interrupt/exception
handlers, before a scheduler exists, or a panic/fault path.
`core_rendezvous_barrier` fills that gap for a barrier: non-leader
cores never park, they spin, invoking a caller-supplied callback once
per iteration so the caller can make progress while waiting instead of
just burning cycles.

`Traits` supplies exactly one hook:

- `static void spin_wait() noexcept;` -- invoked once per spin
  iteration *after* the caller's own on-spin callback, so a concrete
  port can substitute a power-efficient wait (Arm `WFE`, x86 `PAUSE`
  via `reloco::hint::spin_loop()`, ...) for a plain busy-loop.

Like `reloco::barrier`, a *generation* counter (a plain `std::atomic`,
not `futex_word` -- nothing here ever blocks, so there is nothing to
wake) makes the barrier safely reusable across an unbounded number of
waves, and `wait()`/`wait(on_spin)` return `true` for exactly one
arbitrarily-chosen core per wave (the "leader" -- the core whose
arrival completed it), `false` for every other participant, matching
`reloco::barrier::wait()`'s own leader-election convention.

See the header's `@file` block for complete `portable_rendezvous_traits`
(portable `PAUSE`/`YIELD` via `reloco::hint::spin_loop()`) and
`arm_wfe_rendezvous_traits` (`WFE`) examples.

See also: [`irq_guard.md`](irq_guard.md), [`core_pin_guard.md`](core_pin_guard.md), [`preemption_guard.md`](preemption_guard.md).
