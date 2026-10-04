<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::noop_sched` / `fixed_priority_sched` / `edf_sched` / `sched_ule` / `sched_4bsd`

`include/structo/sched.hpp`

Five stateless, tickless scheduling policies built directly on top of
[`runqueue.hpp`](runqueue.md)'s intrusive queues. Every policy here is a
class template with **no data members** -- every method is `static` and
resolves "this CPU's scheduler state" through a caller-supplied
`PerCpu` trait, and every per-task field a policy needs lives directly
inside the caller-owned `Entry`, exactly mirroring `runqueue.hpp`'s own
`Hook`/`Priority`-as-pointer-to-member convention.

## Tickless: no periodic tick counting, anywhere

Every policy is purely *event-driven*. A scheduling decision happens
only in response to an explicit call -- `enqueue`, `pick_next`,
`on_yield`, `on_block`, `on_wake` -- never a periodic hardware timer
interrupt counting elapsed "ticks". Where a policy needs real elapsed
time (`sched_ule`'s interactivity score, `sched_4bsd`'s CPU-usage
decay), the caller passes an explicit `reloco::instant now` at each such
call, and the policy computes elapsed duration lazily from
`now - some_earlier_instant` -- never by sampling a running counter on
a fixed period. `noop_sched`/`fixed_priority_sched` need no time concept
at all: round-robin quantum expiry is pushed entirely to the caller
(arm a one-shot [`structo::callout`](callout.md) for `now + quantum`,
call `requeue()` if it fires while the task is still runnable).

## One uniform interface across all five, so they're interchangeable

Every policy exposes the exact same ten method names and signatures --
`enqueue(entry, now)`, `pick_next(now)`, `on_yield(entry, now)`,
`on_block(entry, now)`, `on_wake(entry, now)`, `requeue(entry, now)`,
`remove(entry)`, `force_next(entry)`, `is_linked(entry)`, `empty()`,
`size()` -- so generic/templated caller code (a test harness driving
"whichever policy this instantiation picked" through one fixed call
sequence, in particular) never has to branch on *which* of the five is
bound. `noop_sched`/`fixed_priority_sched`/`edf_sched` accept `now` as a
defaulted, unused parameter on every method that takes it (they have no
clock/duration logic of their own) purely for this signature parity --
it's discarded, never read. Where a policy has nothing extra to compute
for an operation (e.g. `on_block` for the three clock-free policies:
`pick_next` already removed the entry from the queue, so there's
nothing to track until the matching `on_wake`/`enqueue`), that method is
a documented no-op or a plain alias for whichever other method does the
equivalent work (e.g. `requeue` for `sched_ule`/`sched_4bsd` is exactly
`on_yield`: both mean "this still-runnable entry needs to be re-scored
and put back", whether that happened voluntarily or because a
round-robin quantum expired). `sched_4bsd::set_nice` is the one
deliberate exception to full parity -- no other policy has any notion
of a caller-adjustable "niceness", so faking a no-op `set_nice`
elsewhere would silently discard a caller's intent rather than
genuinely support it.

## `force_next`: bypassing priority order entirely for one dispatch

All five policies also support one additional, deliberately
out-of-band operation: `force_next(entry)` unlinks `entry` from
wherever it currently sits (if linked at all) and pins it in a
dedicated one-entry slot that the very next `pick_next` call always
checks *first*, before any of the policy's own ordering -- i.e. "this
exact task must run next, no matter what else is runnable or how it
compares in priority/deadline/interactivity score". This exists for
protocols that need to hand a CPU to one specific task immediately and
unconditionally -- e.g. `sched_world_handoff.hpp`'s cross-world
handoff, where an interrupt needing urgent routing elsewhere must make
a dedicated "world thread" entry win over *any* locally runnable task,
regardless of that task's priority. Like `remove`, `force_next` takes
no `now` -- it is a purely structural operation, never involving a
policy's clock-driven recompute logic.

Two things to know before reaching for it:

- **At most one entry can be pinned at a time.** Calling `force_next`
  again before the previous pin is consumed by `pick_next` re-enqueues
  the previous one through the policy's normal path first, so it is
  never silently lost -- but it does lose its "runs next" guarantee at
  that point.
- **`force_next` accepts an already-blocked (unlinked) entry**, not
  just a currently-queued one -- useful for resuming an entry that was
  deliberately `on_block`ed so this one could be forced into its
  place. This is only behaviorally safe when that specific block was
  arranged as part of the same handoff protocol; forcing an entry
  that's blocked for an unrelated reason (a lock, I/O, a condition
  variable, ...) would dispatch it before whatever it is actually
  waiting for has happened -- `force_next` has no way to tell the
  difference, so this is the caller protocol's responsibility.
  Symmetrically, if a pinned-but-not-yet-dispatched entry is then
  independently `on_block`ed (blocking before `pick_next` ever
  consumed its pin), the stale pin is automatically cleared so
  `pick_next` doesn't later hand out an entry the caller now considers
  blocked.

## `Entry` owns its own per-task scheduling state

Like `runqueue.hpp`, these policies are intrusive and non-owning:
`Entry` is caller-owned storage that must outlive every operation
referencing it. Every per-task field a policy needs -- the intrusive
link, priority, and (for `sched_ule`/`sched_4bsd`) accumulated run-time/
sleep-time/decay bookkeeping -- lives directly inside `Entry`, named via
pointer-to-member non-type template parameters: `Hook`/`Priority`
continue `runqueue.hpp`'s convention; `State` is new here, naming a
field of type `ule_task_state` (for `sched_ule`) or `bsd_task_state`
(for `sched_4bsd`) that the policy maintains entirely on its own --
treat it as opaque storage, not a caller-writable API (`bsd_task_state`
is the one exception: its `nice` field is meant to be set by the
caller, via `sched_4bsd::set_nice`).

## Per-CPU global data: the `PerCpu` trait

None of these policies hold a runqueue (or any other per-CPU state) as
a data member. Every method resolves it through a caller-supplied
`PerCpu` template parameter, which must provide:

```cpp
static state_type *get() noexcept; // current CPU's scheduler state blob
```

where `state_type` is the policy's own per-CPU state-blob type (see
below). [`structo::arch::per_cpu_ptr<Tag, state_type>`](per_cpu_ptr.md)
*already* implements exactly this contract (plus `get(cpu)`/
`set(cpu, ptr)` for explicit-CPU access during per-CPU bring-up), so it
is the natural, zero-glue-code choice for `PerCpu` -- see the worked
example below.

Each policy's `state_type` is also defined as a free, namespace-scope
type independent of any particular `PerCpu`/scheduler specialization
(`noop_sched_state<Entry, Hook>`, `fixed_priority_sched_state<Entry,
Hook, Priority, NumPriorities>`, `sched_ule_state<Entry, Hook, Priority,
NumPriorities>`, `sched_4bsd_state<Entry, Hook, Priority,
NumPriorities>`), precisely so a caller can build
`PerCpu = per_cpu_ptr<MyTag, some_sched_state<...>>` *before* naming
the full scheduler specialization that uses it (the scheduler's own
nested `state_type` alias simply points back to the same free type).

```cpp
struct my_task {
  struct { my_task *next = nullptr; my_task **prev = nullptr; } link;
  unsigned priority = 0;
};

