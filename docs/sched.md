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

- `enqueue(entry[, now])` -- makes `entry` runnable; `sched_ule`/
  `sched_4bsd` take an explicit `instant now`.
- `pick_next([now])` -- selects and removes the next task to run, or
  `nullptr` if none is runnable.
- `remove(entry)` -- removes `entry`. Precondition: `is_linked(entry)`.
- `is_linked(entry)` -- O(1), whether `entry` is currently enqueued (on
  any CPU).
- `empty()`, `size()`.

`fixed_priority_sched` additionally has `requeue(entry)`; `sched_ule`/
`sched_4bsd` additionally have `on_yield(entry, now)`/
`on_block(entry, now)`/`on_wake(entry, now)` (see above); `sched_4bsd`
additionally has `set_nice(entry, nice)`.

See [`runqueue.md`](runqueue.md) for the three underlying runqueue
policies these schedulers are built on, [`callout.md`](callout.md) for
the tickless round-robin/sleep-timeout timer these schedulers expect
the caller to drive them with, and
[`per_cpu_ptr.md`](per_cpu_ptr.md)/`arch/per_cpu_ptr.hpp` for the
storage-free per-CPU resolver `PerCpu` is designed to reuse directly.
