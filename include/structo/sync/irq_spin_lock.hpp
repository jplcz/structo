// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file irq_spin_lock.hpp
 * @brief `structo::sync::irq_spin_lock<Lock, IrqLocker>`: wraps any
 * non-reader/writer lock from this family (`kernel_spin_lock`,
 * `ticket_spin_lock`, `queue_spin_lock`) together with an IRQ/
 * preemption-exclusion locker (`irq_guard<Traits>` from
 * `irq_guard.hpp`, or `spinlock_entry_guard<Traits>` from
 * `spinlock_entry_guard.hpp`) so that acquiring the lock *also*
 * disables interrupts (and/or enters a critical section) for exactly
 * as long as the lock is held -- the same pairing FreeBSD's
 * `mtx_lock_spin(9)` performs internally (`spinlock_enter()` then the
 * mutex's own lock word) and Linux's `spin_lock_irqsave()` performs via
 * `local_irq_save()` plus `spin_lock()`.
 *
 * Without this wrapper, callers already have to pair the two building
 * blocks by hand, and -- critically -- get the *order* right: the IRQ/
 * preemption locker must be acquired *before* the spin lock and
 * released *after* it, or an interrupt handler on the same core could
 * itself try to take the same lock while this core is still spinning
 * for it, deadlocking against itself. `irq_spin_lock` bakes that
 * ordering into one RAII `guard` so it can never be gotten backwards:
 * `lock()` engages `IrqLocker` first, then the underlying `Lock`;
 * the `guard`'s destructor (or early `unlock()`) releases the
 * underlying `Lock` first, then `IrqLocker`.
 *
 * `IrqLocker` is any default-constructible RAII type exposing the same
 * shape `irq_guard<Traits>`/`spinlock_entry_guard<Traits>` both do: a
 * no-argument constructor that engages the locker, a destructor that
 * releases it unless already released, a move constructor/assignment
 * operator that transfers ownership (leaving the moved-from instance a
 * no-op on destruction), and an idempotent `unlock()` for early
 * release. `Lock` is any of `kernel_spin_lock<Traits>`/
 * `ticket_spin_lock<Traits>` (whose `lock()`/`try_lock()`/`unlock()`
 * take no arguments) or `queue_spin_lock<Traits>` (whose
 * `lock(node&)`/`try_lock(node&)`/`unlock(node&)` take a caller-
 * supplied `node`); `irq_spin_lock` detects which shape `Lock` has (via
 * the presence of a nested `Lock::node` type) and exposes the matching
 * `lock()`/`try_lock()` overload automatically.
 *
 * @code
 * struct arm_irq_traits {
 *   using flags_type = std::uint32_t;
 *
 *   static flags_type hw_save_irqs() noexcept {
 *     std::uint32_t cpsr;
 *     asm volatile("mrs %0, cpsr" : "=r"(cpsr));
 *     asm volatile("cpsid i" ::: "memory");
 *     return cpsr;
 *   }
 *
 *   static void hw_restore_irqs(flags_type cpsr) noexcept {
 *     asm volatile("msr cpsr_c, %0" : : "r"(cpsr) : "memory");
 *   }
 * };
 *
 * struct kernel_lock_traits {
 *   using owner_type = std::uintptr_t;
 *   static owner_type current_owner() noexcept { return get_current_thread_id(); }
 * };
 *
 * structo::sync::irq_spin_lock<structo::sync::kernel_spin_lock<kernel_lock_traits>,
 *                               structo::sync::irq_guard<arm_irq_traits>>
 *     lock;
 * {
 *   auto g = lock.lock(); // interrupts disabled, then the spinlock acquired
 *   // ... protected section, safe from this core's own interrupt handler too ...
 * } // spinlock released, then interrupts restored
 * @endcode
 *
 * `IrqLocker` may just as well be
 * `structo::sync::spinlock_entry_guard<Traits>` (FreeBSD-style
 * `spinlock_enter()`/`spinlock_exit()`) instead of `irq_guard<Traits>`
 * -- both satisfy the exact same shape, by design (see
 * `spinlock_entry_guard.hpp`'s top-level docs).
 */

#include <structo/sync/spin_lock_traits.hpp>

#include <reloco/detail/compat.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>

#include <type_traits>
#include <utility>

namespace structo::sync {

/**
 * @brief Pairs an IRQ/preemption-exclusion locker with a mutual-
 * exclusion spin lock so acquiring one always acquires (and releases)
 * the other in the correct order; see this file's top-level docs.
 * @tparam Lock      `kernel_spin_lock<Traits>`, `ticket_spin_lock<Traits>`,
 * or `queue_spin_lock<Traits>`.
 * @tparam IrqLocker `irq_guard<Traits>` or `spinlock_entry_guard<Traits>`
 * (or any type matching their shape).
 */
template <typename Lock, typename IrqLocker> class irq_spin_lock {
public:
  using lock_type = Lock;
  using irq_locker_type = IrqLocker;
  using node = detail::spin_lock_node_t<Lock>;

  /** @brief True if `Lock` requires a caller-supplied `node` (i.e. is `queue_spin_lock`-shaped). */
  static constexpr bool uses_node = detail::spin_lock_uses_node_v<Lock>;

  constexpr irq_spin_lock() noexcept = default;

  irq_spin_lock(const irq_spin_lock &) = delete;
  irq_spin_lock &operator=(const irq_spin_lock &) = delete;

  /**
   * @brief RAII handle returned by `lock()`/`try_lock()`: holds both
   * the engaged `IrqLocker` and the acquired `Lock` for its lifetime,
   * releasing the `Lock` first and the `IrqLocker` second on
   * destruction (or early `unlock()`).
   */
  class [[nodiscard]] RELOCO_POINTER guard {
  public:
    ~guard() noexcept { unlock(); }

    guard(const guard &) = delete;
    guard &operator=(const guard &) = delete;

    /** @brief Transfers ownership of both the `IrqLocker` and the held `Lock`; `other` is left a no-op on destruction. */
    guard(guard &&other) noexcept
        : m_irq(std::move(other.m_irq)), m_lock(std::exchange(other.m_lock, nullptr)),
          m_node(std::exchange(other.m_node, nullptr)) {}

    /** @brief Releases this guard's own state first, then takes over `other`'s; `other` is left a no-op on destruction. */
    guard &operator=(guard &&other) noexcept {
      if (this != &other) {
        unlock();
        m_irq = std::move(other.m_irq);
        m_lock = std::exchange(other.m_lock, nullptr);
        m_node = std::exchange(other.m_node, nullptr);
      }
      return *this;
    }

    /**
     * @brief Early explicit release before scope exit: releases the
     * underlying `Lock` first, then the `IrqLocker`. Idempotent: a
     * second call (or destruction afterwards) is a no-op.
     */
    void unlock() noexcept {
      if (m_lock != nullptr) {
        if constexpr (uses_node) {
          m_lock->unlock(*m_node);
        } else {
          m_lock->unlock();
        }
        m_lock = nullptr;
      }
      m_irq.unlock();
    }

    /** @brief Whether this guard still holds the underlying lock (i.e. not yet `unlock()`ed or moved-from). */
    [[nodiscard]] bool is_locked() const noexcept { return m_lock != nullptr; }

    /** @brief The underlying `IrqLocker`, e.g. to mint its `critical_section_token`/`spinlock_entered_token`. */
    [[nodiscard]] const IrqLocker &irq_locker() const noexcept RELOCO_LIFETIMEBOUND { return m_irq; }

    /** @brief Tag selecting the "already acquired" constructors used by `try_lock()`. */
    struct adopt_t {};

    // Public so `reloco::optional<guard>`'s placement-new can reach them from
    // `try_lock()`'s `std::in_place` construction; still effectively
    // unreachable from outside this file, since `adopt_t` itself is a
    // private nested type only `irq_spin_lock` (a friend) can name.
    guard(adopt_t, IrqLocker &&irq, Lock &lock) noexcept : m_irq(std::move(irq)), m_lock(&lock) {}
    guard(adopt_t, IrqLocker &&irq, Lock &lock, node &n) noexcept : m_irq(std::move(irq)), m_lock(&lock), m_node(&n) {}

  private:
    friend class irq_spin_lock;

    explicit guard(Lock &lock) noexcept : m_irq(), m_lock(&lock) { m_lock->lock(); }
    guard(Lock &lock, node &n) noexcept : m_irq(), m_lock(&lock), m_node(&n) { m_lock->lock(n); }

    IrqLocker m_irq;
    Lock *m_lock{nullptr};
    node *m_node{nullptr};
  };

  /**
   * @brief Engages `IrqLocker`, then blocks (per `Lock::lock()`'s own
   * semantics) until the underlying lock is acquired.
   * @return A `guard` holding both until it is destroyed or explicitly
   * `unlock()`ed.
   */
  template <bool B = uses_node, std::enable_if_t<!B, int> = 0> [[nodiscard]] guard lock() & noexcept {
    return guard(m_lock);
  }

  /** @brief `queue_spin_lock`-shaped overload: enqueues `n`, matching `Lock::lock(node&)`'s own semantics. */
  template <bool B = uses_node, std::enable_if_t<B, int> = 0> [[nodiscard]] guard lock(node &n) & noexcept {
    return guard(m_lock, n);
  }

  /**
   * @brief Engages `IrqLocker`, then attempts to acquire the underlying
   * lock without spinning. If the underlying `try_lock()` fails,
   * `IrqLocker` is released immediately before returning, so a failed
   * attempt never leaves interrupts/preemption disabled.
   * @return A `guard` holding both if acquired, else `reloco::nullopt`.
   */
  template <bool B = uses_node, std::enable_if_t<!B, int> = 0>
  [[nodiscard]] reloco::optional<guard> try_lock() & noexcept {
    IrqLocker irq;
    if (!m_lock.try_lock()) {
      return reloco::nullopt;
    }
    return reloco::optional<guard>(std::in_place, typename guard::adopt_t{}, std::move(irq), m_lock);
  }

  /** @brief `queue_spin_lock`-shaped overload of `try_lock()`, matching `Lock::try_lock(node&)`'s own semantics. */
  template <bool B = uses_node, std::enable_if_t<B, int> = 0>
  [[nodiscard]] reloco::optional<guard> try_lock(node &n) & noexcept {
    IrqLocker irq;
    if (!m_lock.try_lock(n)) {
      return reloco::nullopt;
    }
    return reloco::optional<guard>(std::in_place, typename guard::adopt_t{}, std::move(irq), m_lock, n);
  }

  /** @brief Best-effort snapshot of whether the underlying lock is currently held; see `Lock::is_locked()`. */
  [[nodiscard]] bool is_locked() const noexcept { return m_lock.is_locked(); }

  /** @brief Whether the calling context currently holds the underlying lock; see `Lock::is_locked_by_current()`. */
  [[nodiscard]] bool is_locked_by_current() const noexcept { return m_lock.is_locked_by_current(); }

  /** @brief Direct access to the underlying `Lock`, e.g. for diagnostics that don't fit this wrapper's API. */
  [[nodiscard]] Lock &underlying() noexcept RELOCO_LIFETIMEBOUND { return m_lock; }
  /** @brief `const`-qualified overload of `underlying()`. */
  [[nodiscard]] const Lock &underlying() const noexcept RELOCO_LIFETIMEBOUND { return m_lock; }

private:
  Lock m_lock;
};

} // namespace structo::sync