using state_type = structo::fixed_priority_sched_state<my_task, &my_task::link, &my_task::priority, 32>;

struct my_sched_percpu_tag {
  static inline constexpr std::size_t max_cpus = 64;
  static inline void *slots[max_cpus]{nullptr};
  static void *get_ptr(std::size_t cpu) noexcept { return slots[cpu]; }
  static void set_ptr(std::size_t cpu, void *ptr) noexcept { slots[cpu] = ptr; }
  static std::size_t current() noexcept { return hw_current_cpu_index(); }
};
using my_percpu = structo::arch::per_cpu_ptr<my_sched_percpu_tag, state_type>;
using my_sched = structo::fixed_priority_sched<my_task, &my_task::link, &my_task::priority, 32, my_percpu>;

// Once per CPU, during that CPU's own bring-up:
static state_type g_rq_storage[my_percpu::max_cpus];
my_percpu::set(this_cpu, &g_rq_storage[this_cpu]);

// Thereafter, from any context running on that CPU:
my_sched::enqueue(some_task);
my_task *next = my_sched::pick_next();
```

## The five policies

| Policy | Built on | Priority model | Time needed? |
|---|---|---|---|
| `noop_sched<Entry, Hook, PerCpu>` | `fifo_runqueue` | none | no |
| `fixed_priority_sched<Entry, Hook, Priority, NumPriorities, PerCpu>` | `priority_bucket_runqueue` | static, caller-set | no |
| `edf_sched<Entry, Hook, Deadline, PerCpu>` | `priority_list_runqueue` | caller-set absolute deadline (`reloco::instant`) | no (compares only, never reads a clock) |
| `sched_ule<Entry, Hook, Priority, State, NumPriorities, PerCpu>` | two `priority_bucket_runqueue`s | recomputed from run/sleep history | yes |
| `sched_4bsd<Entry, Hook, Priority, State, NumPriorities, PerCpu>` | `priority_bucket_runqueue` | recomputed from decayed CPU usage + `nice` | yes |

### `noop_sched`

Plain FIFO, no priority concept whatsoever -- the trivial,
always-correct baseline a kernel boots with before a real policy is
installed, or the entirety of what a single-priority cooperative kernel
needs. `enqueue`/`pick_next`/`remove`/`is_linked`/`empty`/`size`, all
one-line forwards to the underlying `fifo_runqueue`.

### `fixed_priority_sched`

Static priorities -- `Entry.*Priority` is set once by the caller and
never recomputed by this scheduler. `enqueue`/`pick_next`/`remove` give
POSIX `SCHED_FIFO`-like behavior (same-priority tasks run to completion/
block in FIFO order); call `requeue()` instead of `remove()` when an
externally-armed, tickless round-robin quantum (a one-shot `callout`
the caller arms for `now + quantum` at dispatch time) expires while the
task is still runnable, for `SCHED_RR`-like behavior -- this scheduler
itself has no clock/duration logic at all, keeping it genuinely
tickless by delegating all timing to the caller.

### `edf_sched` (Earliest Deadline First)

The classic, uniprocessor-optimal (Liu & Layland, 1973) dynamic-priority
real-time policy: always dispatches whichever runnable task has the
soonest absolute `reloco::instant` deadline. `Entry.*Deadline` is plain
caller-owned storage -- the caller sets it (typically
`now + relative_deadline`) before each `enqueue`; this scheduler never
reads a clock or recomputes a deadline itself, it only ever *compares*
two already-computed instants via `priority_list_runqueue`'s ordinary
`operator<`-ordered insertion, so it stays genuinely tickless despite
ordering by time. It is EDF's bare dispatch rule only: no admission
control/schedulability check is performed, so avoiding an overloaded
task set (one that would miss deadlines under *any* policy) is the
caller's responsibility, same as real EDF implementations (e.g. Linux's
`SCHED_DEADLINE`) layer admission control on top of, not inside, the
dispatch rule itself.

### `sched_ule` (simplified FreeBSD `SCHED_ULE`)

Two `priority_bucket_runqueue`s per CPU, `curr`/`next`, bundled in
`sched_ule_state`: `pick_next` always dispatches from `curr`, swapping
`curr`/`next` once `curr` empties -- ULE's hallmark two-queue batching,
so a burst of newly-`enqueue`d/just-woken tasks (landing in `next`)
never starves whatever's already running out of `curr`. A per-task
`ule_task_state` (named via `State`) tracks accumulated run-time and
sleep-time (reset/halved periodically as their sum grows, to keep
history recency-weighted without a periodic tick); every time a task is
requeued (`on_yield`/`on_wake`, or initial `enqueue`) its interactivity
score, `sleep_micros * 100 / (run_micros + sleep_micros)` (100 for a
brand-new task), is recomputed and compared against a threshold (30):
scoring at or above it lands the task in a reserved low-numbered
("interactive") priority sub-range scaled by score; below it, a single
shared "batch" priority level.

**What's simplified relative to real `SCHED_ULE`:** the interactivity
score is this one ratio, not `SCHED_ULE`'s exact piecewise formula; the
"batch" range is a single shared FIFO level rather than a further
CPU-usage-ranked sub-range; no SMP load balancing/migration.

### `sched_4bsd` (simplified FreeBSD `SCHED_4BSD`)

A single per-CPU `priority_bucket_runqueue`. A per-task `bsd_task_state`
(named via `State`) tracks an accumulated CPU-usage estimate (`estcpu`)
and a caller-settable `nice` (set via `sched_4bsd::set_nice`, like
`setpriority(2)`); priority is recomputed on every `enqueue`/
`on_yield`/`on_wake` as `priority = base + estcpu/scale + 2*nice`
(clamped to `[0, NumPriorities)`), matching 4BSD's classic shape. Real
4BSD halves `estcpu` on a periodic ~1s tick (`schedcpu()`); this header
instead decays it lazily and tickless-ly, by right-shifting once per
elapsed *real* decay-half-life of wall-clock time since the last
recompute (`now - state.last_decay`, never a counted tick) -- an
integer approximation of the same geometric decay, triggered only when
the task is next touched.

**What's simplified relative to real `SCHED_4BSD`:** the decay is an
integer power-of-two approximation of 4BSD's fixed-point decay
constant, not a verbatim port; no SMP load balancing/migration, no
priority-inversion/priority-propagation handling.

## Operations shared by all five

- `enqueue(entry, now)` -- makes `entry` runnable. `now` is accepted but
  unused by `noop_sched`/`fixed_priority_sched`/`edf_sched` (defaulted,
  for signature parity -- see "One uniform interface" above).
- `pick_next(now)` -- selects and removes the next task to run, or
  `nullptr` if none is runnable.
- `on_yield(entry, now)` -- a dispatched `entry` voluntarily gives up
  the CPU but stays runnable.
- `on_block(entry, now)` -- a dispatched `entry` blocks (becomes
  non-runnable); no-op for the three clock-free policies.
- `on_wake(entry, now)` -- a previously-`on_block`ed `entry` becomes
  runnable again.
- `requeue(entry, now)` -- re-enqueues a just-dispatched `entry` that's
  still runnable, for round-robin-style quantum expiry (same operation
  as `on_yield` for `sched_ule`/`sched_4bsd`).
- `remove(entry)` -- removes `entry`. Precondition: `is_linked(entry)`.
- `force_next(entry)` -- pins `entry` so the very next `pick_next` call
  returns it first, bypassing all normal ordering (see "`force_next`:
  bypassing priority order entirely for one dispatch" above). Also
  accepts an already-blocked `entry`, under the caller protocol's own
  responsibility.
- `is_linked(entry)` -- O(1), whether `entry` is currently enqueued (on
  any CPU).
- `empty()`, `size()`.

`sched_4bsd` additionally has `set_nice(entry, nice)` -- the one
operation with no equivalent on the other four, since none of them have
any notion of caller-adjustable "niceness".

## Putting it together: SMP load balancing via `work_steal.hpp`

"No SMP load balancing/migration" above describes what *this* header
does on its own -- a single CPU's dispatch rule. Multi-CPU balancing is
layered entirely on top, out of three other, independently usable
pieces: [`cpu_load`](cpu_load.md) (per-CPU runnable-task tracking),
[`arch::cpu_sibling_map`](cpu_sibling_map.md) (precomputed,
distance-ordered steal candidates), and [`find_steal_candidate` plus a
steal policy](work_steal.md) (the *decision* of which CPU to steal
from) -- this header has no opinion on any of it, exactly like it has
no opinion on locking. The sketch below is pseudocode (it elides real
per-CPU storage/locking/IPI wake-up plumbing, which is entirely
OS-specific), showing where each piece plugs into one CPU's
idle-vs-dispatch path:

```cpp
// Per-CPU state this fake OS already maintains elsewhere:
//   my_sched              -- e.g. structo::fixed_priority_sched<...> for this CPU
//   percpu_load[cpu]      -- structo::cpu_load<...>, updated on every enqueue/remove
//   siblings[cpu]         -- structo::arch::cpu_sibling_map<...>, built once at boot
//   online                -- structo::arch::cpu_online_dispatcher, hotplug-maintained

