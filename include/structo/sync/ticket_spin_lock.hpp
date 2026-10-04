// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ticket_spin_lock.hpp
 * @brief `structo::sync::ticket_spin_lock<Traits>`: a FIFO-fair busy-wait
 * lock -- "take a ticket, spin until it's your number" -- in the same
 * spirit as `kernel_spin_lock<Traits>` (owner tracking, `backoff`,
 * `softlock_detector` wiring, `Traits` shape), but trading its bare
 * test-and-test-and-set `lock()` for strict first-come-first-served
 * acquisition order.
 *
 * ## Why fairness, and why it costs something
 *
 * `kernel_spin_lock`/`reloco::spin_lock` are both a simple compare-
 * exchange race: whichever contender's `compare_exchange` happens to
 * land first wins, with no memory of who asked first. Under heavy,
 * sustained contention that can let one hot core keep re-winning the
 * race indefinitely while a less "lucky" one (often simply the one
 * physically farther from the cache line, or scheduled back in after a
 * longer pause) starves -- acceptable for a lock held briefly and
 * rarely contended, but a real correctness/fairness risk for one many
 * cores hammer constantly (e.g. a global runqueue lock). A ticket lock
 * removes that risk entirely, at the cost of one extra atomic
 * fetch-add per `lock()` and a second cache line (`now_serving_`) every
 * waiter polls -- the same trade-off Linux's own `ticket_spinlock_t`
 * (the pre-`qspinlock` default) made.
 *
 * Two monotonically increasing counters implement it: `next_ticket_`
 * (the next number `lock()` will hand out) and `now_serving_` (the
 * number currently allowed to proceed). `lock()` atomically takes the
 * next ticket, then spins until `now_serving_` reaches it; `unlock()`
 * advances `now_serving_` by one, releasing exactly the next-in-line
 * waiter. Like a real deli/bakery counter, a ticket numerically between
 * two already-served numbers can never jump the queue.
 *
 * `Traits` supplies the exact same two hooks as `kernel_spin_lock`'s own
 * `Traits` (and an optional `softlock_limit`, also matching
 * `kernel_spin_lock`) -- see that header for the full shape; both lock
 * types can share one `Traits` policy.
 *
 * Example:
 * @code
 * structo::sync::ticket_spin_lock<kernel_lock_traits> lock;
 * lock.lock();
 * // ... protected section, released strictly in arrival order ...
 * lock.unlock();
 * @endcode
 *
 * `Traits` may additionally supply an optional `name(self)` and/or
 * `panic(reason, name)` hook for richer trap diagnostics -- see
 * `lock_diagnostics.hpp` (shared by every lock type in this family).
 *
 * Like `kernel_spin_lock`, never appropriate outside contexts where
 * spinning is known to be short (IRQ/exception handlers,
 * pre-scheduler-init code, data shared with an interrupt handler on
 * another core) -- a waiter here can never even attempt to jump ahead
 * of an earlier one that is itself stalled, so an unexpectedly long
 * critical section delays strictly more waiters than the unfair
 * `kernel_spin_lock` would.
 */

#include <structo/sync/backoff.hpp>
#include <structo/sync/lock_diagnostics.hpp>
#include <structo/sync/softlock_detector.hpp>

#include <reloco/detail/assert.hpp>

#include <atomic>
#include <cstdint>

