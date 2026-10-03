// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file kernel_spin_lock.hpp
 * @brief `structo::sync::kernel_spin_lock<Traits>`: a busy-wait lock that
 * records *which* owner holds it (not merely *whether* it is held),
 * backed by `structo::sync::backoff` instead of a bare `spin_loop()`
 * retry, in the same spirit as `reloco::spin_lock` but for kernel code
 * that already has a notion of "the current thread/CPU" to stamp the
 * lock with.
 *
 * `reloco::spin_lock` (see that header for the full rationale on *why*
 * a spinlock rather than a blocking `mutex` in the first place) is
 * deliberately anonymous: it stores only a `bool`, so it cannot answer
 * "who holds this?" or assert "the caller releasing this must be the
 * one who acquired it" -- useful properties for kernel lock debugging
 * (deadlock/double-unlock detection, lock-order verification, `WITNESS`-
 * style diagnostics) that most real kernels' own spinlocks provide.
 * `kernel_spin_lock<Traits>` adds exactly that: an owner slot, stored as
 * a plain `std::atomic<std::uintptr_t>` (0 means "unlocked"), stamped
 * with whatever `Traits` says the current owner is.
 *
 * `Traits` supplies:
 * - `owner_type` -- any type convertible to `std::uintptr_t` and back
 *   (typically a pointer to the current thread/task control block, or a
 *   small integer CPU/thread id); must never legitimately be `0`, since
 *   `0` is this lock's reserved "unlocked" sentinel.
 * - `static owner_type current_owner() noexcept;` -- answers "what
 *   should the owner slot read as if the calling context locked this
 *   right now?" (e.g. `return current_thread();`).
 *
 * Example `Traits` (current thread's control block pointer as the owner):
 * @code
 * struct kernel_lock_traits {
 *   using owner_type = thread *;
 *
 *   static owner_type current_owner() noexcept {
 *     return get_current_thread();
 *   }
 * };
 *
 * structo::sync::kernel_spin_lock<kernel_lock_traits> lock;
 * lock.lock();
 * // ... protected section ...
 * lock.unlock();
 * @endcode
 *
 * Like `reloco::spin_lock`, never fair and never adaptive: a contended
 * `lock()` spins forever (with `structo::sync::backoff` thinning the
 * polling rate as contention persists) rather than parking or falling
 * back to a blocking wait. Only appropriate where spinning is known to
 * be short -- the same contexts `reloco::spin_lock`'s docs enumerate
 * (IRQ/exception handlers, pre-scheduler-init code, data shared with an
 * interrupt handler on another core).
 *
 * `lock()`'s contended spin is also wired into a
 * `structo::sync::softlock_detector` (see `softlock_detector.hpp`): a
 * true deadlock (the owner never releases) traps instead of spinning
 * the calling core forever with no diagnostic. The tick limit defaults
 * to `softlock_detector::default_limit()` (itself overridable
 * process/kernel-wide via `softlock_detector::set_default_limit()`);
 * supply an optional
 * `static constexpr softlock_detector::counter_type softlock_limit`
 * member on `Traits` to override it for a specific lock policy instead.
 */

#include <structo/sync/backoff.hpp>
#include <structo/sync/softlock_detector.hpp>

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>

#include <atomic>
#include <cstdint>

namespace structo::sync {


/**
 * @brief Busy-wait lock that stamps the lock with whichever owner
 * `Traits::current_owner()` reports, rather than a bare `bool`.
 * @tparam Traits Kernel policy providing `owner_type` and
 * `current_owner()`; see this file's top-level docs.
 */
template <typename Traits> class RELOCO_CAPABILITY("mutex") kernel_spin_lock {
public:
  using traits_type = Traits;
  using owner_type = typename Traits::owner_type;

  constexpr kernel_spin_lock() noexcept = default;

  /**
   * @brief Traps (via `RELOCO_ASSERT`) if the lock is still held.
   *
   * A held lock being destroyed means either its holder leaked it
   * (forgot to `unlock()`) or -- far more dangerously -- another
   * context is still spinning in `lock()` against memory that is about
   * to disappear out from under it. Both are kernel bugs worth trapping
   * on immediately rather than silently freeing a (possibly still
   * contended) lock.
   */
  ~kernel_spin_lock() noexcept {
    RELOCO_ASSERT(owner_.load(std::memory_order_relaxed) == 0, "kernel_spin_lock: destroyed while still held");
  }

  kernel_spin_lock(const kernel_spin_lock &) = delete;
  kernel_spin_lock &operator=(const kernel_spin_lock &) = delete;

  /**
   * @brief Spins (with exponential backoff) until the lock is acquired.
   *
   * Traps (via `RELOCO_ASSERT`) if the calling context already owns
   * this lock: this is a non-recursive lock, so that would otherwise
   * self-deadlock (spin forever against an owner slot only the caller
   * itself can clear).
   */
  void lock() & noexcept RELOCO_ACQUIRE() {
    const std::uintptr_t self = owner_value();
    RELOCO_ASSERT(owner_.load(std::memory_order_relaxed) != self,
                  "kernel_spin_lock: lock() called while already held by the calling context (self-deadlock)");

    backoff bo;
    softlock_detector lockup(detail::softlock_limit_for<Traits>::value());
    for (;;) {
      std::uintptr_t expected = 0;
      if (owner_.compare_exchange_weak(expected, self, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
      }

      // Test-and-test-and-set: retry the cheap relaxed load while
      // contended instead of hammering the exchange itself, which would
      // otherwise force the cache line to bounce between cores on every
      // iteration even though only one of them can ever win it.
      while (owner_.load(std::memory_order_relaxed) != 0) {
        lockup.tick();
        bo.spin();
      }
    }
  }

  /** @brief Attempts to acquire the lock without spinning; returns whether it succeeded. */
  [[nodiscard]] bool try_lock() & noexcept RELOCO_TRY_ACQUIRE(true) {
    std::uintptr_t expected = 0;
    return owner_.compare_exchange_strong(expected, owner_value(), std::memory_order_acquire, std::memory_order_relaxed);
  }

  /**
   * @brief Releases a lock held by the calling owner.
   * Traps (via `RELOCO_ASSERT`) if the calling context is not the
   * current owner (double-unlock, or unlock from the wrong context).
   */
  void unlock() & noexcept RELOCO_RELEASE() {
    RELOCO_ASSERT(owner_.load(std::memory_order_relaxed) == owner_value(),
                  "kernel_spin_lock: unlock() by non-owner (or already unlocked)");
    owner_.store(0, std::memory_order_release);
  }

  /**
   * @brief Best-effort snapshot of whether the lock is currently held.
   * Racy by nature (another thread may lock/unlock immediately after
   * this returns) -- useful only for diagnostics/assertions, never for
   * making a synchronization decision.
   */
  [[nodiscard]] bool is_locked() const noexcept { return owner_.load(std::memory_order_relaxed) != 0; }

  /**
   * @brief Whether the calling context currently holds this lock.
   * Useful for `RELOCO_ASSERT`-style "caller must already hold this
   * lock" preconditions on internal helpers.
   */
  [[nodiscard]] bool is_locked_by_current() const noexcept {
    return owner_.load(std::memory_order_relaxed) == owner_value();
  }

private:
  /** @brief `Traits::current_owner()`, converted to the raw `uintptr_t` stamp, asserting it isn't the reserved `0`. */
  [[nodiscard]] static std::uintptr_t owner_value() noexcept {
    const auto value = static_cast<std::uintptr_t>(Traits::current_owner());
    RELOCO_ASSERT(value != 0, "kernel_spin_lock: current_owner() must not report 0 (reserved for \"unlocked\")");
    return value;
  }

  std::atomic<std::uintptr_t> owner_{0};
};

} // namespace structo::sync
