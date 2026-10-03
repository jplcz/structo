<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `kernel_spin_lock<Traits>`

`include/structo/sync/kernel_spin_lock.hpp`

A busy-wait lock that records *which* owner holds it (not merely
*whether* it is held), backed by [`backoff`](backoff.md) instead of a
bare `spin_loop()` retry -- the same spirit as `reloco::spin_lock`, but
for kernel code that already has a notion of "the current thread/CPU" to
stamp the lock with:

```cpp
struct kernel_lock_traits {
  using owner_type = thread *;

  static owner_type current_owner() noexcept { return get_current_thread(); }
};

structo::sync::kernel_spin_lock<kernel_lock_traits> lock;
lock.lock();
// ... protected section ...
lock.unlock();
```

`Traits` supplies:

- `owner_type` -- any type convertible to `std::uintptr_t` and back
  (typically a pointer to the current thread/task control block, or a
  small integer CPU/thread id); must never legitimately be `0`, since
  `0` is this lock's reserved "unlocked" sentinel.
- `static owner_type current_owner() noexcept;` -- answers "what should
  the owner slot read as if the calling context locked this right now?"

- `lock()` -- spins (with `backoff`-throttled polling) until acquired,
  then stamps the owner slot with `Traits::current_owner()`. Traps
  (`RELOCO_ASSERT`) if the calling context already owns the lock: this
  is a non-recursive lock, so that would otherwise self-deadlock.
- `try_lock()` -- attempts to acquire without spinning; returns whether
  it succeeded.
- `unlock()` -- releases the lock. Traps (`RELOCO_ASSERT`) if the
  calling context is not the current owner (double-unlock, or unlock
  from the wrong context).
- `is_locked()` -- best-effort, racy snapshot of whether the lock is
  held by anyone.
- `is_locked_by_current()` -- whether the calling context currently
  holds this lock; useful for `RELOCO_ASSERT`-style "caller must already
  hold this lock" preconditions on internal helpers.
- Destructor traps (`RELOCO_ASSERT`) if the lock is still held --
  destroying a held lock means either a leaked `unlock()` or another
  context still spinning against memory about to disappear.

`lock()`'s contended spin is also wired into a
[`softlock_detector`](softlock_detector.md): a true deadlock (the owner
never releases) traps instead of spinning the calling core forever with
no diagnostic. The tick limit defaults to
`softlock_detector::default_limit()` (itself overridable process/kernel-
wide via `softlock_detector::set_default_limit()`); supply an optional
`static constexpr softlock_detector::counter_type softlock_limit` member
on `Traits` to override it for a specific lock policy instead.

Like `reloco::spin_lock`, never fair and never adaptive: a contended
`lock()` spins forever rather than parking or falling back to a
blocking wait. Only appropriate where spinning is known to be short --
IRQ/exception handlers, pre-scheduler-init code, data shared with an
interrupt handler on another core. See
[`ticket_spin_lock<Traits>`](ticket_spin_lock.md) for a FIFO-fair
alternative sharing the same `Traits` shape, trading a little extra
overhead for immunity to starvation under heavy, sustained contention.

See also: [`backoff.md`](backoff.md), [`softlock_detector.md`](softlock_detector.md), [`ticket_spin_lock.md`](ticket_spin_lock.md), [`irq_guard.md`](irq_guard.md).
