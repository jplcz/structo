// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file irq_guard.hpp
 * @brief `structo::sync::irq_guard<Traits>`: an RAII interrupt-disable
 * guard plus the supporting `critical_section_token`/`with_irq_disabled`/
 * `irq_locked<T>` building blocks for writing interrupt-safe kernel code
 * without manually pairing `hw_save_irqs()`/`hw_restore_irqs()` calls.
 *
 * `Traits` supplies the two architecture hooks (`hw_save_irqs()` /
 * `hw_restore_irqs(flags)`) plus a `flags_type`; everything else here
 * (the guard, the proof token, the functional helper, and the
 * `Mutex<RefCell<T>>`-style data wrapper) is architecture-agnostic and
 * built purely on top of those two hooks.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <type_traits>
#include <utility>

namespace structo::sync {

// Forward declarations
template <typename Traits> class irq_guard;

// -----------------------------------------------------------------------------
// Rust-Style Proof Token (Zero-Sized Type)
// -----------------------------------------------------------------------------
/**
 * @brief Zero-sized proof token representing an active critical section.
 *
 * Cannot be forged or default-constructed by arbitrary code. Can only be
 * obtained by holding an active irq_guard or inside with_irq_disabled().
 * Callee functions requiring interrupts to be masked can demand this token:
 *
 *   void modify_scheduler_queue(critical_section_token cs);
 */
class critical_section_token {
private:
  struct private_tag {};
  explicit constexpr critical_section_token(private_tag) noexcept {}

  template <typename Traits> friend class irq_guard;

