// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file queue_spin_lock.hpp
 * @brief `structo::sync::queue_spin_lock<Traits>`: an MCS-style queue
 * lock -- the same local-spinning idea behind Linux's `qspinlock` -- in
 * the same `kernel_spin_lock`/`ticket_spin_lock` family (owner tracking,
 * `backoff`, `softlock_detector` wiring, matching `Traits` shape), but
 * fixing the one thing both of those still share: every waiter polling
 * a *single* shared cache line (`owner_`, or `now_serving_`), which
 * itself becomes a bottleneck and a source of cross-core cache traffic
 * as contention grows.
 *
 * ## Why a queue of local spin sites
 *
 * `kernel_spin_lock` has every waiter re-poll one shared atomic;
 * `ticket_spin_lock` fixes *fairness* but still has every waiter re-poll
 * one shared counter. Both mean *N* contending cores all keep bouncing
 * the same cache line between themselves even though only one waiter's
 * read actually matters at any given moment (the one currently first in
 * line). An MCS lock instead has each waiter enqueue a small node --
 * supplied by the caller, typically stack-allocated for the duration of
 * the critical section -- onto a singly linked list via one atomic
 * exchange of a shared tail pointer, then spin only on a `bool` *inside
 * its own node*. `unlock()` hands off by writing directly into the
 * next-in-line node's local flag, so no two cores ever contend the same
 * cache line while waiting. This is the same trick Linux's own
 * `qspinlock` scales to hundreds of cores with (that implementation
 * additionally packs everything into one word and uses small per-CPU
 * node arrays indexed by interrupt-nesting depth to avoid asking the
 * caller for a node explicitly; this one keeps the caller-supplied node
 * instead, trading that transparency for staying free of any per-CPU/
 * nesting-depth infrastructure dependency).
 *
 * `Traits` supplies the exact same two hooks as `kernel_spin_lock`'s own
 * `Traits` (plus the same optional `softlock_limit`) -- all three lock
 * types in this family can share one `Traits` policy.
 *
 * ## Usage
 *
 * Unlike `kernel_spin_lock`/`ticket_spin_lock`, `lock()`/`try_lock()`/
 * `unlock()` take the caller's node explicitly: it must stay alive and
 * untouched for the entire time the lock is held (and while queued
 * waiting for it), so it is almost always a local on the stack frame
 * that also holds the critical section.
 *
 * @code
 * structo::sync::queue_spin_lock<kernel_lock_traits> lock;
 *
 * structo::sync::queue_spin_lock<kernel_lock_traits>::node qnode;
 * lock.lock(qnode);
 * // ... protected section ...
 * lock.unlock(qnode);
 * @endcode
 *
 * `Traits` may additionally supply an optional `name(self)` and/or
 * `panic(reason, name)` hook for richer trap diagnostics -- see
 * `lock_diagnostics.hpp` (shared by every lock type in this family).
 *
 * Like the rest of this family, never appropriate outside contexts
 * where spinning is known to be short (IRQ/exception handlers,
 * pre-scheduler-init code, data shared with an interrupt handler on
 * another core).
 */

#include <structo/sync/backoff.hpp>
#include <structo/sync/lock_diagnostics.hpp>
#include <structo/sync/softlock_detector.hpp>

#include <reloco/detail/assert.hpp>
#include <reloco/hint.hpp>

#include <atomic>
#include <cstdint>

