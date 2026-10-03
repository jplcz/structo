// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file queue_rw_spin_lock.hpp
 * @brief `structo::sync::queue_rw_spin_lock<Traits>`: a reader-writer
 * spinlock in the same spirit as `rw_spin_lock<Traits>`, but with
 * writer-vs-writer contention resolved through an MCS-style admission
 * queue (`queue_spin_lock`) instead of every waiting writer re-CAS-ing
 * one shared word -- the same relationship Linux's own queued rwlock
 * (`qrwlock`) has to the older, plain `rwlock_t`.
 *
 * ## Why queue only the writer side
 *
 * `rw_spin_lock` already solves writer starvation (via its "writer
 * waiting" bit), but under *heavy multi-writer* contention every waiting
 * writer still independently spins trying to CAS the same shared state
 * word -- exactly the cache-line-bouncing problem `queue_spin_lock`
 * solves for an ordinary mutex. `queue_rw_spin_lock` fixes just that
 * part: writers first queue up via an internal `queue_spin_lock`
 * (local spinning, one waiter per node, no shared cache line to
 * contend), and only the writer that has *already* won admission
 * touches the shared reader/writer state word to announce intent and
 * wait for readers to drain. Readers remain simple CAS-spinners against
 * that shared word, same as `rw_spin_lock` -- reads are expected to be
 * brief and frequent, so queueing them individually would add overhead
 * without a matching benefit (this matches `qrwlock`'s own design: only
 * the writer path goes through a queue/wait-lock, not the reader path).
 *
 * `Traits` supplies the exact same two hooks as `kernel_spin_lock`'s own
 * `Traits` (plus the same optional `softlock_limit`) -- every lock type
 * in this family can share one `Traits` policy.
 *
 * ## Usage
 *
 * Like `queue_spin_lock`, the writer side takes a caller-supplied node
 * (see that header for the full rationale):
 *
 * @code
 * structo::sync::queue_rw_spin_lock<kernel_lock_traits> lock;
 *
 * lock.read_lock();
 * // ... any number of concurrent readers here ...
 * lock.read_unlock();
 *
 * structo::sync::queue_rw_spin_lock<kernel_lock_traits>::node qnode;
 * lock.write_lock(qnode);
 * // ... exclusive access here ...
 * lock.write_unlock(qnode);
 * @endcode
 *
 * `Traits` may additionally supply an optional `name(self)` and/or
 * `panic(reason, name)` hook for richer trap diagnostics -- see
 * `lock_diagnostics.hpp` (shared by every lock type in this family).
 */

#include <structo/sync/backoff.hpp>
#include <structo/sync/lock_diagnostics.hpp>
#include <structo/sync/queue_spin_lock.hpp>
#include <structo/sync/softlock_detector.hpp>

#include <reloco/detail/assert.hpp>

#include <atomic>
#include <cstdint>

namespace structo::sync {

/**
 * @brief Reader-writer spinlock with a queued (MCS) writer admission
 * path; see this file's top-level docs.
 * @tparam Traits Kernel policy providing `owner_type` and
 * `current_owner()`, matching `kernel_spin_lock::Traits`.
 */
template <typename Traits> class queue_rw_spin_lock {
public:
  using traits_type = Traits;
  using owner_type = typename Traits::owner_type;
  using state_type = std::uint32_t;

  /** @brief Per-writer admission-queue node; see `queue_spin_lock::node`. */
  using node = typename queue_spin_lock<Traits>::node;

  static constexpr state_type writer_bit = 0x8000'0000u;
  static constexpr state_type writer_waiting_bit = 0x4000'0000u;
  static constexpr state_type reader_mask = 0x3FFF'FFFFu;

  constexpr queue_rw_spin_lock() noexcept = default;

  /**
   * @brief Traps (via `RELOCO_ASSERT`) if the lock is still held by any
   * reader or the writer, or a writer is still waiting for readers to
   * drain. The writer admission queue's own destructor (run immediately
   * afterwards) additionally traps if a writer is still queued for it.
   */
  ~queue_rw_spin_lock() noexcept {
    STRUCTO_SYNC_LOCK_ASSERT(Traits, *this, state_.load(std::memory_order_relaxed) == 0,
                              "queue_rw_spin_lock: destroyed while still held (reader or writer) or while a writer is waiting");
  }

  queue_rw_spin_lock(const queue_rw_spin_lock &) = delete;
  queue_rw_spin_lock &operator=(const queue_rw_spin_lock &) = delete;

  /**
   * @brief Spins (with exponential backoff) until a shared (read) lock
   * is acquired. Refused so long as a writer holds the lock, or one has
   * already won admission and is waiting for readers to drain.
   */
  void read_lock() & noexcept {
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
  [[nodiscard]] bool try_read_lock() & noexcept {
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
  void read_unlock() & noexcept {
    state_type expected = state_.load(std::memory_order_relaxed);
    for (;;) {
      STRUCTO_SYNC_LOCK_ASSERT(Traits, *this, (expected & reader_mask) != 0,
                                "queue_rw_spin_lock: read_unlock() called with no active readers (double-unlock, or never locked)");
      STRUCTO_SYNC_LOCK_ASSERT(Traits, *this, (expected & writer_bit) == 0,
                                "queue_rw_spin_lock: read_unlock() called while write-locked (corrupted state)");
      if (state_.compare_exchange_weak(expected, expected - 1, std::memory_order_release, std::memory_order_relaxed)) {
        return;
      }
    }
  }

  /**
   * @brief Enqueues `n` onto the writer admission queue (spinning there,
   * MCS-style, only on `n`'s own local flag against other writers), then
   * -- once admitted -- announces intent and spins (with exponential
   * backoff) on the shared state word until every existing reader has
   * drained.
   *
   * Traps (via `RELOCO_ASSERT`, from the underlying admission queue)
   * if the calling context already holds this as a writer: this is a
   * non-recursive lock, so that would otherwise self-deadlock.
   */
  void write_lock(node &n) & noexcept {
    // Writer-vs-writer admission is fully delegated to writer_queue_:
    // its own self-deadlock assert (keyed to the same Traits::current_owner())
    // covers a recursive write_lock() by the current writer for free.
    writer_queue_.lock(n);

    state_.fetch_or(writer_waiting_bit, std::memory_order_relaxed);

    backoff bo;
    softlock_detector lockup(detail::softlock_limit_for<Traits>::value());
    for (;;) {
      state_type expected = writer_waiting_bit;
      if (state_.compare_exchange_weak(expected, writer_bit, std::memory_order_acquire, std::memory_order_relaxed)) {
        break;
      }
      lockup.tick();
      bo.spin();
    }

    owner_.store(owner_value(), std::memory_order_relaxed);
  }

  /**
   * @brief Attempts to acquire the exclusive (write) lock without
   * spinning; returns whether it succeeded. Only ever succeeds when
   * admission is immediately free *and* the lock is completely free (no
   * readers, no writer) -- never announces intent, so it cannot starve
   * readers already racing it.
   */
  [[nodiscard]] bool try_write_lock(node &n) & noexcept {
    if (!writer_queue_.try_lock(n)) {
      return false;
    }

    state_type expected = 0;
    if (!state_.compare_exchange_strong(expected, writer_bit, std::memory_order_acquire, std::memory_order_relaxed)) {
      writer_queue_.unlock(n);
      return false;
    }

    owner_.store(owner_value(), std::memory_order_relaxed);
    return true;
  }

  /**
   * @brief Releases the exclusive (write) lock held by the calling
   * owner via node `n` (the same node instance passed to the matching
   * `write_lock()`/`try_write_lock()`), then admits the next queued
   * writer (if any).
   *
   * Traps (via `RELOCO_ASSERT`) if the calling context is not the
   * current writer (double-unlock, or unlock from the wrong context).
   */
  void write_unlock(node &n) & noexcept {
    STRUCTO_SYNC_LOCK_ASSERT(Traits, *this, owner_.load(std::memory_order_relaxed) == owner_value(),
                              "queue_rw_spin_lock: write_unlock() by non-owner (or already unlocked)");
    owner_.store(0, std::memory_order_relaxed);
    state_.store(0, std::memory_order_release);
    writer_queue_.unlock(n);
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
    RELOCO_ASSERT(value != 0, "queue_rw_spin_lock: current_owner() must not report 0 (reserved for \"unlocked\")");
    return value;
  }

  std::atomic<state_type> state_{0};
  std::atomic<std::uintptr_t> owner_{0};
  queue_spin_lock<Traits> writer_queue_;
};

} // namespace structo::sync
