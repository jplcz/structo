<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::sched_world_handoff`

`include/structo/sched_world_handoff.hpp`

Per-CPU state tracking for handing a CPU back and forth between this
kernel and another, cooperating execution context ("the other world"
-- e.g. a hypervisor's other guest, a TEE's normal-world OS, or any
other scheme where two independent kernels take turns owning a core)
via one dedicated per-CPU "world thread", built on top of
[`sched.hpp`](sched.md)'s `force_next`.

## The model: a world thread is a first-class, per-CPU-bound task

Exactly one `Entry` per CPU -- the "world thread" -- represents "this
CPU is currently executing the other world". Handing the CPU to the
other world is not a special, scheduler-invisible context switch: it is
an ordinary dispatch of the world thread, through the exact same
`enqueue`/`pick_next`/`on_block`/`on_wake` calls every other task on
this CPU goes through. This keeps every other scheduler-level property
(SMP work-stealing over [`work_steal.hpp`](work_steal.md), per-CPU load
accounting over [`cpu_load.hpp`](cpu_load.md), priority ordering, ...)
working unmodified: the world thread is just another runnable entry,
not a parallel, out-of-band execution mode the rest of the scheduler
has to know about.

What *is* special about the world thread, and what `sched_world_handoff`
exists to track, is a small amount of per-CPU bookkeeping no other task
needs:

- **Forcing a return to the other world, unconditionally.** Whatever is
  runnable locally, once the other world must be given the CPU back
  (e.g. an interrupt belonging to it needs routing there), the world
  thread must be dispatched *next*, regardless of priority. This is
  exactly `sched.hpp`'s `force_next` primitive -- `request_force_switch()`
  calls it for the caller.
- **Tracking how long the other world has held the CPU.** Unlike an
  ordinary task, the world thread has no scheduler-visible quantum: it
  can run for as long as the other world chooses, cooperatively. This
  class measures that dwell time itself (`dwell_time()`) so a caller can
  both enforce a bounded budget (`should_switch_to_world()` forces a
  pseudo-IRQ-style reentry once `Traits::max_dwell()` is exceeded) and
  detect a CPU that looks stuck (`stuck_warning_due()`).
- **Distinguishing a cooperative entry from a forced one.** The other
  world is normally entered via its own accessor call (e.g. a
  hypercall) -- `world_entry_kind::cooperative`. It can also be entered
  by a forced kernel entry that happens without that cooperation at all
  (e.g. this kernel's own interrupt handler firing while the other
  world was already running) -- `world_entry_kind::critical`. This
  class only records which kind is in effect (`entry_kind()`); deciding
  which concrete work is safe to run during a critical entry is the
  caller's own policy, built on top of that flag.

## What this class does *not* do

It does not own a runqueue, does not implement a `schedule()` loop, and
does not decide *what* to run -- it is a thin, per-CPU state object a
caller's own scheduler-dispatch code consults and updates at a handful
of well-defined checkpoints, alongside its ordinary `Sched` calls
(`enqueue`/`pick_next`/`on_block`/`on_wake`/...). See "Usage" below for
exactly which checkpoints.

## `Traits`

```cpp
struct my_world_handoff_traits {
  // Budget before a cooperatively-entered other world is forced to
  // give the CPU back, even without an explicit switch request.
  static constexpr reloco::duration max_dwell() noexcept { return reloco::duration::from_millis(50); }
  // Threshold (normally >= max_dwell()) beyond which `stuck_warning_due()`
  // starts reporting true, e.g. to drive a one-shot diagnostic log line.
  static constexpr reloco::duration stuck_warning_threshold() noexcept {
    return reloco::duration::from_millis(500);
  }
};
```

## Usage

One `sched_world_handoff` instance per CPU, constructed with a
reference to that CPU's dedicated world-thread `Entry` (which must
outlive it) -- `Sched` itself is a type parameter, matching
`sched.hpp`'s own stateless, all-`static`-method policies, so no
`Sched` instance is needed:

```cpp
// Call sites, alongside the CPU's ordinary scheduler-dispatch code:

// The kernel's own cross-world entry glue, right after this CPU has
// started (or resumed) executing the other world:
handoff.on_world_entered(now, structo::world_entry_kind::cooperative);

// ...and right before actually returning control to the other world
// (the matching exit of the above):
handoff.on_world_exited(now);

// Anywhere that discovers the other world must be given the CPU
// back (e.g. an IRQ routed to it fires while this kernel is running):
handoff.request_force_switch(now);