namespace structo::sync {

/**
 * @brief MCS-style queue lock; see this file's top-level docs.
 * @tparam Traits Kernel policy providing `owner_type` and
 * `current_owner()`, matching `kernel_spin_lock::Traits`.
 */
template <typename Traits> class queue_spin_lock {
public:
  using traits_type = Traits;
  using owner_type = typename Traits::owner_type;

  /**
   * @brief Per-waiter queue node. Caller-allocated (typically a local on
   * the stack of whichever function calls `lock()`/`unlock()`); must
   * outlive the time spent queued and holding the lock.
   */
  class node {
  public:
    constexpr node() noexcept = default;

    /**
     * @brief Traps (via `RELOCO_ASSERT`) if destroyed while still linked
     * into a lock's wait queue -- either still waiting, or `unlock()`
     * was never called to unlink it.
     */
    ~node() noexcept {
      RELOCO_ASSERT(!waiting_.load(std::memory_order_relaxed) && next_.load(std::memory_order_relaxed) == nullptr,
                    "queue_spin_lock::node: destroyed while still queued or holding a lock");
    }

    node(const node &) = delete;
    node &operator=(const node &) = delete;

  private:
    friend class queue_spin_lock;

    std::atomic<node *> next_{nullptr};
    std::atomic<bool> waiting_{false};
  };

  constexpr queue_spin_lock() noexcept = default;

  /**
   * @brief Traps (via `RELOCO_ASSERT`) if the lock is still held or a
   * waiter is still queued for it.
   */
  ~queue_spin_lock() noexcept {
    STRUCTO_SYNC_LOCK_ASSERT(Traits, *this, tail_.load(std::memory_order_relaxed) == nullptr,
                              "queue_spin_lock: destroyed while still held or while a waiter is queued");
  }

  queue_spin_lock(const queue_spin_lock &) = delete;
  queue_spin_lock &operator=(const queue_spin_lock &) = delete;

  /**
   * @brief Enqueues `n` onto the wait list, then spins (with exponential
   * backoff) on `n`'s own local flag -- never on any state shared with
   * other waiters -- until handed the lock.
   *
   * Traps (via `RELOCO_ASSERT`) if the calling context already owns
   * this lock: this is a non-recursive lock, so queueing behind one's
   * own already-held lock would otherwise self-deadlock forever.
   */
  void lock(node &n) & noexcept {
    STRUCTO_SYNC_LOCK_ASSERT(Traits, *this, !is_locked_by_current(),
                              "queue_spin_lock: lock() called while already held by the calling context (self-deadlock)");

    n.next_.store(nullptr, std::memory_order_relaxed);
    n.waiting_.store(true, std::memory_order_relaxed);

    node *prev = tail_.exchange(&n, std::memory_order_acq_rel);
    if (prev != nullptr) {
      prev->next_.store(&n, std::memory_order_release);

      backoff bo;
      softlock_detector lockup(detail::softlock_limit_for<Traits>::value());
      while (n.waiting_.load(std::memory_order_acquire)) {
        lockup.tick();
        bo.spin();
      }
    }

    n.waiting_.store(false, std::memory_order_relaxed);
    owner_.store(owner_value(), std::memory_order_relaxed);
  }

  /**
   * @brief Attempts to acquire the lock without spinning; returns
   * whether it succeeded. Only ever succeeds when the queue is
   * genuinely empty (free, with nobody already queued) -- matching
   * `lock()`'s FIFO guarantee rather than letting `try_lock()` itself
   * jump the queue.
   */
  [[nodiscard]] bool try_lock(node &n) & noexcept {
    node *expected = nullptr;
    n.next_.store(nullptr, std::memory_order_relaxed);
    n.waiting_.store(false, std::memory_order_relaxed);
    if (!tail_.compare_exchange_strong(expected, &n, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      return false;
    }
    owner_.store(owner_value(), std::memory_order_relaxed);
    return true;
  }

  /**
   * @brief Releases a lock held by the calling owner via node `n`
   * (the same node instance passed to the matching `lock()`/
   * `try_lock()`), handing off directly to the next-in-line waiter's
   * local flag if one is already linked.
   *
   * Traps (via `RELOCO_ASSERT`) if the calling context is not the
   * current owner (double-unlock, or unlock from the wrong context).
   */
  void unlock(node &n) & noexcept {
    STRUCTO_SYNC_LOCK_ASSERT(Traits, *this, owner_.load(std::memory_order_relaxed) == owner_value(),
                              "queue_spin_lock: unlock() by non-owner (or already unlocked)");
    owner_.store(0, std::memory_order_relaxed);

    node *expected = &n;
    if (tail_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel, std::memory_order_relaxed)) {
      // Nobody was queued behind us: the queue is now empty.
      return;
    }

    // Someone already won the race to become the new tail (via lock()'s
    // exchange()) but may not yet have published its node's address into
    // our `next_` -- a tiny, bounded window inherent to the two-step
    // enqueue (exchange tail, then link predecessor). Spin briefly for it
    // to land; this is never a true deadlock risk (no softlock_detector
    // needed), since whichever core is publishing it is, by construction,
    // not itself blocked on anything.
    node *successor = n.next_.load(std::memory_order_acquire);
    while (successor == nullptr) {
      reloco::hint::spin_loop();
      successor = n.next_.load(std::memory_order_acquire);
    }

    n.next_.store(nullptr, std::memory_order_relaxed);
    successor->waiting_.store(false, std::memory_order_release);
  }

  /**
   * @brief Best-effort snapshot of whether the lock is currently held
   * (by anyone) or has a waiter queued. Racy by nature -- useful only
   * for diagnostics/assertions, never for making a synchronization
   * decision.
   */
  [[nodiscard]] bool is_locked() const noexcept { return tail_.load(std::memory_order_relaxed) != nullptr; }

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
    RELOCO_ASSERT(value != 0, "queue_spin_lock: current_owner() must not report 0 (reserved for \"unlocked\")");
    return value;
  }

  std::atomic<node *> tail_{nullptr};
  std::atomic<std::uintptr_t> owner_{0};
};

} // namespace structo::sync
