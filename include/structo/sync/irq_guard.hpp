// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

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
 */
template <typename Traits> class [[nodiscard]] irq_guard {
public:
  using traits_type = Traits;
  using flags_type = typename Traits::flags_type;

  irq_guard() noexcept : m_flags(Traits::hw_save_irqs()), m_armed(true) {}

  ~irq_guard() noexcept {
    if (m_armed) {
      Traits::hw_restore_irqs(m_flags);
    }
  }

  irq_guard(const irq_guard &) = delete;
  irq_guard &operator=(const irq_guard &) = delete;

  irq_guard(irq_guard &&other) noexcept : m_flags(other.m_flags), m_armed(std::exchange(other.m_armed, false)) {}

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
   */
  void unlock() noexcept {
    if (m_armed) {
      Traits::hw_restore_irqs(m_flags);
      m_armed = false;
    }
  }

  [[nodiscard]] constexpr bool is_armed() const noexcept { return m_armed; }
  [[nodiscard]] constexpr flags_type saved_flags() const noexcept { return m_flags; }

  /**
   * @brief Mint a zero-sized token proving critical section status.
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
 */
template <typename T, typename Traits> class RELOCO_OWNER irq_locked {
public:
  using value_type = T;
  using traits_type = Traits;

  template <typename... Args>
  constexpr explicit irq_locked(Args &&...args) noexcept : m_value(std::forward<Args>(args)...) {}

  irq_locked(const irq_locked &) = delete;
  irq_locked &operator=(const irq_locked &) = delete;

  // ---------------------------------------------------------------------------
  // RAII Locked Reference
  // ---------------------------------------------------------------------------
  class [[nodiscard]] RELOCO_POINTER guard {
  public:
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

    [[nodiscard]] critical_section_token token() const noexcept { return m_irq_guard.token(); }

  private:
    irq_guard<Traits> m_irq_guard;
    T &m_value;
  };

  /**
   * @brief Disables IRQs and returns an RAII handle granting access to T.
   */
  [[nodiscard]] guard lock() noexcept { return guard(*this); }

  /**
   * @brief Fast-path zero-cost borrow using a preexisting critical section token.
   *
   * If already inside an active critical section, accesses T with ZERO
   * CPSID/MSR instructions.
   */
  [[nodiscard]] T &borrow(critical_section_token) noexcept RELOCO_LIFETIMEBOUND { return m_value; }

  [[nodiscard]] const T &borrow(critical_section_token) const noexcept RELOCO_LIFETIMEBOUND { return m_value; }

  /**
   * @brief Scoped accessor passing T directly to a closure.
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