// In the dispatch loop, before/instead of an ordinary `pick_next()`,
// once this CPU is about to run the other world cooperatively:
if (handoff.should_switch_to_world(now)) {
  sched.force_next(world_thread); // idempotent if already pinned
}

// Whenever the dispatch loop calls `sched.on_block(entry, now)` for
// any entry, also tell the handoff object (cheap, no-op unless
// `entry` happened to be an outstanding resume hint -- see
// `on_world_entered`'s `resume_hint` parameter):
handoff.on_block(entry);

// Whenever the dispatch loop calls `sched.on_wake(entry, now)` for
// any entry, also tell the handoff object (cheap, no-op unless
// `entry` is this CPU's own world thread with a switch still owed):
handoff.on_wake(entry);
```

## API

- `on_world_entered(now, kind, resume_hint = nullptr)` -- records that
  this CPU has just started (or resumed) executing the other world;
  clears any previously outstanding `force_switch_needed()` and starts
  dwell-time tracking from `now`. If `resume_hint` is non-null (e.g.
  the task that was running when the other world was last forced to
  give up the CPU), it is forced into place immediately via
  `Sched::force_next`.
- `on_world_exited(now)` -- records that this CPU has just handed
  control back to the other world (the world thread is about to return
  there). Pure bookkeeping: no `force_next` call is needed here, since
  by construction the only way back into the other world is through the
  world thread itself having already been dispatched. At most clears
  `force_switch_needed()` and stops dwell-time tracking.
- `on_block(entry)` -- tell the handoff object that `entry` has just
  blocked. A no-op unless `entry` is the `resume_hint` most recently
  passed to `on_world_entered` and it blocked before ever being
  dispatched -- in which case the stale hint is released, so a later,
  unrelated wake of `entry` does not re-force it into place.
- `on_wake(entry)` -- tell the handoff object that `entry` has just
  become runnable again. A no-op unless `entry` is this CPU's own world
  thread *and* a switch is still owed (`force_switch_needed()`) -- in
  which case the world thread is re-forced into place, since the world
  thread blocking partway through a forced return (e.g. needing a lock)
  must not cost it its "runs next" guarantee once it wakes back up.
- `request_force_switch(now)` -- marks that the other world must be
  given the CPU back as soon as possible and immediately forces the
  world thread to be dispatched next, regardless of whatever else is
  runnable or how it compares in priority. Idempotent.
- `should_switch_to_world(now)` -- whether the world thread should be
  dispatched next, right now: true if `request_force_switch` was called
  and not yet satisfied by a matching `on_world_exited`, or if the other
  world has been running cooperatively for at least `Traits::max_dwell()`
  without yielding on its own (a pseudo-IRQ-style forced reentry
  budget). The latter case also marks the switch as needed and forces
  the world thread into place, same as `request_force_switch` -- so a
  caller need only check this return value, not separately force
  anything. Always false while `in_world()` is false.
- `stuck_warning_due(now)` -- whether the other world has now been
  executing on this CPU for at least `Traits::stuck_warning_threshold()`
  without returning -- intended to drive a one-shot diagnostic warning,
  not a hard failure: unlike `should_switch_to_world`, this performs no
  forcing of its own. Always false while `in_world()` is false.
- `in_world()`, `entry_kind()`, `force_switch_needed()`, `dwell_time(now)`
  -- plain accessors for the tracked state.

## Why no `force_next` call is needed inside `on_world_exited`

Switching to the other world always goes through the world thread
itself -- there is no other path back. By the time `on_world_exited` is
called, the world thread has therefore already been dispatched (its
`force_next` pin, if any, already consumed by `pick_next`); there is
nothing left to force. `on_world_exited` is pure bookkeeping: clearing
`force_switch_needed()` and resetting dwell tracking.

## Why `on_block`/`on_wake` exist at all

Both the world thread (forced back to the other world) and a
`resume_hint` (forced into the other world) can legitimately block
*before* actually running -- e.g. needing a lock along the forced-return
path. `sched.hpp`'s own `force_next`/`on_block` already keep its
internal pin consistent (see [`sched.md`](sched.md#force_next-bypassing-priority-order-entirely-for-one-dispatch)),
but `sched_world_handoff` keeps its own separate bookkeeping
(`resume_hint`, `force_switch_needed()`) that must be kept in sync at
the same two checkpoints:

- A blocked `resume_hint` must not be force-dispatched again on an
  unrelated later wake -- `on_block` releases it.
- A blocked world thread, if a switch is still owed, *must* regain its
  "runs next" guarantee once it wakes back up -- `on_wake` re-applies
  `force_next` for it in that case.
