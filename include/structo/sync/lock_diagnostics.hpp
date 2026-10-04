// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file lock_diagnostics.hpp
 * @brief Shared, optional diagnostic hooks for every lock type in
 * `structo::sync`'s spin lock family (`kernel_spin_lock`,
 * `ticket_spin_lock`, `queue_spin_lock`, `rw_spin_lock`,
 * `queue_rw_spin_lock`): a `Traits` policy may optionally supply a lock
 * *name* and a *panic handler*. Neither is required -- absent either
 * one, every lock in this family traps exactly as it always has, via a
 * plain `RELOCO_ASSERT` with a fixed, static message, at zero additional
 * runtime cost (the whole mechanism folds away under `if constexpr`).
 *
 * ## `Traits::name(self)` -- optional, per-instance
 *
 * A real kernel usually has many instances of the exact same lock type
 * sharing one `Traits` -- one `kernel_spin_lock<generic_traits>` per
 * run queue, one per device, etc -- so a single compile-time name baked
 * into `Traits` itself couldn't tell them apart. `name(self)` instead
 * receives the lock object itself by reference, letting `Traits` look
 * up (e.g. via a registry keyed on `&self`) or otherwise derive a name
 * specific to *that* instance:
 * @code
 * struct kernel_lock_traits {
 *   using owner_type = thread *;
 *   static owner_type current_owner() noexcept { return get_current_thread(); }
 *
 *   template <typename Self>
 *   static const char *name(const Self &self) noexcept {
 *     return lock_registry::lookup_name(&self);
 *   }
 * };
 * @endcode
 *
 * ## `Traits::panic(reason, name)` -- optional
 *
 * When absent, every trap in this family falls back to a plain
 * `RELOCO_ASSERT(cond, reason)` (today's exact behavior). When present,
 * `panic(reason, name)` is called instead of that `RELOCO_ASSERT`:
 * @p reason is the same fixed, human-readable description the plain
 * assert would have used (e.g. `"kernel_spin_lock: destroyed while
 * still held"`), and @p name is whatever `Traits::name(self)` resolved
 * to (or `nullptr` if `Traits` doesn't provide `name()` at all). Both
 * are plain `const char *` -- formatting, logging, and panicking are
 * entirely `Traits`' own responsibility (e.g. via microfmt, straight to
 * a kernel panic facility, etc). `panic()` is expected to never return;
 * the calling macro traps via `RELOCO_TRAP()` immediately afterwards
 * regardless, as a safety net.
 * @code
 * struct kernel_lock_traits {
 *   // ... owner_type / current_owner() / name() as above ...
 *
 *   [[noreturn]] static void panic(const char *reason, const char *name) noexcept {
 *     kernel_panic("lock '%s': %s", name ? name : "<unnamed>", reason);
 *   }
 * };
 * @endcode
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>

#include <type_traits>
#include <utility>

namespace structo::sync::detail {

/** @brief True if `Traits::name(self)` is callable for a given `Self`. */
template <typename Traits, typename Self, typename = void> struct has_lock_name : std::false_type {};

template <typename Traits, typename Self>
struct has_lock_name<Traits, Self, std::void_t<decltype(Traits::name(std::declval<const Self &>()))>> : std::true_type {
};

/** @brief True if `Traits::panic(reason, name)` is callable. */
template <typename Traits, typename = void> struct has_lock_panic : std::false_type {};

template <typename Traits>
struct has_lock_panic<Traits,
                      std::void_t<decltype(Traits::panic(std::declval<const char *>(), std::declval<const char *>()))>>
    : std::true_type {};

/**
 * @brief Resolves `Traits::name(self)` if provided, else `nullptr`.
 * Shared by every lock type in this family's trap sites.
 */
template <typename Traits, typename Self> [[nodiscard]] const char *lock_name(const Self &self) noexcept {
  if constexpr (has_lock_name<Traits, Self>::value) {
    return Traits::name(self);
  } else {
    return nullptr;
  }
}

} // namespace structo::sync::detail

/**
 * @brief Traps unless @p cond holds, consistent with `RELOCO_ASSERT`.
 *
 * If `Traits::panic(reason, name)` is provided, resolves @p self's name
 * via `structo::sync::detail::lock_name<Traits>` and calls
 * `Traits::panic` with it instead of the plain assert path; otherwise
 * behaves exactly like `RELOCO_ASSERT(cond, reason)` (today's behavior,
 * unchanged).
 * @param Traits The lock's `Traits` policy type.
 * @param self   The lock instance the trap pertains to (typically `*this`).
 * @param cond   Condition that must hold; traps if false.
 * @param reason Fixed, human-readable failure description.
 */
#define STRUCTO_SYNC_LOCK_ASSERT(Traits, self, cond, reason)                                                           \
  do {                                                                                                                 \
    if constexpr (::structo::sync::detail::has_lock_panic<Traits>::value) {                                            \
      if (!(cond))                                                                                                     \
        RELOCO_UNLIKELY {                                                                                              \
          Traits::panic((reason), ::structo::sync::detail::lock_name<Traits>(self));                                   \
          RELOCO_TRAP();                                                                                               \
        }                                                                                                              \
    } else {                                                                                                           \
      RELOCO_ASSERT(cond, reason);                                                                                     \
    }                                                                                                                  \
  } while (0)
