<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::cpu_load`

`include/structo/cpu_load.hpp`

A caller-owned, per-CPU instantaneous-and-decayed load tracker --
`cpu_load<Rep, FracBits>` -- meant to be embedded directly inside each
CPU's own scheduler state (e.g. a field of whatever a
[`sched.hpp`](sched.md) policy's `PerCpu::get()` resolves) and fed from
that CPU's own runqueue depth, so a work-stealing/load-balancing
decision has an actual per-core quantity to compare across CPUs.

## The per-CPU sibling of `load_average`, not a replacement for it

`cpu_load` is built on exactly the same tickless,
`reloco::fixed_point`-based exponential decay
[`load_average`](load_average.md) already implements -- it holds one
internally and forwards `sample()`/`average()` straight to it. It is its
own small type, rather than a caller simply instantiating `load_average`
once per CPU directly, because `load_average`'s own docs explicitly warn
against exactly that: one instance per CPU answers a *different*
question than systemwide `/proc/loadavg` does. `cpu_load` exists to
*be* that different, equally legitimate question, under its own name
and its own doc, so a reader is never left wondering which of the two
(deliberately incompatible) usage conventions a given `load_average`-
shaped instance in some kernel's source is actually following.

## Two numbers, two different questions

- **`current()`** -- the exact, un-decayed runnable count as of the most
  recent `sample()` call: "is there actually a task sitting on this
  CPU's queue *right now* to steal?"
- **`average()`** -- the decayed exponential moving average, exactly as
  `load_average::value()` computes it: "has this CPU been *consistently*
  busier than its neighbors, or is `current()`'s nonzero count just a
  transient blip not worth migrating a task over for?"

A work-stealing policy is expected to gate on `current()` (there must be
something to actually steal right now) and rank candidate CPUs (or
decide whether to bother stealing at all, under a power-saving policy)
by `average()`.

## Caller-driven sampling, same as `load_average`

Exactly like `load_average`, this is a plain value type with no internal
synchronization and no notion of "now" -- `sample(elapsed,
runnable_count)` must only ever be called by the one context that owns
this CPU's own scheduler state (i.e. only ever from code running *on*
that CPU itself, e.g. from inside `enqueue`/`pick_next`/an idle-loop
poll), at whatever cadence is convenient. A *different* CPU wanting to
read this tracker's already-sampled `current()`/`average()` values (e.g.
while looking for a steal candidate) only ever reads them, never calls
`sample()` itself -- any synchronization that cross-CPU read needs
against a concurrent local `sample()` is entirely the caller's
responsibility.

```cpp
struct my_percpu_sched_state {
  my_runqueue_type rq;
  structo::cpu_load<> load; // this CPU's own tracker; never shared
  reloco::instant last_sample;
};

// Called from code running on this CPU, whenever convenient (e.g.
// right before pick_next(), or from an idle-loop poll):
void update_load(my_percpu_sched_state &state, reloco::instant now) {
  state.load.sample(now - state.last_sample, state.rq.size());
  state.last_sample = now;
}

// Called from any CPU looking for a steal candidate -- only ever reads:
bool looks_worth_stealing_from(const my_percpu_sched_state &other) {
  return other.load.current() > 0;
}
```

See also: [`load_average.md`](load_average.md), [`sched.md`](sched.md), [`runqueue.md`](runqueue.md), [`cpu_topology.md`](cpu_topology.md).
