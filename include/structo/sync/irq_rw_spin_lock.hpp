// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file irq_rw_spin_lock.hpp
 * @brief `structo::sync::irq_rw_spin_lock<RwLock, IrqLocker>`: the
 * reader/writer counterpart to `irq_spin_lock<Lock, IrqLocker>` (see
 * `irq_spin_lock.hpp` for the full rationale on *why* this pairing
 * matters and the ordering it guarantees) -- wraps `rw_spin_lock<Traits>`
 * or `queue_rw_spin_lock<Traits>` together with an IRQ/preemption-
 * exclusion locker (`irq_guard<Traits>` or `spinlock_entry_guard<Traits>`)
 * so that taking either the read or write side also disables
 * interrupts (and/or enters a critical section) for exactly as long as
 * that side is held.
 *
 * `IrqLocker` must match the same shape `irq_spin_lock.hpp` documents
 * (default-constructible, engages on construction, idempotent
 * `unlock()`, move-only). `RwLock` is `rw_spin_lock<Traits>` (whose
 * `write_lock()`/`try_write_lock()`/`write_unlock()` take no arguments)
 * or `queue_rw_spin_lock<Traits>` (whose `write_lock(node&)`/
 * `try_write_lock(node&)`/`write_unlock(node&)` take a caller-supplied
 * `node`, used only for the writer side's own MCS admission queue);
 * `irq_rw_spin_lock` detects which shape `RwLock` has (via the presence
 * of a nested `RwLock::node` type) and exposes the matching
 * `write_lock()`/`try_write_lock()` overload automatically. The reader
 * side (`read_lock()`/`try_read_lock()`/`read_unlock()`) never takes a
 * node for either underlying lock type.
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
 * structo::sync::irq_rw_spin_lock<structo::sync::rw_spin_lock<kernel_lock_traits>,
 *                                  structo::sync::irq_guard<arm_irq_traits>>
 *     lock;
 * {
 *   auto g = lock.read_lock(); // interrupts disabled, then a reader slot acquired
 *   // ... any number of concurrent readers, each with interrupts disabled on its own core ...
 * } // reader slot released, then interrupts restored
 * @endcode
 */

#include <structo/sync/spin_lock_traits.hpp>

#include <reloco/detail/compat.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>

#include <type_traits>
#include <utility>

namespace structo::sync {

/**
 * @brief Pairs an IRQ/preemption-exclusion locker with a reader/writer
 * spin lock so taking either side always acquires (and releases) the
 * `IrqLocker` in the correct order; see this file's top-level docs.
 * @tparam RwLock    `rw_spin_lock<Traits>` or `queue_rw_spin_lock<Traits>`.
 * @tparam IrqLocker `irq_guard<Traits>` or `spinlock_entry_guard<Traits>`
 * (or any type matching their shape; see `irq_spin_lock.hpp`).
 */
template <typename RwLock, typename IrqLocker> class irq_rw_spin_lock {
public:
  using lock_type = RwLock;
  using irq_locker_type = IrqLocker;
  using node = detail::spin_lock_node_t<RwLock>;

  /** @brief True if `RwLock`'s writer side requires a caller-supplied `node` (i.e. is `queue_rw_spin_lock`-shaped). */
  static constexpr bool write_uses_node = detail::spin_lock_uses_node_v<RwLock>;

  constexpr irq_rw_spin_lock() noexcept = default;

  irq_rw_spin_lock(const irq_rw_spin_lock &) = delete;
  irq_rw_spin_lock &operator=(const irq_rw_spin_lock &) = delete;

  /**
   * @brief RAII handle returned by `read_lock()`/`try_read_lock()`:
   * holds both the engaged `IrqLocker` and the acquired reader slot for
   * its lifetime, releasing the reader slot first and the `IrqLocker`
   * second on destruction (or early `unlock()`).
   */
  class [[nodiscard]] RELOCO_POINTER read_guard {
  public:
    ~read_guard() noexcept { unlock(); }

    read_guard(const read_guard &) = delete;
    read_guard &operator=(const read_guard &) = delete;

    read_guard(read_guard &&other) noexcept
        : m_irq(std::move(other.m_irq)), m_lock(std::exchange(other.m_lock, nullptr)) {}

    read_guard &operator=(read_guard &&other) noexcept {
      if (this != &other) {
        unlock();
        m_irq = std::move(other.m_irq);
        m_lock = std::exchange(other.m_lock, nullptr);
      }
      return *this;
    }

    /** @brief Early explicit release before scope exit; idempotent. */
    void unlock() noexcept {
      if (m_lock != nullptr) {
        m_lock->read_unlock();
        m_lock = nullptr;
      }
      m_irq.unlock();
    }

    /** @brief Whether this guard still holds the reader slot (i.e. not yet `unlock()`ed or moved-from). */
    [[nodiscard]] bool is_locked() const noexcept { return m_lock != nullptr; }

    /** @brief The underlying `IrqLocker`, e.g. to mint its `critical_section_token`/`spinlock_entered_token`. */
    [[nodiscard]] const IrqLocker &irq_locker() const noexcept RELOCO_LIFETIMEBOUND { return m_irq; }

    /** @brief Tag selecting the "already acquired" constructor used by `try_read_lock()`. */
    struct adopt_t {};

    // Public so `reloco::optional<read_guard>`'s placement-new can reach it
    // from `try_read_lock()`'s `std::in_place` construction; still
    // effectively unreachable from outside this file, since `adopt_t` itself
    // is a private nested type only `irq_rw_spin_lock` (a friend) can name.
    read_guard(adopt_t, IrqLocker &&irq, RwLock &lock) noexcept : m_irq(std::move(irq)), m_lock(&lock) {}

  private:
    friend class irq_rw_spin_lock;

    explicit read_guard(RwLock &lock) noexcept : m_irq(), m_lock(&lock) { m_lock->read_lock(); }

    IrqLocker m_irq;
    RwLock *m_lock{nullptr};
  };

  /**
   * @brief RAII handle returned by `write_lock()`/`try_write_lock()`:
   * holds both the engaged `IrqLocker` and the acquired writer side for
   * its lifetime, releasing the writer side first and the `IrqLocker`
   * second on destruction (or early `unlock()`).
   */
  class [[nodiscard]] RELOCO_POINTER write_guard {
  public:
    ~write_guard() noexcept { unlock(); }

    write_guard(const write_guard &) = delete;
    write_guard &operator=(const write_guard &) = delete;

    write_guard(write_guard &&other) noexcept
        : m_irq(std::move(other.m_irq)), m_lock(std::exchange(other.m_lock, nullptr)),
          m_node(std::exchange(other.m_node, nullptr)) {}

    write_guard &operator=(write_guard &&other) noexcept {
      if (this != &other) {
        unlock();
        m_irq = std::move(other.m_irq);
        m_lock = std::exchange(other.m_lock, nullptr);
        m_node = std::exchange(other.m_node, nullptr);
      }
      return *this;
    }

    /** @brief Early explicit release before scope exit; idempotent. */
    void unlock() noexcept {
      if (m_lock != nullptr) {
        if constexpr (write_uses_node) {
          m_lock->write_unlock(*m_node);
        } else {
          m_lock->write_unlock();
        }
        m_lock = nullptr;
      }
      m_irq.unlock();
    }

    /** @brief Whether this guard still holds the writer side (i.e. not yet `unlock()`ed or moved-from). */
    [[nodiscard]] bool is_locked() const noexcept { return m_lock != nullptr; }

    /** @brief The underlying `IrqLocker`, e.g. to mint its `critical_section_token`/`spinlock_entered_token`. */
    [[nodiscard]] const IrqLocker &irq_locker() const noexcept RELOCO_LIFETIMEBOUND { return m_irq; }

    /** @brief Tag selecting the "already acquired" constructors used by `try_write_lock()`. */
    struct adopt_t {};

    // Public so `reloco::optional<write_guard>`'s placement-new can reach
    // them from `try_write_lock()`'s `std::in_place` construction; still
    // effectively unreachable from outside this file, since `adopt_t` itself
    // is a private nested type only `irq_rw_spin_lock` (a friend) can name.
    write_guard(adopt_t, IrqLocker &&irq, RwLock &lock) noexcept : m_irq(std::move(irq)), m_lock(&lock) {}
    write_guard(adopt_t, IrqLocker &&irq, RwLock &lock, node &n) noexcept
        : m_irq(std::move(irq)), m_lock(&lock), m_node(&n) {}

  private:
    friend class irq_rw_spin_lock;

    explicit write_guard(RwLock &lock) noexcept : m_irq(), m_lock(&lock) { m_lock->write_lock(); }
    write_guard(RwLock &lock, node &n) noexcept : m_irq(), m_lock(&lock), m_node(&n) { m_lock->write_lock(n); }

    IrqLocker m_irq;
    RwLock *m_lock{nullptr};
    node *m_node{nullptr};
  };

  /** @brief Engages `IrqLocker`, then blocks until a shared (read) slot is acquired; see `RwLock::read_lock()`. */
  [[nodiscard]] read_guard read_lock() & noexcept { return read_guard(m_lock); }

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
    return reloco::optional<read_guard>(std::in_place, typename read_guard::adopt_t{}, std::move(irq), m_lock);
  }

  /** @brief Engages `IrqLocker`, then blocks until the exclusive (write) side is acquired; see `RwLock::write_lock()`.
   */
  template <bool B = write_uses_node, std::enable_if_t<!B, int> = 0> [[nodiscard]] write_guard write_lock() & noexcept {
    return write_guard(m_lock);
  }

  /** @brief `queue_rw_spin_lock`-shaped overload: enqueues `n` onto the writer admission queue; see
   * `RwLock::write_lock(node&)`. */
  template <bool B = write_uses_node, std::enable_if_t<B, int> = 0>
  [[nodiscard]] write_guard write_lock(node &n) & noexcept {
    return write_guard(m_lock, n);
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
    return reloco::optional<write_guard>(std::in_place, typename write_guard::adopt_t{}, std::move(irq), m_lock);
  }

  /** @brief `queue_rw_spin_lock`-shaped overload of `try_write_lock()`; see `RwLock::try_write_lock(node&)`. */
  template <bool B = write_uses_node, std::enable_if_t<B, int> = 0>
  [[nodiscard]] reloco::optional<write_guard> try_write_lock(node &n) & noexcept {
    IrqLocker irq;
    if (!m_lock.try_write_lock(n)) {
      return reloco::nullopt;
    }
    return reloco::optional<write_guard>(std::in_place, typename write_guard::adopt_t{}, std::move(irq), m_lock, n);
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

  /** @brief Direct access to the underlying `RwLock`, e.g. for diagnostics that don't fit this wrapper's API. */
  [[nodiscard]] RwLock &underlying() noexcept RELOCO_LIFETIMEBOUND { return m_lock; }
  /** @brief `const`-qualified overload of `underlying()`. */
  [[nodiscard]] const RwLock &underlying() const noexcept RELOCO_LIFETIMEBOUND { return m_lock; }

private:
  RwLock m_lock;
};

} // namespace structo::sync
