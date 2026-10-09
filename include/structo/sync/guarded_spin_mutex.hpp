// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file guarded_spin_mutex.hpp
 * @brief `structo::sync::guarded_spin_mutex<T, Lock, IrqLocker>`: the
 * spin-lock counterpart of `reloco::guarded_mutex<T, MutexT>` (Rust's
 * `std::sync::Mutex<T>`) -- the protected `T` lives *inside* the mutex
 * itself, reachable only through the RAII `guard` returned by `lock()`/
 * `try_lock()`, instead of pairing a bare lock with a separately
 * declared variable nothing stops code from touching without holding
 * it.
 *
 * `reloco::guarded_mutex<T, MutexT>` already works unmodified with a
 * *plain* spin lock from this family (`kernel_spin_lock<Traits>`/
 * `ticket_spin_lock<Traits>`) as its `MutexT`, since their `lock()`/
 * `try_lock()`/`unlock()` already match `reloco::mutex`'s own no-
 * argument shape exactly. `guarded_spin_mutex` exists for the two
 * things that plain substitution cannot do:
 *
 * 1. **`queue_spin_lock<Traits>`-shaped locks.** Its `lock(node&)`/
 *    `try_lock(node&)`/`unlock(node&)` take a caller-supplied `node`
 *    (see `queue_spin_lock.hpp`'s own docs on why: the node is the
 *    MCS wait-queue entry, and must outlive the time spent queued and
 *    holding the lock -- almost always a local on the very same stack
 *    frame as the critical section, not something `guarded_spin_mutex`
 *    could own itself). `reloco::guarded_mutex` has no way to thread
 *    that extra argument through its fixed `lock()`/`try_lock()`
 *    signature; `guarded_spin_mutex` detects this shape (via the
 *    presence of a nested `Lock::node` type) and exposes a matching
 *    `lock(node&)`/`try_lock(node&)` overload instead.
 * 2. **Pairing with an IRQ/preemption-exclusion locker.** A real
 *    kernel spinlock protecting data an interrupt handler also touches
 *    needs interrupts disabled for the exact duration the lock is
 *    held, in the same order `irq_spin_lock.hpp` documents (`IrqLocker`
 *    engaged *before* the spin lock, released *after* it). Unlike
 *    `irq_spin_lock<Lock, IrqLocker>` (a bare, value-less wrapper
 *    around an existing `Lock` instance -- see that header), this is
 *    the *data-owning* flavor: `IrqLocker` is an extra template
 *    parameter alongside `Lock`, defaulting to `detail::no_irq_locker`
 *    (a zero-cost no-op) for callers who need none.
 *
 * `IrqLocker` is any default-constructible RAII type matching
 * `irq_guard<Traits>`/`spinlock_entry_guard<Traits>`'s shape: engages on
 * construction, releases on destruction unless already released, is
 * move-only, and exposes an idempotent `unlock()` for early release.
 *
 * @code
 * struct kernel_lock_traits {
 *   using owner_type = std::uintptr_t;
 *   static owner_type current_owner() noexcept { return get_current_thread_id(); }
 * };
 *
 * // A plain spin lock, no IRQ masking (IrqLocker defaults to a no-op):
 * structo::sync::guarded_spin_mutex<int, structo::sync::kernel_spin_lock<kernel_lock_traits>> counter;
 * {
 *   auto g = counter.lock();
 *   *g += 1;
 * }
 *
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
 * // IRQ-safe: interrupts disabled for the duration the lock is held.
 * structo::sync::guarded_spin_mutex<int, structo::sync::kernel_spin_lock<kernel_lock_traits>,
 *                                    structo::sync::irq_guard<arm_irq_traits>>
 *     irq_safe_counter;
 * {
 *   auto g = irq_safe_counter.lock();
 *   *g += 1;
 * }
 *
 * // queue_spin_lock-shaped: lock()/try_lock() take a caller-supplied node.
 * structo::sync::guarded_spin_mutex<int, structo::sync::queue_spin_lock<kernel_lock_traits>,
 *                                    structo::sync::irq_guard<arm_irq_traits>>
 *     queued_counter;
 * decltype(queued_counter)::node n;
 * {
 *   auto g = queued_counter.lock(n);
 *   *g += 1;
 * }
 * @endcode
 */

#include <structo/sync/spin_lock_traits.hpp>

#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>

#include <type_traits>
#include <utility>

