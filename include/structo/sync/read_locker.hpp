// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file read_locker.hpp
 * @brief `structo::sync::read_locker<LockT>`: a non-owning, movable RAII
 * shared/reader-lock guard for `rw_spin_lock<Traits>` and
 * `queue_rw_spin_lock<Traits>` -- unlike the writer side
 * (`unmovable_unique_lock.hpp`), the reader side of both lock types
 * needs no per-waiter node at all (`read_lock()`/`try_read_lock()`/
 * `read_unlock()` all take no arguments), so this guard simply forwards
 * to them directly: no embedded node, no address-stability concern, and
 * therefore no reason to forbid moving it.
 *
 * ## One guard type, both rw lock flavors
 *
 * `rw_spin_lock<Traits>` and `queue_rw_spin_lock<Traits>` expose the
 * exact same reader-side method names and signatures
 * (`read_lock()`/`try_read_lock()`/`read_unlock()`, all no-argument), so
 * one template serves both -- @tparam LockT is simply whichever of the
 * two the caller is using.
 *
 * ## Usage
 *
 * @code
 * structo::sync::queue_rw_spin_lock<kernel_lock_traits> rw_lock;
 * {
 *   structo::sync::read_locker guard(rw_lock);
 *   // ... read-only protected section ...
 * } // unlocked here
 * @endcode
 */

#include <reloco/detail/assert.hpp>

#include <utility>

namespace structo::sync {

/**
 * @brief Non-owning, movable RAII shared/reader-lock guard; see this
 * file's top-level docs.
 * @tparam LockT Either `rw_spin_lock<Traits>` or
 * `queue_rw_spin_lock<Traits>` (locked read/shared-side only).
 */
template <typename LockT> class read_locker {
public:
  using lock_type = LockT;

  /** @brief Owns no lock; `owns_lock()` is `false` until a later
   * `lock()`/`try_lock()`/swap/move-assignment brings one in. */
  constexpr read_locker() noexcept = default;

  /** @brief Blocks until @p lock's read side is acquired. */
  explicit read_locker(lock_type &lock) noexcept : lock_(&lock) {
    lock_->read_lock();
    owns_ = true;
  }

  read_locker(read_locker &&other) noexcept : lock_(other.lock_), owns_(other.owns_) {
    other.lock_ = nullptr;
    other.owns_ = false;
  }

  read_locker &operator=(read_locker &&other) noexcept {
    if (this != &other) {
      if (owns_)
        lock_->read_unlock();
      lock_ = other.lock_;
      owns_ = other.owns_;
      other.lock_ = nullptr;
      other.owns_ = false;
    }
    return *this;
  }

  read_locker(const read_locker &) = delete;
  read_locker &operator=(const read_locker &) = delete;

  /** @brief Releases the read lock, if still owned. */
  ~read_locker() noexcept {
    if (owns_)
      lock_->read_unlock();
  }

  /** @brief Blocks until the wrapped lock's read side is acquired.
   * Asserts that no lock is already owned by this `read_locker`
   * (matching `std::shared_lock::lock()`'s precondition). */
  void lock() & noexcept {
    RELOCO_ASSERT(lock_ != nullptr, "read_locker::lock() called without a wrapped lock");
    RELOCO_ASSERT(!owns_, "read_locker::lock() called while already owning the lock");
    lock_->read_lock();
    owns_ = true;
  }

  /** @brief Attempts to acquire the wrapped lock's read side
   * non-blockingly. Same preconditions as `lock()`. */
  [[nodiscard]] bool try_lock() & noexcept {
    RELOCO_ASSERT(lock_ != nullptr, "read_locker::try_lock() called without a wrapped lock");
    RELOCO_ASSERT(!owns_, "read_locker::try_lock() called while already owning the lock");
    owns_ = lock_->try_read_lock();
    return owns_;
  }

  /** @brief Releases the wrapped lock's read side. Asserts it is
   * currently owned. */
  void unlock() & noexcept {
    RELOCO_ASSERT(owns_, "read_locker::unlock() called without owning the lock");
    lock_->read_unlock();
    owns_ = false;
  }

  [[nodiscard]] bool owns_lock() const noexcept { return owns_; }

  explicit operator bool() const noexcept { return owns_; }

  /** @brief The wrapped lock, or `nullptr` if default-constructed or
   * moved-from. Does not imply ownership -- check `owns_lock()`. */
  [[nodiscard]] lock_type *mutex() const noexcept { return lock_; }

  /** @brief Detaches from the wrapped lock without releasing it,
   * returning it to the caller, who becomes responsible for eventually
   * calling `read_unlock()` on it. Matches `std::shared_lock::release()`. */
  [[nodiscard]] lock_type *release() noexcept {
    lock_type *l = lock_;
    lock_ = nullptr;
    owns_ = false;
    return l;
  }

  void swap(read_locker &other) noexcept {
    std::swap(lock_, other.lock_);
    std::swap(owns_, other.owns_);
  }

private:
  lock_type *lock_ = nullptr;
  bool owns_ = false;
};

template <typename LockT> void swap(read_locker<LockT> &lhs, read_locker<LockT> &rhs) noexcept { lhs.swap(rhs); }

} // namespace structo::sync
