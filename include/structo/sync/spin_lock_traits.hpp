// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file spin_lock_traits.hpp
 * @brief Shared detection trait for whether a spin lock type's
 * `lock()`/`try_lock()`/`unlock()` take a caller-supplied `node&`
 * argument (`queue_spin_lock<Traits>`-shaped, e.g. via its own nested
 * `node`, or `queue_rw_spin_lock<Traits>::node`, an alias of the same)
 * or no arguments at all (`kernel_spin_lock<Traits>`/
 * `ticket_spin_lock<Traits>`/`rw_spin_lock<Traits>`-shaped). Shared by
 * `irq_spin_lock.hpp`/`irq_rw_spin_lock.hpp` and
 * `guarded_spin_mutex.hpp` so each wraps the exact same lock family
 * without duplicating (and risking drift in) this detection logic.
 */

#include <type_traits>

namespace structo::sync::detail {

/** @brief An empty placeholder used where `Lock` has no nested `node` type (i.e. takes no queue node argument). */
struct no_spin_lock_node {};

/** @brief Resolves to `Lock::node` if present, else `no_spin_lock_node`. */
template <typename Lock, typename = void> struct spin_lock_node_type {
  using type = no_spin_lock_node;
};

template <typename Lock> struct spin_lock_node_type<Lock, std::void_t<typename Lock::node>> {
  using type = typename Lock::node;
};

template <typename Lock> using spin_lock_node_t = typename spin_lock_node_type<Lock>::type;

/** @brief Whether `Lock::lock()`/`unlock()` take a caller-supplied `Lock::node&` argument. */
template <typename Lock>
inline constexpr bool spin_lock_uses_node_v = !std::is_same_v<spin_lock_node_t<Lock>, no_spin_lock_node>;

} // namespace structo::sync::detail
