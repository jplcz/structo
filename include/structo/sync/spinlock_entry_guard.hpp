// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file spinlock_entry_guard.hpp
 * @brief `structo::sync::spinlock_entry_guard<Traits>`: an RAII guard
 * plus the supporting `spinlock_entered_token`/`with_spinlock_entered`/
 * `spinlock_entered_locked<T>` building blocks, in the same spirit as
 * `preemption_guard.hpp` but wired to FreeBSD-style
 * `spinlock_enter()`/`spinlock_exit()` rather than a plain preemption
 * counter.
 *
 * ## What `spinlock_enter()`/`spinlock_exit()` actually do
 *
 * FreeBSD's real `spinlock_enter(9)`/`spinlock_exit(9)` disable
 * interrupts *and* enter a critical section (preventing preemption) as
 * one combined, nestable unit -- the exact pair of things a true
 * "spin" mutex (`mtx_lock_spin(9)`) needs held around its own
 * lock/unlock so that neither an interrupt handler nor the scheduler
 * can run on this core while it is spinning for (or holding) the
 * mutex, which would otherwise risk a cross-core deadlock against
 * whichever other core is also spinning for the same lock. Critically,
 * nesting and interrupt-flag saving are both handled *inside* those two
 * calls themselves (FreeBSD stores a per-thread nesting count and the
 * outermost saved interrupt flags on `struct thread`), not by whatever
 * calls them -- so, unlike `irq_guard::Traits`, there is no
 * `flags_type` here either: `Traits::spinlock_enter()`/
 * `spinlock_exit()` take no arguments and return nothing, exactly
 * mirroring the real functions' signatures, in the same way
 * `preemption_guard::Traits` mirrors a plain nestable preempt-count.
 *
 * Example `Traits` (delegating straight to a FreeBSD-shaped backend):
 * @code
 * struct kernel_spinlock_entry_traits {
 *   static void spinlock_enter() noexcept { ::spinlock_enter(); }
 *   static void spinlock_exit() noexcept { ::spinlock_exit(); }
 * };
 *
 * structo::sync::spinlock_entry_guard<kernel_spinlock_entry_traits> guard;
 * real_spin_mutex.lock();
 * // ... protected section, safe from both interrupts and preemption ...
 * real_spin_mutex.unlock();
 * @endcode
 *
 * This header only provides the RAII wrapper around entering/exiting
 * that combined section -- it is deliberately *not* a lock itself (no
 * `Traits::current_owner()`, no queueing); pair it with
 * `kernel_spin_lock`/`ticket_spin_lock`/`queue_spin_lock` (or a real
 * kernel's own spin mutex) for the actual mutual exclusion, the same
 * way FreeBSD's `mtx_lock_spin()` calls `spinlock_enter()` internally
 * before ever touching the mutex's own lock word.
 */

#include <reloco/lifetime.hpp>
#include <type_traits>
#include <utility>

namespace structo::sync {

// Forward declarations
template <typename Traits> class spinlock_entry_guard;

// -----------------------------------------------------------------------------
// Rust-Style Proof Token (Zero-Sized Type)
// -----------------------------------------------------------------------------
/**
 * @brief Zero-sized proof token representing an active
 * `spinlock_enter()`-ed section (interrupts disabled, critical section
 * entered).
 *
 * Cannot be forged or default-constructed by arbitrary code. Can only be
 * obtained by holding an active spinlock_entry_guard or inside
 * with_spinlock_entered(). Callee functions requiring the section to
 * already be entered can demand this token:
 *
 *   void touch_spin_protected_state(spinlock_entered_token se);
 */
class spinlock_entered_token {
private:
  struct private_tag {};
  explicit constexpr spinlock_entered_token(private_tag) noexcept {}

  template <typename Traits> friend class spinlock_entry_guard;

  template <typename Traits, typename F> friend decltype(auto) with_spinlock_entered(F &&f) noexcept;
};

// -----------------------------------------------------------------------------
// Scoped RAII Guard
// -----------------------------------------------------------------------------
/**
 * @brief RAII guard that calls `Traits::spinlock_enter()` on entry and
 * `Traits::spinlock_exit()` on exit.
 * @tparam Traits Kernel policy providing `spinlock_enter()` and
 * `spinlock_exit()`; see this file's top-level docs.
 */
template <typename Traits> class [[nodiscard]] spinlock_entry_guard {
public:
  using traits_type = Traits;

  /** @brief Calls `Traits::spinlock_enter()`. */
  spinlock_entry_guard() noexcept : m_armed(true) { Traits::spinlock_enter(); }

  /** @brief Calls `Traits::spinlock_exit()`, unless already unlocked/moved-from. */
  ~spinlock_entry_guard() noexcept {
    if (m_armed) {
      Traits::spinlock_exit();
    }
  }

  spinlock_entry_guard(const spinlock_entry_guard &) = delete;
  spinlock_entry_guard &operator=(const spinlock_entry_guard &) = delete;

  /** @brief Transfers ownership of the entered section; `other` is left disarmed (no-op on destruction). */
  spinlock_entry_guard(spinlock_entry_guard &&other) noexcept : m_armed(std::exchange(other.m_armed, false)) {}

  /** @brief Exits this guard's own section first, then takes over `other`'s state; `other` is left disarmed. */
  spinlock_entry_guard &operator=(spinlock_entry_guard &&other) noexcept {
    if (this != &other) {
      if (m_armed) {
        Traits::spinlock_exit();
      }
      m_armed = std::exchange(other.m_armed, false);
    }
    return *this;
  }

  /**
   * @brief Early explicit exit before scope exit.
   * Idempotent: a second call (or destruction afterwards) is a no-op.
   */
  void unlock() noexcept {
    if (m_armed) {
      Traits::spinlock_exit();
      m_armed = false;
    }
  }

  /** @brief Whether this guard still holds the section entered (i.e. not yet `unlock()`ed or moved-from). */
  [[nodiscard]] constexpr bool is_armed() const noexcept { return m_armed; }

  /**
   * @brief Mint a zero-sized token proving the section is entered.
   * @return A `spinlock_entered_token` attesting that
   * `spinlock_enter()` is currently in effect because of this guard.
   */
  [[nodiscard]] spinlock_entered_token token() const noexcept {
    return spinlock_entered_token{spinlock_entered_token::private_tag{}};
  }

private:
  bool m_armed{false};
};

// -----------------------------------------------------------------------------
// Functional Critical Section Executor (`with_spinlock_entered`)
// -----------------------------------------------------------------------------
/**
 * @brief Executes callable with the section entered, exiting it on exit.
 *
 * Automatically inspects the invocable:
 *   - `[](spinlock_entered_token se) { ... }`            -> receives proof token
 *   - `[](spinlock_entry_guard<Traits>& guard) { ... }` -> receives guard
 *   - `[]() { ... }`                                      -> receives no args
 *
 * @tparam Traits Kernel policy forwarded to the underlying `spinlock_entry_guard<Traits>`.
 * @tparam F      Callable type; invoked with whichever of the three forms above it accepts.
 * @param f Callable to invoke with the section entered.
 * @return Whatever `f` returns, forwarded unchanged.
 */
template <typename Traits, typename F> decltype(auto) with_spinlock_entered(F &&f) noexcept {
  spinlock_entry_guard<Traits> guard;

  if constexpr (std::is_invocable_v<F, spinlock_entered_token>) {
    return f(guard.token());
  } else if constexpr (std::is_invocable_v<F, spinlock_entry_guard<Traits> &>) {
    return f(guard);
  } else {
    return f();
  }
}

// -----------------------------------------------------------------------------
// Rust-Style Data Wrapper (`spinlock_entered_locked<T>`)
// -----------------------------------------------------------------------------
/**
 * @brief Container encapsulating data accessible ONLY with the
 * `spinlock_enter()` section active.
 *
 * Mirrors `preempt_locked<T, Traits>`, but for data that must be safe
 * from both an interrupt handler and the scheduler on this core at
 * once -- e.g. state a real spin mutex's own critical section touches
 * immediately after acquiring it.
 * @tparam T      Protected value type.
 * @tparam Traits Kernel policy forwarded to the underlying `spinlock_entry_guard<Traits>`.
 */
template <typename T, typename Traits> class RELOCO_OWNER spinlock_entered_locked {
public:
  using value_type = T;
  using traits_type = Traits;

  /** @brief Constructs the protected value in place, forwarding `args` to `T`'s constructor. */
  template <typename... Args>
  constexpr explicit spinlock_entered_locked(Args &&...args) noexcept : m_value(std::forward<Args>(args)...) {}

  spinlock_entered_locked(const spinlock_entered_locked &) = delete;
  spinlock_entered_locked &operator=(const spinlock_entered_locked &) = delete;

  // ---------------------------------------------------------------------------
  // RAII Locked Reference
  // ---------------------------------------------------------------------------
  /**
   * @brief RAII handle returned by `spinlock_entered_locked::lock()`:
   * enters the section for its lifetime and grants pointer/reference
   * access to the protected `T`.
   */
  class [[nodiscard]] RELOCO_POINTER guard {
  public:
    /** @brief Enters the section and binds to `owner`'s protected value. */
    explicit guard(spinlock_entered_locked &owner) noexcept : m_entry_guard(), m_value(owner.m_value) {}

    ~guard() noexcept = default;

    guard(const guard &) = delete;
    guard &operator=(const guard &) = delete;

    guard(guard &&) noexcept = default;
    guard &operator=(guard &&) noexcept = default;

    [[nodiscard]] T *operator->() noexcept RELOCO_LIFETIMEBOUND { return &m_value; }
    [[nodiscard]] const T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return &m_value; }

    [[nodiscard]] T &operator*() noexcept RELOCO_LIFETIMEBOUND { return m_value; }
    [[nodiscard]] const T &operator*() const noexcept RELOCO_LIFETIMEBOUND { return m_value; }

    /**
     * @brief Mint a zero-sized token proving the section is entered.
     * @return A `spinlock_entered_token` attesting that
     * `spinlock_enter()` is currently in effect because of this guard.
     */
    [[nodiscard]] spinlock_entered_token token() const noexcept { return m_entry_guard.token(); }

  private:
    spinlock_entry_guard<Traits> m_entry_guard;
    T &m_value;
  };

  /**
   * @brief Enters the section and returns an RAII handle granting access to T.
   * @return A `guard` holding the section entered until it is destroyed
   * or explicitly `unlock()`ed via its underlying `spinlock_entry_guard`.
   */
  [[nodiscard]] guard lock() noexcept { return guard(*this); }

  /**
   * @brief Fast-path zero-cost borrow using a preexisting proof token.
   *
   * If already inside an active entered section, accesses T with zero
   * additional enter/exit calls.
   * @param (unnamed) Proof that the section is already entered by an
   * enclosing `spinlock_entry_guard`/`with_spinlock_entered` scope.
   * @return Reference to the protected value, valid for as long as the
   * section remains entered.
   */
  [[nodiscard]] T &borrow(spinlock_entered_token) noexcept RELOCO_LIFETIMEBOUND { return m_value; }

  /** @brief `const`-qualified overload of `borrow(spinlock_entered_token)`. */
  [[nodiscard]] const T &borrow(spinlock_entered_token) const noexcept RELOCO_LIFETIMEBOUND { return m_value; }

  /**
   * @brief Scoped accessor passing T directly to a closure.
   *
   * Automatically inspects the invocable:
   *   - `[](T& value, spinlock_entered_token se) { ... }` -> receives value + proof token
   *   - `[](T& value) { ... }`                              -> receives value only
   *
   * @tparam F Callable type; invoked with whichever of the two forms above it accepts.
   * @param f Callable to invoke with the section entered and access to the protected value.
   * @return Whatever `f` returns, forwarded unchanged.
   */
  template <typename F> decltype(auto) with_lock(F &&f) noexcept {
    auto g = lock();
    if constexpr (std::is_invocable_v<F, T &, spinlock_entered_token>) {
      return f(*g, g.token());
    } else if constexpr (std::is_invocable_v<F, T &>) {
      return f(*g);
    } else {
      return f();
    }
  }

private:
  T m_value;
};

} // namespace structo::sync