void scheduler_tick(std::size_t this_cpu, instant now) {
  if (Entry *next = my_sched::pick_next(now)) {
    dispatch(next); // local work available -- no need to even look at other CPUs
    return;
  }

  // Local runqueue is empty: this CPU is about to idle. Worth a last-resort
  // steal before halting -- `local_load == 0` here trivially satisfies every
  // built-in policy's `should_attempt_steal`, so this call is never skipped.
  auto eligible = online.snapshot(); // arch::cpu_mask<Tag, MaxCpus>
  for (int attempt = 0; attempt < max_steal_attempts; ++attempt) {
    auto victim = structo::find_steal_candidate<structo::performance_steal_policy>(
        siblings[this_cpu], this_cpu, eligible,
        [&](std::size_t cpu) { return percpu_load[cpu].current(); });
    if (!victim.has_value())
      break; // nobody (left) worth stealing from this round -- genuinely idle

    // Caller owns all locking: lock *victim's remote runqueue, re-check it
    // isn't empty (it may have changed since the decision above), pop one
    // task, unlock, then enqueue it into my_sched on this_cpu.
    if (Entry *stolen = try_steal_one_task_from(*victim)) {
      my_sched::enqueue(*stolen, now);
      dispatch(my_sched::pick_next(now));
      return;
    }
    eligible.clear(*victim); // that one didn't pan out -- don't reconsider it this round
    if (eligible.none())
      eligible = online.snapshot(); // every candidate excluded -- refresh for hotplug changes
  }
  halt_until_next_interrupt(); // truly nothing runnable anywhere reachable
}
```

Swapping `performance_steal_policy` for `power_save_steal_policy`/
`always_steal_policy` (see [`work_steal.md`](work_steal.md)) changes
nothing else about this loop -- the policy alone decides whether/which
candidate qualifies; `scheduler_tick` itself never changes based on
which OS power/performance policy is configured.

See [`runqueue.md`](runqueue.md) for the three underlying runqueue
policies these schedulers are built on, [`callout.md`](callout.md) for
the tickless round-robin/sleep-timeout timer these schedulers expect
the caller to drive them with, and
[`per_cpu_ptr.md`](per_cpu_ptr.md)/`arch/per_cpu_ptr.hpp` for the
storage-free per-CPU resolver `PerCpu` is designed to reuse directly.