namespace structo::sync::detail {

/**
 * @brief Zero-cost default `IrqLocker` for `guarded_spin_mutex`: engages
 * and releases nothing, for callers whose spin lock needs no IRQ/
 * preemption exclusion around it. Matches `irq_guard<Traits>`/
 * `spinlock_entry_guard<Traits>`'s shape (default-constructible,
 * move-only, idempotent `unlock()`) without masking anything.
 */
struct no_irq_locker {
  constexpr no_irq_locker() noexcept = default;

  no_irq_locker(const no_irq_locker &) = delete;
  no_irq_locker &operator=(const no_irq_locker &) = delete;

  no_irq_locker(no_irq_locker &&) noexcept = default;
  no_irq_locker &operator=(no_irq_locker &&) noexcept = default;

  /** @brief No-op, matching `irq_guard::unlock()`/`spinlock_entry_guard::unlock()`'s idempotent early-release shape. */
  void unlock() noexcept {}
};

} // namespace structo::sync::detail

namespace structo::sync {

/**
 * @brief Owns a value `T` behind a spin lock (optionally paired with an
 * IRQ/preemption-exclusion locker); see this file's top-level docs.
 * @tparam T         Protected value type.
 * @tparam Lock      `kernel_spin_lock<Traits>`, `ticket_spin_lock<Traits>`,
 * or `queue_spin_lock<Traits>`.
 * @tparam IrqLocker `irq_guard<Traits>` or `spinlock_entry_guard<Traits>`
 * (or any type matching their shape); defaults to a no-op
 * (`detail::no_irq_locker`) when no IRQ/preemption exclusion is needed.
 */
template <typename T, typename Lock, typename IrqLocker = detail::no_irq_locker> class RELOCO_OWNER guarded_spin_mutex {
public:
  using value_type = T;
  using lock_type = Lock;
  using irq_locker_type = IrqLocker;
  using node = detail::spin_lock_node_t<Lock>;

  /** @brief True if `Lock` requires a caller-supplied `node` (i.e. is `queue_spin_lock`-shaped). */
  static constexpr bool uses_node = detail::spin_lock_uses_node_v<Lock>;

  /** @brief Default-constructs the protected value. */
  constexpr guarded_spin_mutex() noexcept(std::is_nothrow_default_constructible_v<T>) : m_value() {}
  /** @brief Constructs the protected value by moving @p value into it. */
  constexpr explicit guarded_spin_mutex(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
      : m_value(std::move(value)) {}

  guarded_spin_mutex(const guarded_spin_mutex &) = delete;
  guarded_spin_mutex &operator=(const guarded_spin_mutex &) = delete;

  /**
   * @brief RAII handle returned by `lock()`/`try_lock()`: holds both
   * the engaged `IrqLocker` and the acquired `Lock` for its lifetime,
   * granting access to the protected `T` via `operator*`/`operator->`.
   * Releases the `Lock` first and the `IrqLocker` second on destruction
   * (or early `unlock()`).
   */
  class [[nodiscard]] RELOCO_POINTER guard {
  public:
    ~guard() noexcept { unlock(); }

    guard(const guard &) = delete;
    guard &operator=(const guard &) = delete;

    /** @brief Transfers ownership of both the `IrqLocker` and the held `Lock`; `other` is left a no-op on destruction.
     */
    guard(guard &&other) noexcept
        : m_irq(std::move(other.m_irq)), m_owner(std::exchange(other.m_owner, nullptr)),
          m_node(std::exchange(other.m_node, nullptr)) {}

    /** @brief Releases this guard's own state first, then takes over `other`'s; `other` is left a no-op on destruction.
     */
    guard &operator=(guard &&other) noexcept {
      if (this != &other) {
        unlock();
        m_irq = std::move(other.m_irq);
        m_owner = std::exchange(other.m_owner, nullptr);
        m_node = std::exchange(other.m_node, nullptr);
      }
      return *this;
    }

    [[nodiscard]] T &operator*() const noexcept RELOCO_LIFETIMEBOUND { return m_owner->m_value; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return &m_owner->m_value; }

    /** @brief Read-only access to the protected value; equivalent to `*this` with `const` emphasized at the call site.
     */
    [[nodiscard]] const T &get() const noexcept RELOCO_LIFETIMEBOUND { return m_owner->m_value; }
    /** @brief Mutable access to the protected value; equivalent to `*this`, spelled out for parity with `get()`. */
    [[nodiscard]] T &get_mut() const noexcept RELOCO_LIFETIMEBOUND { return m_owner->m_value; }

    /**
     * @brief Early explicit release before scope exit: releases the
     * underlying `Lock` first, then the `IrqLocker`. Idempotent: a
     * second call (or destruction afterwards) is a no-op.
     */
    void unlock() noexcept {
      if (m_owner != nullptr) {
        if constexpr (uses_node) {
          m_owner->m_lock.unlock(*m_node);
        } else {
          m_owner->m_lock.unlock();
        }
        m_owner = nullptr;
      }
      m_irq.unlock();
    }

    /** @brief Whether this guard still holds the underlying lock (i.e. not yet `unlock()`ed or moved-from). */
    [[nodiscard]] bool is_locked() const noexcept { return m_owner != nullptr; }

    /** @brief The underlying `IrqLocker`, e.g. to mint its `critical_section_token`/`spinlock_entered_token`. */
    [[nodiscard]] const IrqLocker &irq_locker() const noexcept RELOCO_LIFETIMEBOUND { return m_irq; }

    /** @brief Tag selecting the "already acquired" constructors used by `try_lock()`. */
    struct adopt_t {};

    // Public so `reloco::optional<guard>`'s placement-new can reach them
    // from `try_lock()`'s `std::in_place` construction; still effectively
    // unreachable from outside this file, since `adopt_t` itself is a
    // private nested type only `guarded_spin_mutex` (a friend) can name.
    guard(adopt_t, IrqLocker &&irq, guarded_spin_mutex &owner) noexcept : m_irq(std::move(irq)), m_owner(&owner) {}
    guard(adopt_t, IrqLocker &&irq, guarded_spin_mutex &owner, node &n) noexcept
        : m_irq(std::move(irq)), m_owner(&owner), m_node(&n) {}

  private:
    friend class guarded_spin_mutex;

    explicit guard(guarded_spin_mutex &owner) noexcept : m_irq(), m_owner(&owner) { m_owner->m_lock.lock(); }
    guard(guarded_spin_mutex &owner, node &n) noexcept : m_irq(), m_owner(&owner), m_node(&n) {
      m_owner->m_lock.lock(n);
    }

    IrqLocker m_irq;
    guarded_spin_mutex *m_owner{nullptr};
    node *m_node{nullptr};
  };

  /**
   * @brief Engages `IrqLocker`, then blocks (per `Lock::lock()`'s own
   * semantics) until the underlying lock is acquired.
   * @return A `guard` granting access to the protected value until it
   * is destroyed or explicitly `unlock()`ed.
   */
  template <bool B = uses_node, std::enable_if_t<!B, int> = 0> [[nodiscard]] guard lock() & noexcept {
    return guard(*this);
  }

  /** @brief `queue_spin_lock`-shaped overload: enqueues `n`, matching `Lock::lock(node&)`'s own semantics. */
  template <bool B = uses_node, std::enable_if_t<B, int> = 0> [[nodiscard]] guard lock(node &n) & noexcept {
    return guard(*this, n);
  }

  /**
   * @brief Engages `IrqLocker`, then attempts to acquire the underlying
   * lock without spinning. If the underlying `try_lock()` fails,
   * `IrqLocker` is released immediately before returning, so a failed
   * attempt never leaves interrupts/preemption disabled.
   * @return A `guard` if acquired, else `reloco::nullopt`.
   */
  template <bool B = uses_node, std::enable_if_t<!B, int> = 0>
  [[nodiscard]] reloco::optional<guard> try_lock() & noexcept {
    IrqLocker irq;
    if (!m_lock.try_lock()) {
      return reloco::nullopt;
    }
    return reloco::optional<guard>(std::in_place, typename guard::adopt_t{}, std::move(irq), *this);
  }

  /** @brief `queue_spin_lock`-shaped overload of `try_lock()`, matching `Lock::try_lock(node&)`'s own semantics. */
  template <bool B = uses_node, std::enable_if_t<B, int> = 0>
  [[nodiscard]] reloco::optional<guard> try_lock(node &n) & noexcept {
    IrqLocker irq;
    if (!m_lock.try_lock(n)) {
      return reloco::nullopt;
    }
    return reloco::optional<guard>(std::in_place, typename guard::adopt_t{}, std::move(irq), *this, n);
  }

  /**
   * @brief Direct, unguarded mutable access -- sound exactly when the
   * caller already holds an exclusive `guarded_spin_mutex&` (matching
   * `reloco::guarded_mutex::unsafe_get_mut()`, which borrows `&mut self`
   * at compile time instead of taking the lock at runtime). Unlike Rust,
   * C++ has no borrow checker to enforce that exclusivity, so this is
   * named and annotated `unsafe_`: nothing stops a caller from also
   * holding a `guard` live at the same time, racing this access against
   * it.
   */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE T &unsafe_get_mut() & noexcept RELOCO_LIFETIMEBOUND { return m_value; }

private:
  T m_value;
  Lock m_lock;
};

} // namespace structo::sync
