// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file core_pin_guard.hpp
 * @brief `structo::sync::core_pin_guard<Traits>`: an RAII guard that
 * pins the calling thread/task to its current CPU core for its
 * lifetime, in the same spirit as `irq_guard.hpp`/`preemption_guard.hpp`
 * but preventing migration rather than interrupts or preemption.
 *
 * Pinning (e.g. Linux's `get_cpu()`/`put_cpu()`, FreeBSD's
 * `sched_pin()`/`sched_unpin()`) stops the scheduler from migrating the
 * calling context to a different core while a sequence of operations
 * needs to stay bound to one core's identity or its per-CPU state (see
 * `per_cpu_ptr.hpp`) -- it does *not* by itself disable preemption or
 * interrupts; a thread can still be preempted and resumed later, just
 * always on the same core. Combine with `preemption_guard`/`irq_guard`
 * if stronger exclusion is also required.
 *
 * `Traits` supplies the two scheduler hooks (`pin()` / `unpin()`) plus
 * an optional `cpu_id_type` (defaults to `std::size_t`); everything else
 * here (the guard and the functional helper) is scheduler-agnostic and
 * built purely on top of those two hooks. Unlike `irq_guard::Traits`,
 * `pin()` itself reports which CPU got pinned (the guard does not
 * resolve "current CPU" independently -- `Traits` owns that, the same
 * way `cpu_index<Tag>::current()` does for `per_cpu_ptr`). Example
 * `Traits` (a per-thread nesting counter guarding a scheduler pin):
 * @code
 * struct kernel_pin_traits {
 *   using cpu_id_type = std::size_t;
 *
 *   static cpu_id_type pin() noexcept {
 *     if (current_thread().pin_count++ == 0) {
 *       scheduler::disable_migration(current_thread());
 *     }
 *     return hw_current_cpu_index();
 *   }
 *
 *   static void unpin() noexcept {
 *     if (--current_thread().pin_count == 0) {
 *       scheduler::enable_migration(current_thread());
 *     }
 *   }
 * };
 * @endcode
 */

#include <cstddef>
#include <type_traits>
#include <utility>

namespace structo::sync {

namespace detail {

/** @brief Resolves `Traits`'s CPU-id type: `Traits::cpu_id_type` if provided, else `std::size_t`. */
template <typename Traits, typename = void> struct core_pin_traits_cpu_id {
  using type = std::size_t;
};

template <typename Traits> struct core_pin_traits_cpu_id<Traits, std::void_t<typename Traits::cpu_id_type>> {
  using type = typename Traits::cpu_id_type;
};

} // namespace detail

// Forward declarations
template <typename Traits> class core_pin_guard;

// -----------------------------------------------------------------------------
// Scoped RAII Guard
// -----------------------------------------------------------------------------
/**
 * @brief RAII guard that pins the calling thread to its current CPU on
 * entry and releases the pin on exit.
 * @tparam Traits Scheduler policy providing `pin()` (returns the CPU the
 * caller is now pinned to) and `unpin()`.
 */
template <typename Traits> class [[nodiscard]] core_pin_guard {
public:
  using traits_type = Traits;
  using cpu_id_type = typename detail::core_pin_traits_cpu_id<Traits>::type;

  /** @brief Pins the calling thread to its current CPU, recording which CPU that is. */
  core_pin_guard() noexcept : m_cpu(Traits::pin()), m_armed(true) {}

  /** @brief Releases the pin, unless already unlocked/moved-from. */
  ~core_pin_guard() noexcept {
    if (m_armed) {
      Traits::unpin();
    }
  }

  core_pin_guard(const core_pin_guard &) = delete;
  core_pin_guard &operator=(const core_pin_guard &) = delete;

  /** @brief Transfers ownership of the pin; `other` is left disarmed (no-op on destruction). */
  core_pin_guard(core_pin_guard &&other) noexcept : m_cpu(other.m_cpu), m_armed(std::exchange(other.m_armed, false)) {}

  /** @brief Releases this guard's own pin first, then takes over `other`'s state; `other` is left disarmed. */
  core_pin_guard &operator=(core_pin_guard &&other) noexcept {
    if (this != &other) {
      if (m_armed) {
        Traits::unpin();
      }
      m_cpu = other.m_cpu;
      m_armed = std::exchange(other.m_armed, false);
    }
    return *this;
  }

  /**
   * @brief Early explicit release before scope exit.
   * Idempotent: a second call (or destruction afterwards) is a no-op.
   */
  void unlock() noexcept {
    if (m_armed) {
      Traits::unpin();
      m_armed = false;
    }
  }

  /** @brief Whether this guard still holds the pin (i.e. not yet `unlock()`ed or moved-from). */
  [[nodiscard]] constexpr bool is_armed() const noexcept { return m_armed; }

  /**
   * @brief The CPU this guard pinned the calling thread to.
   *
   * Valid for the lifetime of the pin; the caller is guaranteed to keep
   * running on this CPU for as long as `is_armed()` is `true`.
   */
  [[nodiscard]] constexpr cpu_id_type pinned_cpu() const noexcept { return m_cpu; }

private:
  cpu_id_type m_cpu{};
  bool m_armed{false};
};

// -----------------------------------------------------------------------------
// Functional Pinned-Section Executor (`with_cpu_pinned`)
// -----------------------------------------------------------------------------
/**
 * @brief Executes callable pinned to the calling thread's current CPU,
 * releasing the pin on exit.
 *
 * Automatically inspects the invocable:
 *   - `[](auto cpu) { ... }`                      -> receives the pinned `cpu_id_type`
 *   - `[](core_pin_guard<Traits>& guard) { ... }`  -> receives the guard
 *   - `[]() { ... }`                               -> receives no args
 *
 * @tparam Traits Scheduler policy forwarded to the underlying `core_pin_guard<Traits>`.
 * @tparam F      Callable type; invoked with whichever of the three forms above it accepts.
 * @param f Callable to invoke while pinned to one CPU.
 * @return Whatever `f` returns, forwarded unchanged.
 */
template <typename Traits, typename F> decltype(auto) with_cpu_pinned(F &&f) noexcept {
  core_pin_guard<Traits> guard;

  if constexpr (std::is_invocable_v<F, typename core_pin_guard<Traits>::cpu_id_type>) {
    return f(guard.pinned_cpu());
  } else if constexpr (std::is_invocable_v<F, core_pin_guard<Traits> &>) {
    return f(guard);
  } else {
    return f();
  }
}

} // namespace structo::sync
