<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::sync` lock diagnostics

`include/structo/sync/lock_diagnostics.hpp`

Shared, optional diagnostic hooks for the spin lock family
([`kernel_spin_lock`](kernel_spin_lock.md),
[`ticket_spin_lock`](ticket_spin_lock.md),
[`queue_spin_lock`](queue_spin_lock.md),
[`rw_spin_lock`](rw_spin_lock.md),
[`queue_rw_spin_lock`](queue_rw_spin_lock.md)). A lock's `Traits` policy
may supply a per-instance *name* and a *panic handler*. Neither is
required: without them each trap is a plain `RELOCO_ASSERT` with a fixed
message, and the whole mechanism folds away under `if constexpr` at no
runtime cost.

This header is mostly for lock implementers and `Traits` authors; the
only public-facing pieces are the two optional `Traits` functions and
the `STRUCTO_SYNC_LOCK_ASSERT` macro used inside lock implementations.

## Usage

```cpp
struct kernel_lock_traits {
  // Normal lock policy members (see kernel_spin_lock.md).
  using owner_type = thread *;
  static owner_type current_owner() noexcept { return get_current_thread(); }

  // OPTIONAL. Many locks share this one Traits type (one per run queue,
  // per device, ...), so the name is derived from the lock instance
  // itself: `self` is the lock object (const ref) the trap concerns.
  // Return a string with static/long-enough lifetime, or nullptr.
  template <typename Self> static const char *name(const Self &self) noexcept {
    return lock_registry::lookup_name(&self); // e.g. a registry keyed on the address
  }

  // OPTIONAL. Called instead of the plain RELOCO_ASSERT when a lock
  // invariant is violated. `reason` is the fixed description (e.g.
  // "kernel_spin_lock: destroyed while still held"); `name` is the
  // result of name(self), or nullptr if Traits has no name(). Formatting
  // and logging are up to you. Must not return: the macro executes
  // RELOCO_TRAP() right after as a safety net.
  [[noreturn]] static void panic(const char *reason, const char *name) noexcept {
    kernel_panic("lock '%s': %s", name ? name : "<unnamed>", reason);
  }
};

// Inside a lock implementation: traps unless the condition holds.
// Arguments: the lock's Traits type, the lock instance, the condition
// that must be true, and the fixed failure text.
// STRUCTO_SYNC_LOCK_ASSERT(Traits, *this, !held_, "kernel_spin_lock: destroyed while still held");
```

## API

| Entity | Description |
|---|---|
| `detail::has_lock_name<Traits, Self>` | `true_type` if `Traits::name(const Self&)` is callable. |
| `detail::has_lock_panic<Traits>` | `true_type` if `Traits::panic(const char*, const char*)` is callable. |
| `detail::lock_name<Traits>(self)` | Returns `Traits::name(self)` if provided, else `nullptr`. |
| `STRUCTO_SYNC_LOCK_ASSERT(Traits, self, cond, reason)` | If `Traits::panic` exists and `cond` is false, calls `Traits::panic(reason, lock_name<Traits>(self))` then `RELOCO_TRAP()`; otherwise behaves exactly like `RELOCO_ASSERT(cond, reason)`. |

Everything in `structo::sync::detail` is an implementation detail; only
the macro and the two `Traits` hooks are intended to be relied on.
