// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file guarded_rw_spin_mutex.hpp
 * @brief `structo::sync::guarded_rw_spin_mutex<T, RwLock, IrqLocker>`:
 * the reader/writer counterpart to `guarded_spin_mutex<T, Lock,
 * IrqLocker>` (see `guarded_spin_mutex.hpp` for the full rationale on
 * *why* a data-owning mutex matters and how it differs from
 * `irq_rw_spin_lock<RwLock, IrqLocker>`) -- the protected `T` lives
 * *inside* the mutex itself, reachable only through the RAII
 * `read_guard`/`write_guard` returned by `read_lock()`/`write_lock()`/
 * their `try_` counterparts.
 *
 * `reloco::guarded_mutex<T, MutexT>` has no reader/writer equivalent to
 * substitute a plain structo lock into, so this header -- like
 * `guarded_spin_mutex.hpp` -- is a standalone implementation that does
 * not wrap or depend on `irq_rw_spin_lock.hpp`; the two serve different
 * purposes (a bare, value-less lock wrapper the caller still owns, vs.
 * a mutex that owns both the lock and the value it protects).
 *
 * `RwLock` is `rw_spin_lock<Traits>` (whose `write_lock()`/
 * `try_write_lock()`/`write_unlock()` take no arguments) or
 * `queue_rw_spin_lock<Traits>` (whose `write_lock(node&)`/
 * `try_write_lock(node&)`/`write_unlock(node&)` take a caller-supplied
 * `node`, used only for the writer side's own MCS admission queue);
 * `guarded_rw_spin_mutex` detects which shape `RwLock` has (via the
 * presence of a nested `RwLock::node` type) and exposes the matching
 * `write_lock()`/`try_write_lock()` overload automatically. The reader
 * side (`read_lock()`/`try_read_lock()`) never takes a node for either
 * underlying lock type.
 *
 * `IrqLocker` is any default-constructible RAII type matching
 * `irq_guard<Traits>`/`spinlock_entry_guard<Traits>`'s shape (engages
 * on construction, releases on destruction unless already released, is
 * move-only, exposes an idempotent `unlock()`); it defaults to a
 * zero-cost no-op (`detail::no_irq_locker`, shared with
 * `guarded_spin_mutex.hpp`) for callers who need none.
 *
 * @code
 * struct kernel_lock_traits {
 *   using owner_type = std::uintptr_t;
 *   static owner_type current_owner() noexcept { return get_current_thread_id(); }
 * };
 *
 * // A plain reader/writer spin lock, no IRQ masking:
 * structo::sync::guarded_rw_spin_mutex<int, structo::sync::rw_spin_lock<kernel_lock_traits>> counter;
 * {
 *   auto g = counter.read_lock();
 *   int seen = *g;
 * }
 * {
 *   auto g = counter.write_lock();
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
 * // IRQ-safe: interrupts disabled for the duration either side is held.
 * structo::sync::guarded_rw_spin_mutex<int, structo::sync::rw_spin_lock<kernel_lock_traits>,
 *                                       structo::sync::irq_guard<arm_irq_traits>>
 *     irq_safe_counter;
 *
 * // queue_rw_spin_lock-shaped: write_lock()/try_write_lock() take a caller-supplied node.
 * structo::sync::guarded_rw_spin_mutex<int, structo::sync::queue_rw_spin_lock<kernel_lock_traits>,
 *                                       structo::sync::irq_guard<arm_irq_traits>>
 *     queued_counter;
 * decltype(queued_counter)::node n;
 * {
 *   auto g = queued_counter.write_lock(n);
 *   *g += 1;
 * }
 * @endcode
 */

#include <structo/sync/guarded_spin_mutex.hpp>
#include <structo/sync/spin_lock_traits.hpp>

#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>

#include <type_traits>
#include <utility>

namespace structo::sync {

/**
 * @brief Owns a value `T` behind a reader/writer spin lock (optionally
 * paired with an IRQ/preemption-exclusion locker); see this file's
 * top-level docs.
 * @tparam T         Protected value type.
 * @tparam RwLock    `rw_spin_lock<Traits>` or `queue_rw_spin_lock<Traits>`.
 * @tparam IrqLocker `irq_guard<Traits>` or `spinlock_entry_guard<Traits>`
 * (or any type matching their shape); defaults to a no-op
 * (`detail::no_irq_locker`) when no IRQ/preemption exclusion is needed.
 */
template <typename T, typename RwLock, typename IrqLocker = detail::no_irq_locker>
class RELOCO_OWNER guarded_rw_spin_mutex {
public:
  using value_type = T;
  using lock_type = RwLock;
  using irq_locker_type = IrqLocker;
  using node = detail::spin_lock_node_t<RwLock>;

  /** @brief True if `RwLock`'s writer side requires a caller-supplied `node` (i.e. is `queue_rw_spin_lock`-shaped). */
  static constexpr bool write_uses_node = detail::spin_lock_uses_node_v<RwLock>;

  /** @brief Default-constructs the protected value. */
  constexpr guarded_rw_spin_mutex() noexcept(std::is_nothrow_default_constructible_v<T>) : m_value() {}
  /** @brief Constructs the protected value by moving @p value into it. */
  constexpr explicit guarded_rw_spin_mutex(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
      : m_value(std::move(value)) {}

  guarded_rw_spin_mutex(const guarded_rw_spin_mutex &) = delete;
  guarded_rw_spin_mutex &operator=(const guarded_rw_spin_mutex &) = delete;

  /**
   * @brief RAII handle returned by `read_lock()`/`try_read_lock()`:
   * holds both the engaged `IrqLocker` and the acquired reader slot for
   * its lifetime, granting read-only access to the protected `T` via
   * `operator*`/`operator->`. Releases the reader slot first and the
   * `IrqLocker` second on destruction (or early `unlock()`).
   */
  class [[nodiscard]] RELOCO_POINTER read_guard {
  public:
    ~read_guard() noexcept { unlock(); }

    read_guard(const read_guard &) = delete;
    read_guard &operator=(const read_guard &) = delete;

    /** @brief Transfers ownership of both the `IrqLocker` and the held reader slot; `other` is left a no-op on
     * destruction. */
    read_guard(read_guard &&other) noexcept
        : m_irq(std::move(other.m_irq)), m_owner(std::exchange(other.m_owner, nullptr)) {}

    /** @brief Releases this guard's own state first, then takes over `other`'s; `other` is left a no-op on destruction.
     */
    read_guard &operator=(read_guard &&other) noexcept {
      if (this != &other) {
        unlock();
        m_irq = std::move(other.m_irq);
        m_owner = std::exchange(other.m_owner, nullptr);
      }
      return *this;
    }

    [[nodiscard]] const T &operator*() const noexcept RELOCO_LIFETIMEBOUND { return m_owner->m_value; }
    [[nodiscard]] const T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return &m_owner->m_value; }

    /** @brief Read-only access to the protected value; equivalent to `*this` with `const` emphasized at the call site.
     */
    [[nodiscard]] const T &get() const noexcept RELOCO_LIFETIMEBOUND { return m_owner->m_value; }

    /** @brief Early explicit release before scope exit: releases the reader slot first, then the `IrqLocker`;
     * idempotent. */
    void unlock() noexcept {
      if (m_owner != nullptr) {
        m_owner->m_lock.read_unlock();
        m_owner = nullptr;
      }
      m_irq.unlock();
    }

    /** @brief Whether this guard still holds the reader slot (i.e. not yet `unlock()`ed or moved-from). */
    [[nodiscard]] bool is_locked() const noexcept { return m_owner != nullptr; }

    /** @brief The underlying `IrqLocker`, e.g. to mint its `critical_section_token`/`spinlock_entered_token`. */
    [[nodiscard]] const IrqLocker &irq_locker() const noexcept RELOCO_LIFETIMEBOUND { return m_irq; }

    /** @brief Tag selecting the "already acquired" constructor used by `try_read_lock()`. */
    struct adopt_t {};

    // Public so `reloco::optional<read_guard>`'s placement-new can reach it
    // from `try_read_lock()`'s `std::in_place` construction; still
    // effectively unreachable from outside this file, since `adopt_t` itself
    // is a private nested type only `guarded_rw_spin_mutex` (a friend) can name.
    read_guard(adopt_t, IrqLocker &&irq, guarded_rw_spin_mutex &owner) noexcept
        : m_irq(std::move(irq)), m_owner(&owner) {}

  private:
    friend class guarded_rw_spin_mutex;

    explicit read_guard(guarded_rw_spin_mutex &owner) noexcept : m_irq(), m_owner(&owner) {
      m_owner->m_lock.read_lock();
    }

    IrqLocker m_irq;
    guarded_rw_spin_mutex *m_owner{nullptr};
  };

  /**
   * @brief RAII handle returned by `write_lock()`/`try_write_lock()`:
   * holds both the engaged `IrqLocker` and the acquired writer side for
   * its lifetime, granting mutable access to the protected `T` via
   * `operator*`/`operator->`. Releases the writer side first and the
   * `IrqLocker` second on destruction (or early `unlock()`).
   */
  class [[nodiscard]] RELOCO_POINTER write_guard {
  public:
    ~write_guard() noexcept { unlock(); }

    write_guard(const write_guard &) = delete;
    write_guard &operator=(const write_guard &) = delete;

    /** @brief Transfers ownership of both the `IrqLocker` and the held writer side; `other` is left a no-op on
     * destruction. */
    write_guard(write_guard &&other) noexcept
        : m_irq(std::move(other.m_irq)), m_owner(std::exchange(other.m_owner, nullptr)),
          m_node(std::exchange(other.m_node, nullptr)) {}

    /** @brief Releases this guard's own state first, then takes over `other`'s; `other` is left a no-op on destruction.
     */
    write_guard &operator=(write_guard &&other) noexcept {
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

    /** @brief Early explicit release before scope exit: releases the writer side first, then the `IrqLocker`;
     * idempotent. */
    void unlock() noexcept {
      if (m_owner != nullptr) {
        if constexpr (write_uses_node) {
          m_owner->m_lock.write_unlock(*m_node);
        } else {
          m_owner->m_lock.write_unlock();
        }
        m_owner = nullptr;
      }
      m_irq.unlock();
    }

    /** @brief Whether this guard still holds the writer side (i.e. not yet `unlock()`ed or moved-from). */
    [[nodiscard]] bool is_locked() const noexcept { return m_owner != nullptr; }

    /** @brief The underlying `IrqLocker`, e.g. to mint its `critical_section_token`/`spinlock_entered_token`. */
    [[nodiscard]] const IrqLocker &irq_locker() const noexcept RELOCO_LIFETIMEBOUND { return m_irq; }

    /** @brief Tag selecting the "already acquired" constructors used by `try_write_lock()`. */
    struct adopt_t {};

    // Public so `reloco::optional<write_guard>`'s placement-new can reach
    // them from `try_write_lock()`'s `std::in_place` construction; still
    // effectively unreachable from outside this file, since `adopt_t` itself
    // is a private nested type only `guarded_rw_spin_mutex` (a friend) can name.
    write_guard(adopt_t, IrqLocker &&irq, guarded_rw_spin_mutex &owner) noexcept
        : m_irq(std::move(irq)), m_owner(&owner) {}
    write_guard(adopt_t, IrqLocker &&irq, guarded_rw_spin_mutex &owner, node &n) noexcept
        : m_irq(std::move(irq)), m_owner(&owner), m_node(&n) {}

  private:
    friend class guarded_rw_spin_mutex;

    explicit write_guard(guarded_rw_spin_mutex &owner) noexcept : m_irq(), m_owner(&owner) {
      m_owner->m_lock.write_lock();
    }
    write_guard(guarded_rw_spin_mutex &owner, node &n) noexcept : m_irq(), m_owner(&owner), m_node(&n) {
      m_owner->m_lock.write_lock(n);
    }

    IrqLocker m_irq;
    guarded_rw_spin_mutex *m_owner{nullptr};
    node *m_node{nullptr};
  };

  /** @brief Engages `IrqLocker`, then blocks until a shared (read) slot is acquired; see `RwLock::read_lock()`. */
  [[nodiscard]] read_guard read_lock() & noexcept { return read_guard(*this); }

  /**
   * @brief Engages `IrqLocker`, then attempts to acquire a shared
   * (read) slot without spinning; see `RwLock::try_read_lock()`. If
   * that fails, `IrqLocker` is released immediately before returning.
   */
  [[nodiscard]] reloco::optional<read_guard> try_read_lock() & noexcept {
    IrqLocker irq;
    if (!m_lock.try_read_lock()) {
      return reloco::nullopt;
    }
    return reloco::optional<read_guard>(std::in_place, typename read_guard::adopt_t{}, std::move(irq), *this);
  }

  /** @brief Engages `IrqLocker`, then blocks until the exclusive (write) side is acquired; see `RwLock::write_lock()`.
   */
  template <bool B = write_uses_node, std::enable_if_t<!B, int> = 0> [[nodiscard]] write_guard write_lock() & noexcept {
    return write_guard(*this);
  }

  /** @brief `queue_rw_spin_lock`-shaped overload: enqueues `n` onto the writer admission queue; see
   * `RwLock::write_lock(node&)`. */
  template <bool B = write_uses_node, std::enable_if_t<B, int> = 0>
  [[nodiscard]] write_guard write_lock(node &n) & noexcept {
    return write_guard(*this, n);
  }

  /**
   * @brief Engages `IrqLocker`, then attempts to acquire the exclusive
   * (write) side without spinning; see `RwLock::try_write_lock()`. If
   * that fails, `IrqLocker` is released immediately before returning.
   */
  template <bool B = write_uses_node, std::enable_if_t<!B, int> = 0>
  [[nodiscard]] reloco::optional<write_guard> try_write_lock() & noexcept {
    IrqLocker irq;
    if (!m_lock.try_write_lock()) {
      return reloco::nullopt;
    }
    return reloco::optional<write_guard>(std::in_place, typename write_guard::adopt_t{}, std::move(irq), *this);
  }

  /** @brief `queue_rw_spin_lock`-shaped overload of `try_write_lock()`; see `RwLock::try_write_lock(node&)`. */
  template <bool B = write_uses_node, std::enable_if_t<B, int> = 0>
  [[nodiscard]] reloco::optional<write_guard> try_write_lock(node &n) & noexcept {
    IrqLocker irq;
    if (!m_lock.try_write_lock(n)) {
      return reloco::nullopt;
    }
    return reloco::optional<write_guard>(std::in_place, typename write_guard::adopt_t{}, std::move(irq), *this, n);
  }

  /** @brief Best-effort snapshot of the number of readers currently holding the lock; see `RwLock::reader_count()`. */
  [[nodiscard]] auto reader_count() const noexcept { return m_lock.reader_count(); }

  /** @brief Best-effort snapshot of whether any reader currently holds the lock; see `RwLock::is_read_locked()`. */
  [[nodiscard]] bool is_read_locked() const noexcept { return m_lock.is_read_locked(); }

  /** @brief Best-effort snapshot of whether the writer currently holds the lock; see `RwLock::is_write_locked()`. */
  [[nodiscard]] bool is_write_locked() const noexcept { return m_lock.is_write_locked(); }

  /** @brief Best-effort snapshot of whether the lock is held at all; see `RwLock::is_locked()`. */
  [[nodiscard]] bool is_locked() const noexcept { return m_lock.is_locked(); }

  /** @brief Whether the calling context currently holds the writer side; see `RwLock::is_write_locked_by_current()`. */
  [[nodiscard]] bool is_write_locked_by_current() const noexcept { return m_lock.is_write_locked_by_current(); }

  /**
   * @brief Direct, unguarded mutable access -- sound exactly when the
   * caller already holds an exclusive `guarded_rw_spin_mutex&`
   * (matching `reloco::guarded_mutex::unsafe_get_mut()`, which borrows
   * `&mut self` at compile time instead of taking the lock at runtime).
   * Unlike Rust, C++ has no borrow checker to enforce that exclusivity,
   * so this is named and annotated `unsafe_`: nothing stops a caller
   * from also holding a `read_guard`/`write_guard` live at the same
   * time, racing this access against it.
   */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE T &unsafe_get_mut() & noexcept RELOCO_LIFETIMEBOUND { return m_value; }

private:
  T m_value;
  RwLock m_lock;
};

} // namespace structo::sync
