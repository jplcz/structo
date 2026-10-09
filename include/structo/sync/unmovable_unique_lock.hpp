// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file unmovable_unique_lock.hpp
 * @brief `structo::sync::unmovable_unique_lock<LockT>`: a non-owning
 * RAII exclusive-lock guard for `queue_spin_lock<Traits>` and
 * `queue_rw_spin_lock<Traits>` (write side only) that embeds its own
 * `queue_spin_lock<Traits>::node` by value, instead of borrowing one
 * from a `queue_lock_node_stack` pool.
 *
 * ## Why unmovable
 *
 * `queue_spin_lock`/`queue_rw_spin_lock` link a waiter's node directly
 * into their wait queue by *address* -- the node must stay at a fixed
 * location for as long as it is queued or holding the lock. A guard
 * embedding the node by value therefore cannot itself be moved (doing
 * so would leave the lock pointing at the old, now-dead object's
 * address), so copy and move are both deleted here. Reach for
 * `queue_lock_node_stack` plus a pool-backed, pointer-holding guard
 * instead whenever a movable guard is required (e.g. to return one from
 * a function, or store it in a container); this type is the simpler,
 * zero-pool-dependency choice everywhere a guard only needs to live on
 * one stack frame.
 *
 * ## One guard type, both lock flavors
 *
 * `queue_spin_lock<Traits>::lock()`/`unlock()` and
 * `queue_rw_spin_lock<Traits>::write_lock()`/`write_unlock()` both take
 * the same `queue_spin_lock<Traits>::node &` (the latter reuses the
 * former's `node` type outright), differing only in method name. This
 * guard detects, via SFINAE, whether @tparam LockT exposes
 * `write_lock()`/`write_unlock()` (a `queue_rw_spin_lock`) and uses
 * those; otherwise it uses the plain `lock()`/`unlock()`
 * (`queue_spin_lock`) -- one template serves both, with no reader-side
 * support at all (read locking needs no node, so it has nothing to do
 * with this type; use `queue_rw_spin_lock::read_lock()`/`read_unlock()`
 * directly, or wrap them in `read_locker.hpp`'s `read_locker` instead).
 *
 * ## Usage
 *
 * @code
 * structo::sync::queue_spin_lock<kernel_lock_traits> lock;
 * {
 *   structo::sync::unmovable_unique_lock guard(lock);
 *   // ... protected section ...
 * } // unlocked here
 *
 * structo::sync::queue_rw_spin_lock<kernel_lock_traits> rw_lock;
 * {
 *   structo::sync::unmovable_unique_lock write_guard(rw_lock);
 *   // ... protected section (exclusive/writer side) ...
 * } // unlocked here
 * @endcode
 *
 * A caller wanting a short, fixed name for one particular `Traits`
 * instantiation can always define their own alias:
 * @code
 * using my_lock_guard = structo::sync::unmovable_unique_lock<structo::sync::queue_spin_lock<my_lock_traits>>;
 * @endcode
 */

#include <structo/sync/queue_spin_lock.hpp>

#include <reloco/detail/assert.hpp>

#include <type_traits>

namespace structo::sync {

namespace detail {

/** @brief True if `LockT` exposes `write_lock(node &)`/`write_unlock(node &)`
 * (a `queue_rw_spin_lock`, used writer-side); false for a plain
 * `queue_spin_lock`, which uses `lock(node &)`/`unlock(node &)` instead. */
template <typename LockT, typename = void> struct has_write_lock : std::false_type {};

template <typename LockT>
struct has_write_lock<LockT,
                      std::void_t<decltype(std::declval<LockT &>().write_lock(std::declval<typename LockT::node &>()))>>
    : std::true_type {};

} // namespace detail

/**
 * @brief Non-owning, unmovable RAII exclusive-lock guard embedding its
 * own `LockT::node`; see this file's top-level docs.
 * @tparam LockT Either `queue_spin_lock<Traits>` or
 * `queue_rw_spin_lock<Traits>` (locked write/exclusive-side only).
 */
template <typename LockT> class unmovable_unique_lock {
public:
  using lock_type = LockT;
  using node_type = typename LockT::node;

  /** @brief Blocks until @p lock is acquired. */
  explicit unmovable_unique_lock(lock_type &lock) noexcept : lock_(&lock) { do_lock(); }

  unmovable_unique_lock(const unmovable_unique_lock &) = delete;
  unmovable_unique_lock &operator=(const unmovable_unique_lock &) = delete;
  unmovable_unique_lock(unmovable_unique_lock &&) = delete;
  unmovable_unique_lock &operator=(unmovable_unique_lock &&) = delete;

  /** @brief Releases the wrapped lock. */
  ~unmovable_unique_lock() noexcept { do_unlock(); }

private:
  /** @brief Dispatches to `write_lock()`/`write_unlock()` for a
   * `queue_rw_spin_lock`, or plain `lock()`/`unlock()` for a
   * `queue_spin_lock` -- see `detail::has_write_lock`. */
  void do_lock() noexcept {
    if constexpr (detail::has_write_lock<lock_type>::value) {
      lock_->write_lock(node_);
    } else {
      lock_->lock(node_);
    }
  }

  void do_unlock() noexcept {
    if constexpr (detail::has_write_lock<lock_type>::value) {
      lock_->write_unlock(node_);
    } else {
      lock_->unlock(node_);
    }
  }

  lock_type *lock_;
  node_type node_{};
};

} // namespace structo::sync
