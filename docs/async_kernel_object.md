<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::async_kernel_object`

`include/structo/async_kernel_object.hpp`

A memory-safe, owning building block for *any* kernel object whose
completion is signaled asynchronously -- from an interrupt handler,
another CPU, a deferred-work/softirq runner, a timer wheel's expiry
sweep, or any other execution context outside the arming caller's
control -- rather than being awaited synchronously in the same call
stack that armed it.

This header is deliberately subsystem-agnostic: it says nothing about
*when* `fire()` is called or *why* (a timer becoming due, a work item
being dequeued, a DPC being run, ...), only the one problem every such
subsystem shares and must get right: safely owning a callback that may
be invoked from a racing, uncontrolled context, while still letting the
arming caller cancel it or safely reclaim its memory. A concrete
subsystem (a future FreeBSD-`callout(9)`-like timer-wheel object, a
future workqueue/tasklet-style deferred-work object, ...) is expected to
wrap this type, adding only what it actually needs on top.

## The hazard this exists to prevent

A callback registered with *some* subsystem to be invoked later, from a
context the registering caller does not control, is one of the most
reliable sources of use-after-free bugs in kernel code: the object
holding the callback gets freed while a fire is still in flight on
another core, or a caller re-arms/destroys it believing a prior fire has
already finished when it has not. FreeBSD's `callout(9)` --
`callout_stop()` (non-blocking, "best-effort" cancel) vs.
`callout_drain()` (blocking: wait for any in-flight invocation to
actually finish before returning) -- is the canonical precedent this
header's `cancel()`/`drain()` split mirrors exactly.

## State machine

Every `async_kernel_object` tracks exactly three independent, atomic
bits:

- **active**: armed (`try_submit`) and not yet `deactivate`d/`cancel`ed.
  A `fire()` that observes this bit cleared skips invoking the callback
  entirely, even if it still observed **pending** set.
- **pending**: currently submitted to -- and not yet picked up by --
  whatever concrete subsystem `Traits::submit` enqueued it with.
- **firing**: `fire()` is currently invoking the callback. `drain()`
  spins on this bit clearing to know when it is safe to return (and
  therefore safe for the caller to free memory this object lives in).

## Why not safe to `try_submit()`/`cancel()` concurrently with each other

Exactly like `callout(9)`, this header's atomics make `cancel()`/
`drain()` safe to call concurrently with `fire()` itself, but do **not**
make concurrent `try_submit()`/`cancel()` calls from two different
threads safe against each other without the caller's own external
serialization. Re-arming from *inside* the firing callback itself is
always safe -- only one `fire()` can ever be in-flight for a given
object at a time, by construction.

## Customizing: `Traits`

`Traits` is a plain compile-time policy (matching
`irq_guard<Traits>`/`preemption_guard<Traits>`, not a type-erased `*_ref`
handle) supplying two mandatory member *templates*, templated on the
object type itself rather than fixed to one `Capacity`, so a single
`Traits` specialization works for every `async_kernel_object<Traits, N>`
regardless of `N`:

```cpp
struct my_subsystem_traits {
  template <typename Obj, typename... Args>
  static reloco::result<void> submit(Obj &self, Args &&...args) noexcept;
  template <typename Obj>
  static reloco::result<void> cancel(Obj &self) noexcept;
};
```

- `submit` enqueues `self` into the concrete subsystem's own scheduling
  structure, to be `fire()`d back from whatever context that subsystem
  fires from; `Args...` is whatever that subsystem needs to know (e.g. a
  single `reloco::duration` for a timer).
- `cancel` removes `self` from that structure -- only ever called once
  `async_kernel_object::cancel()` has already confirmed, via its own
  atomic `pending` bit, that `self` genuinely was still sitting there
  un-fired.

Optionally, `Traits` may also supply `hook_type`/`state_type` member
type aliases -- an intrusive link type (e.g. a wheel bucket's
`TAILQ_ENTRY`-equivalent) and/or whatever extra per-object bookkeeping
the subsystem needs. Both default to an empty, zero-size placeholder
type if omitted. Neither is ever exposed to the arming caller: `hook()`/
`state()` are private, reachable only from `Traits`'s own member
templates via `friend Traits`.

## Why owning, unlike `timer_ref`/`uart_ref`/`hw_rng_ref`

Those `hw/` `*_ref` handles are two-word, non-owning *views* over a
caller-owned backend. An asynchronous callback is different: something
must own the callback storage for as long as an in-flight invocation
needs it to remain valid, so `async_kernel_object` owns its callback in
a fixed-`Capacity`, zero-allocation
`reloco::inplace_function<void(async_kernel_object &), Capacity>` --
never a borrowed `reloco::function_ref` (right for
`timer_ref::set_callback`, wrong here: nothing else keeps the referenced
callable alive once `try_submit` returns).

## Operations

- `try_submit(callback, args...)` -- stores `callback`, implicitly
  cancels any previous pending submission, then calls
  `Traits::submit(*this, args...)`; rolls back on failure.
- `cancel()` -- non-blocking; returns `result<bool>`: whether this
  object was actually still pending (a scheduled fire was genuinely
  prevented).
- `drain(max_spins = default_drain_max_spins)` -- `cancel()` then spins
  until any in-flight `fire()` finishes clearing `firing`, or
  `error::timed_out` if `max_spins` is exhausted. Must not be called
  from inside this object's own firing callback.
- `deactivate()` -- clears `active` only; a subsequent `fire()` still
  fires and clears `pending`, but skips invoking the callback.
- `is_pending()`, `is_active()`, `is_firing()`.

`~async_kernel_object()` `RELOCO_ASSERT`s that neither `pending` nor
`firing` is set at destruction time -- callers must `drain()` first.

Non-copyable, non-movable: `Traits::submit`/`cancel` operate on this
object's own address, so its identity must stay fixed for as long as it
may be `pending`/`firing`.

## Example

```cpp
struct fake_subsystem_traits {
  template <typename Obj> static reloco::result<void> submit(Obj &self, int ticks) noexcept {
    // enqueue `&self` into some wheel/queue, to `self.fire()` later ...
    return {};
  }
  template <typename Obj> static reloco::result<void> cancel(Obj &self) noexcept {
    // dequeue `&self` ...
    return {};
  }
};

structo::async_kernel_object<fake_subsystem_traits> obj;
(void)obj.try_submit([](auto &self) noexcept { /* ... */ }, 10);
// ... later, from whatever context the subsystem fires from: obj.fire();
(void)obj.drain(); // before `obj` goes out of scope
```

See also: [`timer_ref.md`](timer_ref.md) (a type-erased, non-owning
hardware timer handle this header is independent of -- a future
callout-like object is expected to be built on top of
`async_kernel_object` and could compose with `timer_ref`-backed
subsystems).
