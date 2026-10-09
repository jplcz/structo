// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file queue_lock_node_stack.hpp
 * @brief `structo::sync::queue_lock_node_stack<Traits, N>`: a fixed-size,
 * freelist-backed pool of `queue_spin_lock<Traits>::node`/
 * `queue_rw_spin_lock<Traits>::node` instances, so a movable RAII lock
 * guard (e.g. `reloco::unique_lock<queue_spin_lock<Traits>>`) can borrow
 * one for the duration of a critical section instead of embedding it by
 * value -- which would otherwise force the guard itself to be immovable,
 * since `queue_spin_lock`/`queue_rw_spin_lock` link a node directly into
 * their wait queue by address and cannot tolerate it moving out from
 * under them while queued or held.
 *
 * ## Why a pool instead of one node per guard
 *
 * `queue_spin_lock::node`/`queue_rw_spin_lock::node` must stay at a
 * fixed address for as long as they are queued or holding the lock, so
 * a RAII guard that owned one by value could never be movable. Handing
 * the guard a *pointer* to a node borrowed from a stable pool instead
 * keeps the guard itself movable (it only ever moves a pointer), while
 * the pool's own storage -- `reloco::array<node_imp, N>` -- never moves
 * for the pool's entire lifetime.
 *
 * `N` is fixed at compile time and should be sized for the deepest
 * nesting of queue-lock acquisitions the owning context (typically one
 * CPU, or one thread) can reach at once -- ordinary lock nesting depth
 * plus one per level of interrupt/exception nesting that can itself
 * take a queue lock, if applicable. `pop()` traps (via `RELOCO_ASSERT`)
 * if the pool is exhausted, exactly like every other hard capacity
 * limit in this library.
 *
 * ## Why a stack, and why that's safe despite out-of-order release
 *
 * Nodes are handed out and returned via a singly linked freelist used as
 * a stack: `pop()` takes the top, `push()` returns an arbitrary node to
 * the top. Unlike the *lock* itself (strictly FIFO), the freelist has no
 * ordering requirement to uphold -- every pooled node is interchangeable,
 * so it does not matter that real critical sections nested through this
 * pool are released in an arbitrary order (not necessarily LIFO
 * matching acquisition order, e.g. a node released from an interrupt
 * handler that preempted a still-held outer acquisition): `push()`
 * accepts whichever node comes back first and simply makes it the next
 * one `pop()` returns.
 *
 * ## Construction: lazy, not static-init-order-dependent
 *
 * A `queue_lock_node_stack` is meant to live in long-lived (often
 * per-CPU/per-domain, see `structo::arch::per_cpu_ptr`/`per_domain_ptr`)
 * storage that may be brought into existence before any C++ static
 * initialization has run in a freestanding/kernel build. Its default
 * constructor therefore does nothing beyond zero-initializing its plain
 * data members; the freelist itself is threaded through `pool_` lazily,
 * on the first `pop()`, rather than up front -- so a
 * `queue_lock_node_stack` is always safe to `constinit`/place in `.bss`
 * and use immediately, with no initialization-order dependency on
 * anything else.
 *
 * ## Access via `Traits`
 *
 * This class owns the pool's storage but not how a caller locates
 * *which* pool instance to use (e.g. "the current CPU's pool", "this
 * specific lock's own pool", ...) -- that policy lives entirely outside
 * this header, typically expressed as a `get_stack<Traits, N>()`-style
 * static method on a caller-supplied traits type, mirroring every other
 * `Tag`/`Traits` indirection in this library (`per_cpu_ptr<Tag, T>`,
 * `kernel_spin_lock<Traits>`, ...):
 *
 * @code
 * struct my_node_stack_traits {
 *   template <typename Traits, std::size_t N>
 *   static queue_lock_node_stack<Traits, N> &get_stack() noexcept {
 *     // e.g. resolve the current CPU's own instance via per_cpu_ptr, or
 *     // a single process-wide instance for a non-SMP/single-core build.
 *     static queue_lock_node_stack<Traits, N> instance;
 *     return instance;
 *   }
 * };
 * @endcode
 */

#include <structo/sync/queue_spin_lock.hpp>

#include <reloco/array.hpp>
#include <reloco/detail/assert.hpp>

#include <cstddef>

namespace structo::sync {

/**
 * @brief Fixed-size freelist pool of `N` `queue_spin_lock<Traits>::node`
 * instances; see this file's top-level docs.
 * @tparam Traits Same lock policy as `queue_spin_lock<Traits>`/
 * `queue_rw_spin_lock<Traits>` -- `node_imp` derives from
 * `queue_spin_lock<Traits>::node` (the same node type
 * `queue_rw_spin_lock<Traits>::node` is itself an alias of), so one pool
 * serves both lock types for a given `Traits`.
 * @tparam N Pool capacity; must be at least 1.
 */
template <typename Traits, std::size_t N> class queue_lock_node_stack {
public:
  static_assert(N > 0, "queue_lock_node_stack requires at least one node");

  /** @brief Pool element: a queue-lock node with an extra freelist link
   * layered on top, entirely separate from the node's own queue-wait
   * link (`queue_spin_lock<Traits>::node::next_`) -- this `next` is only
   * ever touched while the node sits unused in this pool. */
  struct node_imp : queue_spin_lock<Traits>::node {
    node_imp *next = nullptr;
  };

  using node_type = node_imp;

  constexpr queue_lock_node_stack() noexcept = default;

  queue_lock_node_stack(const queue_lock_node_stack &) = delete;
  queue_lock_node_stack &operator=(const queue_lock_node_stack &) = delete;
  queue_lock_node_stack(queue_lock_node_stack &&) = delete;
  queue_lock_node_stack &operator=(queue_lock_node_stack &&) = delete;

  /**
   * @brief Removes and returns the top node, lazily threading the whole
   * `pool_` into the freelist first if this is the very first call.
   * Traps (via `RELOCO_ASSERT`) if the pool is exhausted -- every node
   * is already on loan to a still-live guard.
   */
  [[nodiscard]] node_imp *pop() & noexcept {
    if (!initialized_)
      RELOCO_UNLIKELY { do_init(); }
    RELOCO_ASSERT(top_ != nullptr, "queue_lock_node_stack: exhausted (too many nested lock acquisitions)");
    node_imp *n = top_;
    top_ = n->next;
    n->next = nullptr;
    return n;
  }

  /**
   * @brief Returns a node previously obtained from `pop()` (on this same
   * instance) back to the pool. @p n need not be the most recently
   * popped node -- see the file-level docs for why out-of-order release
   * is always safe here.
   */
  void push(node_imp *n) & noexcept {
    RELOCO_DEBUG_ASSERT(initialized_, "queue_lock_node_stack: push() before any pop() on this instance");
    n->next = top_;
    top_ = n;
  }

private:
  /** @brief Threads every element of `pool_` onto the freelist, called
   * at most once, from the first `pop()`. */
  void do_init() noexcept {
    for (auto &n : pool_) {
      n.next = top_;
      top_ = &n;
    }
    initialized_ = true;
  }

  reloco::array<node_imp, N> pool_{};
  node_imp *top_ = nullptr;
  bool initialized_ = false;
};

} // namespace structo::sync
