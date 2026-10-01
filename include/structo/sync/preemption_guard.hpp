// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file preemption_guard.hpp
 * @brief `structo::sync::preemption_guard<Traits>`: an RAII
 * preemption-disable guard plus the supporting
 * `preemption_disabled_token`/`with_preemption_disabled`/
 * `preempt_locked<T>` building blocks, in the same spirit as
 * `irq_guard.hpp` but for scheduler preemption rather than interrupts.
 *
 * `Traits` supplies two architecture/scheduler hooks
 * (`disable_preemption()` / `enable_preemption()`); everything else here
 * (the guard, the proof token, the functional helper, and the
 * `Mutex<RefCell<T>>`-style data wrapper) is scheduler-agnostic and
 * built purely on top of those two hooks.
 *
 * Unlike `irq_guard::Traits`, there is no `flags_type` to save/restore:
 * preemption disable/enable is expected to be a simple nestable counter
 * (e.g. Linux's per-CPU `preempt_count`, FreeBSD's `td_critnest`), so
 * `enable_preemption()` takes no argument -- the scheduler's own counter
 * tracks nesting, not this guard. Example `Traits` (a per-CPU nesting
 * counter plus a reschedule check on the outermost `enable_preemption()`):
 * @code
 * struct kernel_preempt_traits {
 *   static void disable_preemption() noexcept {
 *     ++current_cpu().preempt_count;
 *   }
 *
 *   static void enable_preemption() noexcept {
 *     if (--current_cpu().preempt_count == 0 && current_cpu().resched_pending) {
 *       scheduler::reschedule();
 *     }
 *   }
 * };
 * @endcode
 */

#include <reloco/lifetime.hpp>
#include <type_traits>
#include <utility>

namespace structo::sync {

// Forward declarations
template <typename Traits> class preemption_guard;

// -----------------------------------------------------------------------------
// Rust-Style Proof Token (Zero-Sized Type)
// -----------------------------------------------------------------------------
/**
 * @brief Zero-sized proof token representing an active preemption-disabled section.
 *
 * Cannot be forged or default-constructed by arbitrary code. Can only be
 * obtained by holding an active preemption_guard or inside
 * with_preemption_disabled(). Callee functions requiring preemption to be
 * disabled can demand this token:
 *
 *   void touch_run_queue(preemption_disabled_token np);
 */
class preemption_disabled_token {
private:
  struct private_tag {};
  explicit constexpr preemption_disabled_token(private_tag) noexcept {}

  template <typename Traits> friend class preemption_guard;

