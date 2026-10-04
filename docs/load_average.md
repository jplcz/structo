<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::load_average` / `structo::unix_load_average`

`include/structo/load_average.hpp`

A single, Linux-`calc_load()`-style exponentially-decayed moving average
of an "active count" (runnable tasks, in-flight requests, ...) --
`load_average<Rep, FracBits>` -- and the classic Unix `/proc/loadavg`
triple of 1/5/15-minute averages built on three of them,
`unix_load_average<Rep, FracBits>`. Both are built entirely on
[`reloco::fixed_point`](https://github.com/jplcz/reloco/blob/master/docs/reference.md#fixed_pointrep-fracbits)'s
floating-point-free `exp()` -- in fact, `load_average` is the
originally-motivating use case `reloco`'s `fixed_point.hpp`/
`fixed_int.hpp` were built for.

## Caller-driven (tickless) sampling, not a fixed periodic cadence

Linux's own `calc_load()` samples at a fixed 5-second cadence, which lets
it precompute one constant decay factor per averaging window
(`EXP_1`/`EXP_5`/`EXP_15`) once and reuse it forever. `load_average`
makes no such assumption: `sample(elapsed, active_count)` takes the
actual `reloco::duration` elapsed since the previous sample, and
recomputes `decay = exp(-elapsed/time_constant)` on every call. A caller
on a tickless kernel (dispatched from "the scheduler happened to run",
not a periodic timer interrupt) can sample whenever convenient, at an
irregular cadence, and the decay still comes out correct for however
much (or little) time actually passed.

`load_n = load_0 * decay + active*(1 - decay)` is the classic
exponential-moving-average update; `decay_for()` computes
`decay = 1 / exp(elapsed/time_constant)` (the reciprocal of `exp()` of
the always-non-negative ratio, rather than negating the ratio first) so
that `Rep` never has to be a signed type. Once `elapsed` so far exceeds
`time_constant` that `exp()`'s own repeated-squaring intermediate could
otherwise silently overflow, `decay` is clamped directly to `0` instead
-- the mathematically correct answer anyway (a sample taken long after
the time constant has decayed the previous value to something below any
representable precision).

```cpp
using namespace structo;

load_average<> one_minute(reloco::duration::from_secs(60));
one_minute.sample(reloco::duration::from_secs(5), 3U); // 3 runnable tasks, 5s since the previous sample
auto value = one_minute.value(); // a reloco::fixed_point<std::uint32_t, 11>
std::printf("%u.%02u\n", value.to_int(), value.fractional_percent()); // "/proc/loadavg"-style rendering

unix_load_average<> loadavg;
loadavg.sample(reloco::duration::from_secs(5), 3U); // updates all three (1/5/15-minute) windows at once
auto one = loadavg.one_minute();
auto five = loadavg.five_minute();
auto fifteen = loadavg.fifteen_minute();
```

`Rep` must be no wider than 64 bits (`load_average`'s own precision need
is far smaller than `fixed_point`/`fixed_int`'s general-purpose one;
`decay_for()`'s widened ratio intermediate is a fixed 128-bit
`reloco::fixed_uint<128>`, sized to exactly cover a 64-bit `Rep`) --
construct a plain `reloco::fixed_point<Rep, FracBits>` directly, without
`load_average`, if a wider `Rep` is ever genuinely needed.

## Using this on SMP: one systemwide instance, one serialized sampler

`load_average`/`unix_load_average` are plain value types with no
internal synchronization at all (matching every other stateful,
caller-owned type in this library, e.g. [`sched.hpp`](sched.md)'s
policies) -- `sample()` is not safe to call concurrently from more than
one CPU at once. This is deliberate, not an oversight: exactly like
Linux's own global `avenrun`/`calc_global_load()`, a load average is a
*systemwide* quantity (`/proc/loadavg` reports one number, not one per
CPU), so it should be driven by exactly one logical "sampler", never by
every CPU independently:

- Keep exactly **one** `unix_load_average` (or `load_average`) instance
  for the whole system (e.g. a file-scope/singleton variable, or a
  member of whatever boot-time-initialized global scheduler state
  already exists) -- never one per CPU; a per-CPU instance would each
  decay/track only that CPU's own local count, which is a different
  (also sometimes useful, but not `/proc/loadavg`-equivalent) quantity.
- Feed `sample()` the **sum of every CPU's runnable count**, gathered
  across all online CPUs at the sampling instant (e.g. each CPU's own
  `nr_running`-equivalent reachable through
  [`structo::arch::per_cpu_ptr<Tag, T>`](per_cpu_ptr.md) -- see
  `sched.hpp`'s own docs for that pattern), not any single CPU's own
  count.
- Restrict the actual `sample()` call itself to a single context at a
  time -- the simplest options already in this library are picking one
  fixed CPU to own it (`structo::arch::cpu_index::is_bsp()`-style "only
  the boot CPU calls this"), or guarding the call with a
  [`structo::sync::kernel_spin_lock`](kernel_spin_lock.md)/
  `reloco::spin_lock` if more than one context could plausibly race to
  sample at once (e.g. a periodic callout racing a syscall-driven forced
  recompute). Either way, the cross-CPU *gather* step (summing every
  CPU's count) only needs to be consistent enough for a human-facing
  approximate metric -- exactly like Linux's own `calc_global_load()`,
  which deliberately does not stop every CPU to get a perfectly atomic
  snapshot either.

```cpp
// One systemwide instance (e.g. file-scope, or a field of a global
// scheduler-state singleton) -- never one of these per CPU.
structo::unix_load_average<> g_load_average;

// Called periodically from exactly one context (e.g. only ever from
// the boot CPU's own periodic callout/housekeeping path).
void update_systemwide_load_average(reloco::duration elapsed) {
  std::uint32_t total_runnable = 0;
  for (auto cpu : online_cpus()) // however this kernel enumerates online CPUs
    total_runnable += my_percpu_runqueue[cpu].nr_running(); // e.g. via per_cpu_ptr<Tag, runqueue>
  g_load_average.sample(elapsed, total_runnable);
}
```
