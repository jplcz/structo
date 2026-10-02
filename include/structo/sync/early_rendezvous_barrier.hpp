// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file early_rendezvous_barrier.hpp
 * @brief `structo::sync::early_rendezvous_wait<Traits>`: a thin C++
 * convenience wrapper over `early_rendezvous_barrier.h`'s plain-C
 * `structo_early_rendezvous_wait()`, trading its raw function-pointer +
 * `void *user_data` callback pair for an ordinary (optionally
 * capturing) callable.
 *
 * The underlying state is still exactly `structo_early_rendezvous_state`
 * -- a plain C struct, caller-owned, zero-initialized by the language at
 * static/global storage duration, with no construction step -- so the
 * same placement guidance (and the same "works before any constructor
 * anywhere has run yet" guarantee) from `early_rendezvous_barrier.h`'s
 * file-level docs applies unchanged; this header adds no storage of its
 * own and does not alter that contract.
 *
 * `Traits` supplies exactly one hook (the same contract
 * `core_rendezvous_barrier::Traits` uses):
 * - `static void spin_wait() noexcept;` -- invoked once per spin
 *   iteration *after* the caller's own on-spin callback.
 *
 * Example `Traits`:
 * @code
 * struct early_boot_rendezvous_traits {
 *   static void spin_wait() noexcept { asm volatile("yield" ::: "memory"); }
 * };
 * @endcode
 *
 * @code
 * static structo_early_rendezvous_state g_boot_barrier{};
 *
 * const bool is_leader = structo::sync::early_rendezvous_wait<early_boot_rendezvous_traits>(
 *     g_boot_barrier, num_cores, [](std::size_t arrived) {
 *       // Invoked once per spin iteration on every non-leader core.
 *       service_pending_ipis();
 *     });
 * @endcode
 */

#include "early_rendezvous_barrier.h"

#include <cstddef>
#include <type_traits>

namespace structo::sync {

namespace detail {

/** @brief Trampoline binding `Traits::spin_wait()` to the plain-C `void(*)(void)` hook. */
template <typename Traits> void early_rendezvous_spin_wait_trampoline() noexcept { Traits::spin_wait(); }

/** @brief Trampoline unpacking `user_data` back into `F` and invoking it with whichever arity it accepts. */
template <typename F> void early_rendezvous_on_spin_trampoline(std::size_t arrived, void *user_data) noexcept {
  auto &on_spin = *static_cast<F *>(user_data);
  if constexpr (std::is_invocable_v<F, std::size_t>) {
    on_spin(arrived);
  } else {
    on_spin();
  }
}

} // namespace detail

/**
 * @brief Arrives at `state`'s barrier and spins until @p num_cores
 * participants have called this function, then releases every
 * participant in that wave together. See `structo_early_rendezvous_wait`
 * for the full contract (reusability across waves, leader election,
 * `num_cores` agreement requirement, `0`-is-`1` handling).
 *
 * @tparam Traits Architecture policy providing `spin_wait()`.
 * @tparam F Callable type; invoked once per spin iteration on every
 * non-leader participant, as either `on_spin(std::size_t arrived)` or
 * `on_spin()`, whichever it accepts. Never invoked on the leader.
 * @param state Caller-owned barrier state; every participant in a wave
 * must pass a reference to the *same* `state`.
 * @param num_cores Number of participants in this wave.
 * @param on_spin Callback invoked once per spin iteration while waiting.
 * @return `true` for exactly one arbitrarily-chosen participant per
 * wave (the leader); `false` for every other participant.
 */
template <typename Traits, typename F>
bool early_rendezvous_wait(structo_early_rendezvous_state &state, std::size_t num_cores, F &&on_spin) noexcept {
  using decayed_f = std::decay_t<F>;
  return structo_early_rendezvous_wait(&state, num_cores, &detail::early_rendezvous_spin_wait_trampoline<Traits>,
                                       &detail::early_rendezvous_on_spin_trampoline<decayed_f>,
                                       const_cast<void *>(static_cast<const void *>(&on_spin))) != 0;
}

/** @brief `early_rendezvous_wait<Traits>(state, num_cores, on_spin)` with a no-op spin callback. */
template <typename Traits>
bool early_rendezvous_wait(structo_early_rendezvous_state &state, std::size_t num_cores) noexcept {
  return structo_early_rendezvous_wait(&state, num_cores, &detail::early_rendezvous_spin_wait_trampoline<Traits>,
                                       nullptr, nullptr) != 0;
}

} // namespace structo::sync