namespace structo::sync {

/**
 * @brief FIFO-fair busy-wait lock; see this file's top-level docs.
 * @tparam Traits Kernel policy providing `owner_type` and
 * `current_owner()`, matching `kernel_spin_lock::Traits`.
 */
template <typename Traits> class ticket_spin_lock {
public:
  using traits_type = Traits;
  using owner_type = typename Traits::owner_type;
  using ticket_type = std::uint64_t;

  constexpr ticket_spin_lock() noexcept = default;

  /**
   * @brief Traps (via `RELOCO_ASSERT`) if the lock is still held or a
   * waiter is still queued for it.
   *
   * Mirrors `kernel_spin_lock`'s own destructor: either its holder
   * leaked it, or another context is still spinning against memory
   * about to disappear -- both worth trapping on immediately.
   */
  ~ticket_spin_lock() noexcept {
    STRUCTO_SYNC_LOCK_ASSERT(
        Traits, *this, now_serving_.load(std::memory_order_relaxed) == next_ticket_.load(std::memory_order_relaxed),
        "ticket_spin_lock: destroyed while still held or while a waiter is queued");
  }

  ticket_spin_lock(const ticket_spin_lock &) = delete;
  ticket_spin_lock &operator=(const ticket_spin_lock &) = delete;

  /**
   * @brief Takes the next ticket, then spins (with exponential backoff)
   * until it is the one being served.
   *
   * Traps (via `RELOCO_ASSERT`) if the calling context already owns
   * this lock: this is a non-recursive lock, so queueing a second
   * ticket behind one's own would otherwise self-deadlock forever.
   */
  void lock() & noexcept {
    STRUCTO_SYNC_LOCK_ASSERT(
        Traits, *this, !is_locked_by_current(),
        "ticket_spin_lock: lock() called while already held by the calling context (self-deadlock)");

    const ticket_type my_ticket = next_ticket_.fetch_add(1, std::memory_order_relaxed);

    backoff bo;
    softlock_detector lockup(detail::softlock_limit_for<Traits>::value());
    while (now_serving_.load(std::memory_order_acquire) != my_ticket) {
      lockup.tick();
      bo.spin();
    }

    owner_.store(owner_value(), std::memory_order_relaxed);
  }

  /**
   * @brief Attempts to acquire the lock without spinning; returns
   * whether it succeeded. Only ever succeeds when the lock is free *and*
   * no other waiter is already queued ahead of this attempt -- matching
   * `lock()`'s FIFO guarantee rather than letting `try_lock()` itself
   * jump the queue.
   */
  [[nodiscard]] bool try_lock() & noexcept {
    ticket_type ticket = now_serving_.load(std::memory_order_relaxed);
    if (!next_ticket_.compare_exchange_strong(ticket, ticket + 1, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
      return false;
    }
    owner_.store(owner_value(), std::memory_order_relaxed);
    return true;
  }

  /**
   * @brief Releases a lock held by the calling owner, admitting the
   * next-in-line waiter (if any).
   * Traps (via `RELOCO_ASSERT`) if the calling context is not the
   * current owner (double-unlock, or unlock from the wrong context).
   */
  void unlock() & noexcept {
    STRUCTO_SYNC_LOCK_ASSERT(Traits, *this, owner_.load(std::memory_order_relaxed) == owner_value(),
                             "ticket_spin_lock: unlock() by non-owner (or already unlocked)");
    owner_.store(0, std::memory_order_relaxed);
    now_serving_.fetch_add(1, std::memory_order_release);
  }

  /**
   * @brief Best-effort snapshot of whether the lock is currently held
   * (by anyone) or has a waiter queued. Racy by nature -- useful only
   * for diagnostics/assertions, never for making a synchronization
   * decision.
   */
  [[nodiscard]] bool is_locked() const noexcept {
    return now_serving_.load(std::memory_order_relaxed) != next_ticket_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Whether the calling context currently holds this lock.
   * Useful for `RELOCO_ASSERT`-style "caller must already hold this
   * lock" preconditions on internal helpers.
   */
  [[nodiscard]] bool is_locked_by_current() const noexcept {
    return is_locked() && owner_.load(std::memory_order_relaxed) == owner_value();
  }

private:
  /** @brief `Traits::current_owner()`, converted to the raw `uintptr_t` stamp, asserting it isn't the reserved `0`. */
  [[nodiscard]] static std::uintptr_t owner_value() noexcept {
    const auto value = static_cast<std::uintptr_t>(Traits::current_owner());
    RELOCO_ASSERT(value != 0, "ticket_spin_lock: current_owner() must not report 0 (reserved for \"unlocked\")");
    return value;
  }

  std::atomic<ticket_type> next_ticket_{0};
  std::atomic<ticket_type> now_serving_{0};
  std::atomic<std::uintptr_t> owner_{0};
};

} // namespace structo::sync