  template <typename Traits, typename F> friend decltype(auto) with_irq_disabled(F &&f) noexcept;
};

// -----------------------------------------------------------------------------
// Scoped RAII Guard
// -----------------------------------------------------------------------------
/**
 * @brief RAII guard that disables interrupts on entry and restores them on exit.
 * @tparam Traits Architecture policy providing `flags_type`, `hw_save_irqs()`,
 * and `hw_restore_irqs(flags_type)`.
 */
template <typename Traits> class [[nodiscard]] irq_guard {
public:
  using traits_type = Traits;
  using flags_type = typename Traits::flags_type;

  /** @brief Disables interrupts, saving the prior flags for later restoration. */
  irq_guard() noexcept : m_flags(Traits::hw_save_irqs()), m_armed(true) {}

  /** @brief Restores the previously-saved interrupt flags, unless already unlocked/moved-from. */
  ~irq_guard() noexcept {
    if (m_armed) {
      Traits::hw_restore_irqs(m_flags);
    }
  }

  irq_guard(const irq_guard &) = delete;
  irq_guard &operator=(const irq_guard &) = delete;

  /** @brief Transfers ownership of the saved flags; `other` is left disarmed (no-op on destruction). */
  irq_guard(irq_guard &&other) noexcept : m_flags(other.m_flags), m_armed(std::exchange(other.m_armed, false)) {}

  /** @brief Restores this guard's own flags first, then takes over `other`'s state; `other` is left disarmed. */
  irq_guard &operator=(irq_guard &&other) noexcept {
    if (this != &other) {
      if (m_armed) {
        Traits::hw_restore_irqs(m_flags);
      }
      m_flags = other.m_flags;
      m_armed = std::exchange(other.m_armed, false);
    }
    return *this;
  }

  /**
   * @brief Early explicit restore before scope exit.
   * Idempotent: a second call (or destruction afterwards) is a no-op.
   */
  void unlock() noexcept {
    if (m_armed) {
      Traits::hw_restore_irqs(m_flags);
      m_armed = false;
    }
  }

  /** @brief Whether this guard still holds interrupts disabled (i.e. not yet `unlock()`ed or moved-from). */
  [[nodiscard]] constexpr bool is_armed() const noexcept { return m_armed; }
  /** @brief The interrupt flags captured at construction, to be restored on unlock/destruction. */
  [[nodiscard]] constexpr flags_type saved_flags() const noexcept { return m_flags; }

  /**
   * @brief Mint a zero-sized token proving critical section status.
   * @return A `critical_section_token` attesting that interrupts are
   * currently disabled by this guard.
   */
  [[nodiscard]] critical_section_token token() const noexcept {
    return critical_section_token{critical_section_token::private_tag{}};
  }

private:
  flags_type m_flags{};
  bool m_armed{false};
};

// -----------------------------------------------------------------------------
// Functional Critical Section Executor (`critical_section::with`)
// -----------------------------------------------------------------------------
/**
 * @brief Executes callable with interrupts disabled, restoring previous state on exit.
 *
 * Automatically inspects the invocable:
 *   - `[](critical_section_token cs) { ... }` -> receives proof token
 *   - `[](irq_guard<Traits>& guard) { ... }`  -> receives guard
 *   - `[]() { ... }`                          -> receives no args
 *
 * @tparam Traits Architecture policy forwarded to the underlying `irq_guard<Traits>`.
 * @tparam F      Callable type; invoked with whichever of the three forms above it accepts.
 * @param f Callable to invoke with interrupts disabled.
 * @return Whatever `f` returns, forwarded unchanged.
 */
template <typename Traits, typename F> decltype(auto) with_irq_disabled(F &&f) noexcept {
  irq_guard<Traits> guard;

  if constexpr (std::is_invocable_v<F, critical_section_token>) {
    return f(guard.token());
  } else if constexpr (std::is_invocable_v<F, irq_guard<Traits> &>) {
    return f(guard);
  } else {
    return f();
  }
}

// -----------------------------------------------------------------------------
// Rust-Style Data Wrapper (`irq_locked<T>`)
// -----------------------------------------------------------------------------
/**
 * @brief Container encapsulating data accessible ONLY with interrupts disabled.
 *
 * Mirrors Rust's `critical_section::Mutex<RefCell<T>>`.
 * @tparam T      Protected value type.
 * @tparam Traits Architecture policy forwarded to the underlying `irq_guard<Traits>`.
 */
template <typename T, typename Traits> class RELOCO_OWNER irq_locked {
public:
  using value_type = T;
  using traits_type = Traits;

  /** @brief Constructs the protected value in place, forwarding `args` to `T`'s constructor. */
  template <typename... Args>
  constexpr explicit irq_locked(Args &&...args) noexcept : m_value(std::forward<Args>(args)...) {}

  irq_locked(const irq_locked &) = delete;
  irq_locked &operator=(const irq_locked &) = delete;

  // ---------------------------------------------------------------------------
  // RAII Locked Reference
  // ---------------------------------------------------------------------------
  /**
   * @brief RAII handle returned by `irq_locked::lock()`: disables interrupts
   * for its lifetime and grants pointer/reference access to the protected `T`.
   */
  class [[nodiscard]] RELOCO_POINTER guard {
  public:
    /** @brief Disables interrupts and binds to `owner`'s protected value. */
    explicit guard(irq_locked &owner) noexcept : m_irq_guard(), m_value(owner.m_value) {}

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
     * @brief Mint a zero-sized token proving critical section status.
     * @return A `critical_section_token` attesting that interrupts are
     * currently disabled by this guard.
     */
    [[nodiscard]] critical_section_token token() const noexcept { return m_irq_guard.token(); }

  private:
    irq_guard<Traits> m_irq_guard;
    T &m_value;
  };

  /**
   * @brief Disables IRQs and returns an RAII handle granting access to T.
   * @return A `guard` holding interrupts disabled until it is destroyed or
   * explicitly `unlock()`ed via its underlying `irq_guard`.
   */
  [[nodiscard]] guard lock() noexcept { return guard(*this); }

  /**
   * @brief Fast-path zero-cost borrow using a preexisting critical section token.
   *
   * If already inside an active critical section, accesses T with ZERO
   * CPSID/MSR instructions.
   * @param (unnamed) Proof that interrupts are already disabled by an
   * enclosing `irq_guard`/`with_irq_disabled` scope.
   * @return Reference to the protected value, valid for as long as
   * interrupts remain disabled.
   */
  [[nodiscard]] T &borrow(critical_section_token) noexcept RELOCO_LIFETIMEBOUND { return m_value; }

  /** @brief `const`-qualified overload of `borrow(critical_section_token)`. */
  [[nodiscard]] const T &borrow(critical_section_token) const noexcept RELOCO_LIFETIMEBOUND { return m_value; }

  /**
   * @brief Scoped accessor passing T directly to a closure.
   *
   * Automatically inspects the invocable:
   *   - `[](T& value, critical_section_token cs) { ... }` -> receives value + proof token
   *   - `[](T& value) { ... }`                             -> receives value only
   *
   * @tparam F Callable type; invoked with whichever of the two forms above it accepts.
   * @param f Callable to invoke with interrupts disabled and access to the protected value.
   * @return Whatever `f` returns, forwarded unchanged.
   */
  template <typename F> decltype(auto) with_lock(F &&f) noexcept {
    auto g = lock();
    if constexpr (std::is_invocable_v<F, T &, critical_section_token>) {
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