// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file rw_spin_lock.hpp
 * @brief `structo::sync::rw_spin_lock<Traits>`: a busy-wait
 * reader-writer lock rounding out the `kernel_spin_lock`/
 * `ticket_spin_lock`/`queue_spin_lock` family with one these three don't
 * provide -- any number of concurrent *readers*, with writers still
 * fully exclusive against both readers and other writers.
 *
 * ## State packed into one word
 *
 * A single `std::atomic<std::uint32_t>` encodes everything:
 * - bit 31 (`writer_bit`) -- a writer currently holds the lock.
 * - bit 30 (`writer_waiting_bit`) -- at least one writer is waiting;
 *   sampled by `read_lock()` to refuse *new* readers (see below).
 * - bits 0-29 (`reader_mask`) -- the number of readers currently
 *   holding the lock (up to ~1 billion, far more than any real core
 *   count could ever contend at once).
 *
 * `read_lock()` spins (incrementing the reader count via CAS) so long
 * as neither bit 31 nor bit 30 is set; `write_lock()` first sets bit 30
 * (so every *new* reader -- and any other writer -- backs off) then
 * spins until the word reads back as exactly `writer_waiting_bit` (no
 * readers left, no other writer), at which point it CAS's the whole
 * word to exactly `writer_bit`.
 *
 * ## Why the "writer waiting" bit exists
 *
 * A naive reader-writer spinlock (CAS the reader count up whenever no
 * writer currently holds it) lets a steady stream of readers starve a
 * waiting writer indefinitely, since a new reader can always slip in
 * between two others' `read_unlock()`s. Announcing writer intent *before*
 * spinning for the drain -- the same idea as glibc's
 * `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` -- closes that window:
 * once a writer is waiting, no further reader is admitted, so the
 * existing readers are guaranteed to drain to zero in bounded time.
 *
 * `Traits` supplies the exact same two hooks as `kernel_spin_lock`'s own
 * `Traits` (plus the same optional `softlock_limit`) -- all four lock
 * types in this family can share one `Traits` policy. Readers are not
 * individually tracked by owner (any number may hold the lock at once,
 * from any context), so only the writer side is stamped/asserted against
 * `Traits::current_owner()`.
 *
 * ## Usage
 *
 * @code
 * structo::sync::rw_spin_lock<kernel_lock_traits> lock;
 *
 * lock.read_lock();
 * // ... any number of concurrent readers here ...
 * lock.read_unlock();
 *
 * lock.write_lock();
 * // ... exclusive access here ...
 * lock.write_unlock();
 * @endcode
 *
 * As with the rest of this family, never appropriate outside contexts
 * where spinning is known to be short (IRQ/exception handlers,
 * pre-scheduler-init code, data shared with an interrupt handler on
 * another core). Recursively taking a second `read_lock()` from a
 * context that already holds one is *not* tracked or guarded against
 * (there is no per-reader identity to check) -- as with real kernels'
 * own reader-writer locks (e.g. FreeBSD's `rwlock(9)`), doing so while
 * a writer is also waiting can self-deadlock, because the pending
 * writer bit blocks the *recursive* `read_lock()` call exactly like it
 * would any other new reader.
 */

#include <structo/sync/backoff.hpp>
#include <structo/sync/softlock_detector.hpp>

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>

#include <atomic>
#include <cstdint>

namespace structo::sync {

/**
 * @brief Busy-wait reader-writer lock; see this file's top-level docs.
 * @tparam Traits Kernel policy providing `owner_type` and
 * `current_owner()`, matching `kernel_spin_lock::Traits`.
 */
template <typename Traits> class RELOCO_CAPABILITY("mutex") rw_spin_lock {
public:
  using traits_type = Traits;
  using owner_type = typename Traits::owner_type;
  using state_type = std::uint32_t;

  static constexpr state_type writer_bit = 0x8000'0000u;
  static constexpr state_type writer_waiting_bit = 0x4000'0000u;
  static constexpr state_type reader_mask = 0x3FFF'FFFFu;

  constexpr rw_spin_lock() noexcept = default;

  /**
   * @brief Traps (via `RELOCO_ASSERT`) if the lock is still held by any
   * reader or the writer, or a writer is still waiting for it.
   */
  ~rw_spin_lock() noexcept {
    RELOCO_ASSERT(state_.load(std::memory_order_relaxed) == 0,
                  "rw_spin_lock: destroyed while still held (reader or writer) or while a writer is waiting");
  }

  rw_spin_lock(const rw_spin_lock &) = delete;
  rw_spin_lock &operator=(const rw_spin_lock &) = delete;

  /**
   * @brief Spins (with exponential backoff) until a shared (read) lock
   * is acquired. Refused so long as a writer holds the lock, or one is
   * waiting for it (see this file's top-level docs on writer
   * starvation).
   */
  void read_lock() & noexcept RELOCO_ACQUIRE_SHARED() {
    backoff bo;
    softlock_detector lockup(detail::softlock_limit_for<Traits>::value());
    for (;;) {
      state_type expected = state_.load(std::memory_order_relaxed);
      if ((expected & (writer_bit | writer_waiting_bit)) == 0 &&
          state_.compare_exchange_weak(expected, expected + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        return;
      }

      while ((state_.load(std::memory_order_relaxed) & (writer_bit | writer_waiting_bit)) != 0) {
        lockup.tick();
        bo.spin();
      }
    }
  }

  /**
   * @brief Attempts to acquire a shared (read) lock without spinning;
   * returns whether it succeeded. May spuriously fail under pure
   * reader-vs-reader contention (a single CAS attempt, matching this
   * family's other `try_lock()`s) even when no writer is involved --
   * retry via `read_lock()` if that distinction matters to the caller.
   */
  [[nodiscard]] bool try_read_lock() & noexcept RELOCO_TRY_ACQUIRE_SHARED(true) {
    state_type expected = state_.load(std::memory_order_relaxed);
    if ((expected & (writer_bit | writer_waiting_bit)) != 0) {
      return false;
    }
    return state_.compare_exchange_strong(expected, expected + 1, std::memory_order_acquire, std::memory_order_relaxed);
  }

  /**
   * @brief Releases one previously-acquired shared (read) lock.
   * Traps (via `RELOCO_ASSERT`) if no reader currently holds the lock,
   * or the writer bit is somehow set (corrupted state/misuse).
   */
  void read_unlock() & noexcept RELOCO_RELEASE_SHARED() {
    state_type expected = state_.load(std::memory_order_relaxed);
    for (;;) {
      RELOCO_ASSERT((expected & reader_mask) != 0,
                    "rw_spin_lock: read_unlock() called with no active readers (double-unlock, or never locked)");
      RELOCO_ASSERT((expected & writer_bit) == 0, "rw_spin_lock: read_unlock() called while write-locked (corrupted state)");
      if (state_.compare_exchange_weak(expected, expected - 1, std::memory_order_release, std::memory_order_relaxed)) {
        return;
      }
    }
  }

  /**
   * @brief Announces writer intent, then spins (with exponential
   * backoff) until every existing reader has drained and no other
   * writer holds the lock.
   *
   * Traps (via `RELOCO_ASSERT`) if the calling context already holds
   * this as a writer: this is a non-recursive lock, so that would
   * otherwise self-deadlock (spin forever waiting on a reader/writer
   * state only the caller itself can clear).
   */
  void write_lock() & noexcept RELOCO_ACQUIRE() {
    const std::uintptr_t self = owner_value();
    RELOCO_ASSERT(owner_.load(std::memory_order_relaxed) != self,
                  "rw_spin_lock: write_lock() called while already held by the calling context (self-deadlock)");

    backoff bo;
    softlock_detector lockup(detail::softlock_limit_for<Traits>::value());
    for (;;) {
      state_type expected = state_.load(std::memory_order_relaxed);
      if ((expected & writer_waiting_bit) == 0) {
        expected = state_.fetch_or(writer_waiting_bit, std::memory_order_relaxed) | writer_waiting_bit;
      }

      if (expected == writer_waiting_bit) {
        state_type want = writer_waiting_bit;
        if (state_.compare_exchange_weak(want, writer_bit, std::memory_order_acquire, std::memory_order_relaxed)) {
          break;
        }
      }

      lockup.tick();
      bo.spin();
    }

    owner_.store(self, std::memory_order_relaxed);
  }

  /**
   * @brief Attempts to acquire the exclusive (write) lock without
   * spinning; returns whether it succeeded. Only ever succeeds when the
   * lock is completely free (no readers, no writer, nobody waiting) --
   * unlike `write_lock()`, never announces intent, so it cannot starve
   * readers that are already racing it.
   */
  [[nodiscard]] bool try_write_lock() & noexcept RELOCO_TRY_ACQUIRE(true) {
    state_type expected = 0;
    if (!state_.compare_exchange_strong(expected, writer_bit, std::memory_order_acquire, std::memory_order_relaxed)) {
      return false;
    }
    owner_.store(owner_value(), std::memory_order_relaxed);
    return true;
  }

  /**
   * @brief Releases the exclusive (write) lock held by the calling
   * owner.
   * Traps (via `RELOCO_ASSERT`) if the calling context is not the
   * current writer (double-unlock, or unlock from the wrong context).
   */
  void write_unlock() & noexcept RELOCO_RELEASE() {
    RELOCO_ASSERT(owner_.load(std::memory_order_relaxed) == owner_value(),
                  "rw_spin_lock: write_unlock() by non-owner (or already unlocked)");
    owner_.store(0, std::memory_order_relaxed);
    state_.store(0, std::memory_order_release);
  }

  /** @brief Best-effort snapshot of the number of readers currently holding the lock. */
  [[nodiscard]] state_type reader_count() const noexcept { return state_.load(std::memory_order_relaxed) & reader_mask; }

  /** @brief Best-effort snapshot of whether any reader currently holds the lock. */
  [[nodiscard]] bool is_read_locked() const noexcept { return reader_count() != 0; }

  /** @brief Best-effort snapshot of whether the writer currently holds the lock. */
  [[nodiscard]] bool is_write_locked() const noexcept { return (state_.load(std::memory_order_relaxed) & writer_bit) != 0; }

  /** @brief Best-effort snapshot of whether the lock is held at all (by any reader or the writer). */
  [[nodiscard]] bool is_locked() const noexcept {
    return (state_.load(std::memory_order_relaxed) & (writer_bit | reader_mask)) != 0;
  }

  /**
   * @brief Whether the calling context currently holds this lock's
   * writer side. Useful for `RELOCO_ASSERT`-style "caller must already
   * hold this lock" preconditions on internal helpers.
   */
  [[nodiscard]] bool is_write_locked_by_current() const noexcept {
    return is_write_locked() && owner_.load(std::memory_order_relaxed) == owner_value();
  }

private:
  /** @brief `Traits::current_owner()`, converted to the raw `uintptr_t` stamp, asserting it isn't the reserved `0`. */
  [[nodiscard]] static std::uintptr_t owner_value() noexcept {
    const auto value = static_cast<std::uintptr_t>(Traits::current_owner());
    RELOCO_ASSERT(value != 0, "rw_spin_lock: current_owner() must not report 0 (reserved for \"unlocked\")");
    return value;
  }

  std::atomic<state_type> state_{0};
  std::atomic<std::uintptr_t> owner_{0};
};

} // namespace structo::sync
