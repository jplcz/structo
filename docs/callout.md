<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::callout`

`include/structo/callout.hpp`

A memory-safe, FreeBSD-`callout(9)`-like one-shot/periodic deferred
callback object, built directly on top of
[`structo::async_kernel_object`](async_kernel_object.md). `callout`
itself says nothing about *how* a duration becomes "the callback runs
now": that is entirely the concern of a `Subsystem` -- the **callout
subsystem** customization point, which owns the actual scheduling
policy (a software timer wheel, a single hardware `timer_ref`
multiplexed across every live `callout`, a priority queue keyed by
deadline, ...).

## Relationship to `async_kernel_object`

`callout<Subsystem, Capacity>` is a thin, *composed* wrapper around an
`async_kernel_object<Subsystem, Capacity>` data member -- not a
subclass. `Subsystem::submit`/`cancel` are always instantiated by
`async_kernel_object` with *its own* type as `Obj`, never a derived
type, so composition is the only correct shape here. `callout` narrows
`async_kernel_object::try_submit`'s free-form `Args...` down to exactly
one `reloco::duration`, and renames the generic operations to their
`callout(9)` counterparts:

| `async_kernel_object`  | `callout`       | `callout(9)`          |
|------------------------|-----------------|-----------------------|
| `try_submit(cb, dur)`  | `reset(dur, cb)`| `callout_reset`       |
| `cancel()`             | `stop()`        | `callout_stop`        |
| `drain()`              | `drain()`       | `callout_drain`       |
| `deactivate()`         | `deactivate()`  | `callout_deactivate`  |
| `is_pending()`         | `pending()`     | `callout_pending`     |
| `is_active()`          | `active()`      | `callout_active`      |
| `is_firing()`          | `firing()`      | (no direct equivalent)|

## The callback: `void(callout &)`

The callback given to `reset`/`reset_periodic` is invoked with a
reference to the owning `callout` itself (`void(callout &)`), never the
internal `async_kernel_object` -- callers never see that implementation
detail. `callout` owns a *second*, independently-sized
`inplace_function<void(callout &), Capacity>` for the caller's callback,
and hands the wrapped `async_kernel_object` only a tiny, fixed-size glue
closure capturing nothing but `this` -- so `Capacity` sizes exactly what
the caller's own callback needs to capture, with no hidden overhead.

## Periodic reset: `reset_periodic`

`callout(9)` has no native "periodic" mode -- the idiomatic BSD pattern
is for the callback to call `callout_reset` on itself again as its very
last action. `reset_periodic(period, callback)` is exactly that pattern,
pre-packaged: it re-`reset()`s with the same period and a fresh copy of
`callback` immediately after invoking it, unless the callback itself
called `stop()`/`deactivate()` (checked via `active()` right before the
re-`reset()`). This is safe for the same reason manual self-rearming
from inside `async_kernel_object`'s own firing callback is safe (only
one firing can ever be in flight for a given `callout` at a time), but
`callback` must not read any of its own captured state *after* the
point `reset_periodic` re-arms it -- that storage is reclaimed and
reused for the next invocation as part of the re-arm.

## Customizing: the callout subsystem (`Subsystem`)

`Subsystem` is the exact same compile-time `Traits` policy
`async_kernel_object` itself defines, specialized to a single
`reloco::duration` argument:

```cpp
struct my_callout_subsystem {
  template <typename Obj>
  static reloco::result<void> submit(Obj &self, reloco::duration period) noexcept;
  template <typename Obj>
  static reloco::result<void> cancel(Obj &self) noexcept;
  // optional: using hook_type = ...;    (e.g. a wheel bucket's intrusive link)
  // optional: using state_type = ...;   (e.g. an absolute deadline/generation count)
};
```

`Obj` here is `async_kernel_object<my_callout_subsystem, Capacity>` (the
member `callout` wraps), *not* `callout` itself -- `submit` enqueues
`&self` into whatever concrete wheel/queue structure the subsystem
maintains (using `self.hook()`/`self.state()`, reachable only to
`my_callout_subsystem`'s own member templates via `friend Traits`), to
later call `self.fire()` back once `period` has elapsed, from whatever
context that subsystem fires from. A single `my_callout_subsystem` works
uniformly for every live `callout<my_callout_subsystem, N>` regardless
of `N`.

## Operations

- `reset(duration period, F&& callback)` -- `result<void>`; arms/
  re-arms this `callout`.
- `reset_periodic(duration period, F callback)` -- `result<void>`; arms
  this `callout` to automatically re-`reset()` itself after each fire.
- `stop()` -- `result<bool>`; non-blocking, whether it actually
  prevented a scheduled fire.
- `drain(max_spins = async_object::default_drain_max_spins)` --
  `result<void>`; blocking, safe to free memory afterwards.
- `deactivate()` -- let an already-scheduled fire happen silently,
  without invoking the callback.
- `pending()`, `active()`, `firing()`.

`~callout()` traps (via the wrapped `async_kernel_object`'s own
destructor) if destroyed while still `pending()`/`firing()` -- call
`drain()` first.

Non-copyable, non-movable, exactly like `async_kernel_object`.

## Example

```cpp
struct fake_subsystem {
  template <typename Obj> static reloco::result<void> submit(Obj &self, reloco::duration period) noexcept {
    // enqueue `&self` into some wheel/queue, to `self.fire()` later ...
    return {};
  }
  template <typename Obj> static reloco::result<void> cancel(Obj &self) noexcept {
    // dequeue `&self` ...
    return {};
  }
};

structo::callout<fake_subsystem> co;
(void)co.reset_periodic(reloco::duration::from_millis(10), [](auto &self) noexcept {
  // runs every ~10ms until `self.stop()`/`self.deactivate()` is called
});
// ...
(void)co.drain(); // before `co` goes out of scope
```

See [`examples/callout_scheduler_demo.cpp`](../examples/callout_scheduler_demo.cpp)
for a runnable demo of a toy single-core callout subsystem built on
`reloco::c_tailq` and `hook_type`, [`runqueue.md`](runqueue.md) for
pluggable FIFO/priority-list/priority-bucket runqueue policies a
`Subsystem` could schedule due callouts with instead of a single sorted
list, and [`async_kernel_object.md`](async_kernel_object.md) (the
generic, subsystem-agnostic base this header builds on).