  template <typename Traits, typename F>
  friend decltype(auto) with_preemption_disabled(F &&f) noexcept;
};

// -----------------------------------------------------------------------------
// Scoped RAII Guard
// -----------------------------------------------------------------------------
/**
 * @brief RAII guard that disables preemption on entry and re-enables it on exit.
 * @tparam Traits Scheduler policy providing `disable_preemption()` and
 * `enable_preemption()`.
 */
template <typename Traits> class [[nodiscard]] preemption_guard {
public:
  using traits_type = Traits;

  /** @brief Disables preemption. */
  preemption_guard() noexcept : m_armed(true) { Traits::disable_preemption(); }

  /** @brief Re-enables preemption, unless already unlocked/moved-from. */
  ~preemption_guard() noexcept {
    if (m_armed) {
      Traits::enable_preemption();
    }
  }

  preemption_guard(const preemption_guard &) = delete;
  preemption_guard &operator=(const preemption_guard &) = delete;

  /** @brief Transfers ownership of the disabled section; `other` is left disarmed (no-op on destruction). */
  preemption_guard(preemption_guard &&other) noexcept : m_armed(std::exchange(other.m_armed, false)) {}

  /** @brief Re-enables this guard's own section first, then takes over `other`'s state; `other` is left disarmed. */
  preemption_guard &operator=(preemption_guard &&other) noexcept {
    if (this != &other) {
      if (m_armed) {
        Traits::enable_preemption();
      }
      m_armed = std::exchange(other.m_armed, false);
    }
    return *this;
  }

  /**
   * @brief Early explicit re-enable before scope exit.
   * Idempotent: a second call (or destruction afterwards) is a no-op.
   */
  void unlock() noexcept {
    if (m_armed) {
      Traits::enable_preemption();
      m_armed = false;
    }
  }

  /** @brief Whether this guard still holds preemption disabled (i.e. not yet `unlock()`ed or moved-from). */
  [[nodiscard]] constexpr bool is_armed() const noexcept { return m_armed; }

  /**
   * @brief Mint a zero-sized token proving preemption-disabled status.
   * @return A `preemption_disabled_token` attesting that preemption is
   * currently disabled by this guard.
   */
  [[nodiscard]] preemption_disabled_token token() const noexcept {
    return preemption_disabled_token{preemption_disabled_token::private_tag{}};
  }

private:
  bool m_armed{false};
};

// -----------------------------------------------------------------------------
// Functional Critical Section Executor (`with_preemption_disabled`)
// -----------------------------------------------------------------------------
/**
 * @brief Executes callable with preemption disabled, re-enabling it on exit.
 *
 * Automatically inspects the invocable:
 *   - `[](preemption_disabled_token np) { ... }`   -> receives proof token
 *   - `[](preemption_guard<Traits>& guard) { ... }` -> receives guard
 *   - `[]() { ... }`                                -> receives no args
 *
 * @tparam Traits Scheduler policy forwarded to the underlying `preemption_guard<Traits>`.
 * @tparam F      Callable type; invoked with whichever of the three forms above it accepts.
 * @param f Callable to invoke with preemption disabled.
 * @return Whatever `f` returns, forwarded unchanged.
 */
template <typename Traits, typename F> decltype(auto) with_preemption_disabled(F &&f) noexcept {
  preemption_guard<Traits> guard;

  if constexpr (std::is_invocable_v<F, preemption_disabled_token>) {
    return f(guard.token());
  } else if constexpr (std::is_invocable_v<F, preemption_guard<Traits> &>) {
    return f(guard);
  } else {
    return f();
  }
}

// -----------------------------------------------------------------------------
// Rust-Style Data Wrapper (`preempt_locked<T>`)
// -----------------------------------------------------------------------------
/**
 * @brief Container encapsulating data accessible ONLY with preemption disabled.
 *
 * Mirrors `irq_locked<T, Traits>`, but guards against scheduler
 * preemption (e.g. a run-queue or per-CPU scheduling structure that must
 * not be migrated away from underneath the accessor) rather than
 * interrupts.
 * @tparam T      Protected value type.
 * @tparam Traits Scheduler policy forwarded to the underlying `preemption_guard<Traits>`.
 */
template <typename T, typename Traits> class RELOCO_OWNER preempt_locked {
public:
  using value_type = T;
  using traits_type = Traits;

  /** @brief Constructs the protected value in place, forwarding `args` to `T`'s constructor. */
  template <typename... Args>
  constexpr explicit preempt_locked(Args &&...args) noexcept : m_value(std::forward<Args>(args)...) {}

  preempt_locked(const preempt_locked &) = delete;
  preempt_locked &operator=(const preempt_locked &) = delete;

  // ---------------------------------------------------------------------------
  // RAII Locked Reference
  // ---------------------------------------------------------------------------
  /**
   * @brief RAII handle returned by `preempt_locked::lock()`: disables
   * preemption for its lifetime and grants pointer/reference access to
   * the protected `T`.
   */
  class [[nodiscard]] RELOCO_POINTER guard {
  public:
    /** @brief Disables preemption and binds to `owner`'s protected value. */
    explicit guard(preempt_locked &owner) noexcept : m_preempt_guard(), m_value(owner.m_value) {}

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
     * @brief Mint a zero-sized token proving preemption-disabled status.
     * @return A `preemption_disabled_token` attesting that preemption is
     * currently disabled by this guard.
     */
    [[nodiscard]] preemption_disabled_token token() const noexcept { return m_preempt_guard.token(); }

  private:
    preemption_guard<Traits> m_preempt_guard;
    T &m_value;
  };

  /**
   * @brief Disables preemption and returns an RAII handle granting access to T.
   * @return A `guard` holding preemption disabled until it is destroyed or
   * explicitly `unlock()`ed via its underlying `preemption_guard`.
   */
  [[nodiscard]] guard lock() noexcept { return guard(*this); }

  /**
   * @brief Fast-path zero-cost borrow using a preexisting proof token.
   *
   * If already inside an active preemption-disabled section, accesses T
   * with zero additional disable/enable calls.
   * @param (unnamed) Proof that preemption is already disabled by an
   * enclosing `preemption_guard`/`with_preemption_disabled` scope.
   * @return Reference to the protected value, valid for as long as
   * preemption remains disabled.
   */
  [[nodiscard]] T &borrow(preemption_disabled_token) noexcept RELOCO_LIFETIMEBOUND { return m_value; }

  /** @brief `const`-qualified overload of `borrow(preemption_disabled_token)`. */
  [[nodiscard]] const T &borrow(preemption_disabled_token) const noexcept RELOCO_LIFETIMEBOUND { return m_value; }

  /**
   * @brief Scoped accessor passing T directly to a closure.
   *
   * Automatically inspects the invocable:
   *   - `[](T& value, preemption_disabled_token np) { ... }` -> receives value + proof token
   *   - `[](T& value) { ... }`                                -> receives value only
   *
   * @tparam F Callable type; invoked with whichever of the two forms above it accepts.
   * @param f Callable to invoke with preemption disabled and access to the protected value.
   * @return Whatever `f` returns, forwarded unchanged.
   */
  template <typename F> decltype(auto) with_lock(F &&f) noexcept {
    auto g = lock();
    if constexpr (std::is_invocable_v<F, T &, preemption_disabled_token>) {
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
