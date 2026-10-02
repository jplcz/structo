// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

/**
 * @file early_rendezvous_barrier.h
 * @brief Plain C (C99+), GCC/Clang-only SMP rendezvous barrier for the
 * earliest boot phase -- before `.init_array`/global constructors have
 * run, possibly before any stack beyond a per-core startup stack
 * exists, and certainly before anything resembling `new`/an allocator
 * is available.
 *
 * @details
 * All state this barrier needs is the plain, standard-layout
 * `struct structo_early_rendezvous_state` below -- two `unsigned int`
 * fields, nothing else. There is no construction step: a tentative
 * definition of this struct at static/global storage duration (in C or
 * C++, `extern`-declared or not) is zero-initialized by the
 * language long before any dynamic initialization could run, so a
 * secondary core jumping directly into this function at its reset
 * vector, with no guarantee any constructor anywhere has executed yet,
 * can still call `structo_early_rendezvous_wait()` safely. The caller
 * owns the storage (typically a single `static struct
 * structo_early_rendezvous_state` global, optionally placed into a
 * specific linker section via `__attribute__((section(...)))` if it
 * must live in identity-mapped/uncached memory before the MMU/cache is
 * configured identically on every core) and passes a pointer to it in;
 * this header never allocates or owns any barrier state itself.
 *
 * Atomicity is implemented directly with GCC/Clang's `__atomic_*`
 * builtins (`__ATOMIC_*` orderings), not `<stdatomic.h>`'s
 * `atomic_uint`/`atomic_fetch_add` (equivalent in effect, but spelling
 * it directly in terms of the compiler builtins avoids relying on
 * `<stdatomic.h>` having been included -- or even being available, on
 * some freestanding toolchains -- this early, and keeps the struct's
 * two fields as plain `unsigned int`s rather than `atomic_uint`
 * objects). This header is GCC/Clang-only (`jplcz_structo` targets
 * GCC/Clang exclusively; no MSVC `_Interlocked*` fallback is provided).
 *
 * `num_cores` is not stored anywhere -- it is a plain argument to
 * `structo_early_rendezvous_wait()` every time, supplied by the caller
 * (typically a compile-time constant, or a value read out of an early
 * devicetree/ACPI table). Every participant for one wave must pass the
 * same `num_cores`; this header does not (and, with no stored
 * configuration to check it against, cannot) verify that itself. `0` is
 * treated as `1`.
 *
 * `spin_wait`/`on_spin` are plain function pointers (C has no
 * lambdas/templates): `spin_wait` is the architecture hook invoked once
 * per spin iteration (e.g. Arm `WFE`, x86 `PAUSE`); `on_spin` is an
 * optional caller callback, also invoked once per spin iteration
 * (*before* `spin_wait`), receiving the current provisional arrival
 * count and an opaque `user_data` pointer -- useful for making progress
 * while waiting (feed a watchdog, drain a pending-IPI queue) instead of
 * just burning cycles. Neither is invoked on the leader (the participant
 * whose arrival completed the wave), which never spins. Either may be
 * `NULL`.
 *
 * @code
 * static struct structo_early_rendezvous_state g_boot_barrier;
 *
 * static void arch_spin_hint(void) { __asm__ __volatile__("yield" ::: "memory"); }
 *
 * void secondary_core_entry(size_t num_cores) {
 *   const int is_leader = structo_early_rendezvous_wait(&g_boot_barrier, num_cores, arch_spin_hint, NULL, NULL);
 *   if (is_leader) {
 *     // Exactly one arbitrarily-chosen core runs this once per wave.
 *   }
 * }
 * @endcode
 *
 * For a C++ wrapper offering lambda-based `on_spin` callbacks instead of
 * a raw function pointer + `user_data`, see `early_rendezvous_barrier.hpp`.
 */

#pragma once

#if !defined(__GNUC__) && !defined(__clang__)
#error "early_rendezvous_barrier.h requires GCC or Clang (__atomic_* builtins)"
#endif

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Barrier state: two plain `unsigned int`s, zero-initialized by
 * the language at static/global storage duration -- no construction
 * step, no other members. See this file's top-level docs for placement
 * guidance (section attributes, etc.).
 */
struct structo_early_rendezvous_state {
  unsigned int arrived;
  unsigned int generation;
};

/** @brief Designated-initializer-friendly zero value, equivalent to the implicit zero-initialization of a static
 * instance. */
#define STRUCTO_EARLY_RENDEZVOUS_STATE_INIT {0u, 0u}

/**
 * @brief Arrives at the barrier and spins until @p num_cores
 * participants (across the lifetime of @p state, one wave at a time)
 * have called this function, then releases every participant in that
 * wave together.
 *
 * @param state Caller-owned barrier state; every participant in a wave
 * must pass a pointer to the *same* `state`.
 * @param num_cores Number of participants in this wave; every caller for
 * the same wave must agree on this value. `0` is treated as `1`.
 * @param spin_wait Architecture spin-wait hint, invoked once per spin
 * iteration on every non-leader participant, after `on_spin`. May be
 * `NULL` (no hint emitted).
 * @param on_spin Optional callback invoked once per spin iteration on
 * every non-leader participant, receiving the current provisional
 * arrival count (a lower bound: may be stale by the time it is
 * observed, useful only as a progress hint) and @p user_data. May be
 * `NULL`.
 * @param user_data Opaque pointer forwarded to every @p on_spin call
 * unchanged.
 * @return Nonzero for exactly one arbitrarily-chosen participant per
 * wave (the "leader" -- the one whose arrival completed it); `0` for
 * every other participant in the same wave.
 */
static inline int structo_early_rendezvous_wait(struct structo_early_rendezvous_state *state, size_t num_cores,
                                                void (*spin_wait)(void),
                                                void (*on_spin)(size_t arrived, void *user_data), void *user_data) {
  const unsigned int threshold = (num_cores == 0) ? 1u : (unsigned int)num_cores;
  const unsigned int generation = __atomic_load_n(&state->generation, __ATOMIC_ACQUIRE);

  if (__atomic_add_fetch(&state->arrived, 1u, __ATOMIC_ACQ_REL) == threshold) {
    /* Last arrival in this wave: become the leader, reset for the next
     * wave, and advance the generation so every spinning participant
     * observes the release. */
    __atomic_store_n(&state->arrived, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&state->generation, generation + 1u, __ATOMIC_RELEASE);
    return 1;
  }

  while (__atomic_load_n(&state->generation, __ATOMIC_ACQUIRE) == generation) {
    if (on_spin != NULL) {
      on_spin((size_t)__atomic_load_n(&state->arrived, __ATOMIC_RELAXED), user_data);
    }
    if (spin_wait != NULL) {
      spin_wait();
    }
  }
  return 0;
}

#ifdef __cplusplus
}
#endif
