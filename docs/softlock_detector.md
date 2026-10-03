<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `softlock_detector`

`include/structo/sync/softlock_detector.hpp`

A tiny, stack-only tick counter for spin-wait loops that traps (via
`RELOCO_ASSERT`) once it has been ticked more than some configurable
limit of times without a matching `reset()` -- turning a silent,
infinite soft lockup (a spin loop whose condition will in fact never
become true, e.g. a deadlock or a lost wakeup) into an immediate, loud
failure instead of a hung core that never reports anything:

```cpp
structo::sync::softlock_detector detector;
structo::sync::backoff bo;
while (!condition_met()) {
  detector.tick(); // traps once limit() iterations are recorded
  bo.spin();
}
```

[`kernel_spin_lock<Traits>`](kernel_spin_lock.md) wires one of these
into its own contended `lock()` spin loop automatically, ticking it once
per failed acquisition attempt; supply an optional
`Traits::softlock_limit` to override the default limit for a particular
lock policy.

- `softlock_detector(limit = default_limit())` -- constructs a detector
  tolerating up to `limit` (`std::uint64_t`) `tick()` calls.
- `tick()` -- records one spin-wait iteration; traps once more than
  `limit()` calls have been recorded since construction or the last
  `reset()`.
- `reset()` -- clears the tick count, e.g. once the loop observes
  forward progress (a changed generation counter, ...) and the
  remaining wait should no longer count against the original limit.
- `count()` / `limit()` -- accessors for the current tick count and the
  configured limit.

## The process-wide default limit

A fixed, hardcoded tick limit cannot be "a reasonable number of
iterations" across every target this library runs on: the same spin
loop burns through its iteration budget orders of magnitude faster on a
multi-GHz server core than on a slow embedded one. `default_limit()` /
`set_default_limit()` expose a single process/kernel-wide default
(initially a generous, architecture-agnostic placeholder) a kernel port
is expected to override once, early at boot, with a value derived from
its own measured CPU cycle rate (e.g. `structo::hw::cycles`'s
`clock_cycles.hpp` calibrated frequency times however many seconds of
spinning should count as "stuck") -- the same way a real kernel tunes its own hung-task/
softlockup watchdog threshold to the hardware it is actually running on.

Construct one `softlock_detector` per spin-wait loop (it is non-atomic,
non-shared, exactly one spinning thread/core ever touches one
instance).

See also: [`backoff.md`](backoff.md), [`kernel_spin_lock.md`](kernel_spin_lock.md).
