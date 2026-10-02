// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file core_rendezvous_barrier.hpp
 * @brief `structo::sync::core_rendezvous_barrier<Traits>`: a reusable,
 * spin-only SMP rendezvous point -- "wait until exactly `num_cores`
 * cores have arrived, then release them all together" -- for contexts
 * where blocking/parking a core is not an option (early boot, an IPI/
 * NMI handler, a hypervisor monitor with no scheduler to park against).
 *
 * This is deliberately *not* `reloco::barrier`: `reloco::barrier` blocks
 * each waiting *thread* via `futex_wait` (parking it with the OS
 * scheduler, falling back to a mutex/condition-variable "parking lot"
 * where no native futex exists) -- exactly the right choice for ordinary
 * userspace/kernel-thread code, and exactly what `spin_lock.hpp` already
 * explains is unusable in interrupt handlers, before a scheduler exists,
 * or in a panic/fault path. `core_rendezvous_barrier` fills that same gap
 * `spin_lock` fills for a mutex: non-leader cores never park, they spin,
 * invoking a caller-supplied callback once per iteration so the caller
 * can make progress while waiting (feed a watchdog, drain a pending-IPI
 * queue, check for a higher-priority abort) instead of just burning
 * cycles.
 *
 * `Traits` supplies exactly one architecture hook:
 * - `static void spin_wait() noexcept;` -- invoked once per spin
 *   iteration *after* the caller's own on-spin callback, so a concrete
 *   port can substitute a power-efficient wait (Arm `WFE`, x86 `PAUSE`
 *   via `reloco::hint::spin_loop()`, ...) for a plain busy-loop.
 *
 * Like `reloco::barrier`, a *generation* counter (plain `std::atomic`,
 * not `futex_word` -- nothing here ever blocks, so there is nothing to
 * wake) distinguishes the wave a spinning core belongs to, making the
 * barrier safely reusable across an unbounded number of waves: a core
 * that arrives immediately after a release is correctly counted into the
 * next wave rather than racing the one it just missed. `wait()` (and
 * `wait(on_spin)`) return `true` for exactly one arbitrarily-chosen core
 * per wave (the "leader" -- the core whose arrival completed it), `false`
 * for every other participant, matching `reloco::barrier::wait()`'s own
 * leader-election convention; a typical use is having only the leader
 * perform once-per-wave cleanup after an SMP rendezvous (e.g. advancing a
 * shared TLB-shootdown generation once every core has acknowledged it).
 *
 * Example `Traits` (portable `PAUSE`/`YIELD`-style spin hint via
 * `reloco::hint::spin_loop()`):
 * @code
 * struct portable_rendezvous_traits {
 *   static void spin_wait() noexcept { reloco::hint::spin_loop(); }
 * };
 * @endcode
 *
 * Example `Traits` (Arm `WFE`, woken by another core's `SEV` after it
 * updates the generation counter -- a real port would pair this with
 * `cpu_index<Tag>::send_event()` right after the atomic store that
 * advances the generation, so waiters are not left polling until their
 * next scheduled wakeup):
 * @code
 * struct arm_wfe_rendezvous_traits {
 *   static void spin_wait() noexcept { asm volatile("wfe" ::: "memory"); }
 * };
 * @endcode
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace structo::sync {

/**
 * @brief Reusable, spin-only SMP rendezvous point for exactly
 * `num_cores` participants, calling an optional caller-supplied callback
 * once per spin iteration on every core that arrives before the rest.
 * @tparam Traits Architecture policy providing `spin_wait()`.
 */
template <typename Traits> class core_rendezvous_barrier {
public:
  using traits_type = Traits;

  /**
   * @brief Constructs a barrier that releases a wave of spinning cores
   * once exactly @p num_cores of them have called `wait()`/`wait(on_spin)`.
   * A `num_cores` of `0` behaves like `1`, matching `reloco::barrier`'s
   * own documented `0`-participant behavior: every call would otherwise
   * release immediately without ever counting a generation of arrivals.
   */
  explicit core_rendezvous_barrier(std::size_t num_cores) noexcept : m_threshold(num_cores == 0 ? 1 : num_cores) {}

  core_rendezvous_barrier(const core_rendezvous_barrier &) = delete;
  core_rendezvous_barrier &operator=(const core_rendezvous_barrier &) = delete;

  /**
   * @brief Number of cores required to complete one wave of this barrier.
   */
  [[nodiscard]] constexpr std::size_t threshold() const noexcept { return m_threshold; }

  /**
   * @brief Arrives at the barrier and spins until `threshold()` cores
   * (across the lifetime of this barrier, one wave at a time) have
   * arrived, then releases every core in that wave together. Every
   * participant other than the one whose arrival completed the wave
   * (the "leader") calls @p on_spin once per spin iteration while
   * waiting, followed by `Traits::spin_wait()`.
   *
   * Automatically inspects the invocable:
   *   - `[](std::size_t arrived) { ... }` -> receives the current
   *     provisional arrival count for this wave (a lower bound: may be
   *     stale by the time it is observed, useful only as a progress hint)
   *   - `[]() { ... }`                    -> receives no args
   *
   * @tparam F Callable type; invoked with whichever of the two forms above it accepts.
   * @param on_spin Callback invoked once per spin iteration on every
   * non-leader participant; never invoked on the leader, which never spins.
   * @return `true` for exactly one arbitrarily-chosen core per wave (the
   * leader); `false` for every other participant in the same wave.
   */
  template <typename F> bool wait(F &&on_spin) noexcept {
    const std::uint32_t generation = m_generation.load(std::memory_order_acquire);

    if (m_arrived.fetch_add(1, std::memory_order_acq_rel) + 1 == m_threshold) {
      // Last arrival in this wave: become the leader, reset for the next
      // wave, and advance the generation so every spinning core observes
      // the release.
      m_arrived.store(0, std::memory_order_relaxed);
      m_generation.store(generation + 1, std::memory_order_release);
      return true;
    }

    while (m_generation.load(std::memory_order_acquire) == generation) {
      if constexpr (std::is_invocable_v<F, std::size_t>) {
        on_spin(m_arrived.load(std::memory_order_relaxed));
      } else {
        on_spin();
      }
      Traits::spin_wait();
    }
    return false;
  }

  /** @brief `wait(on_spin)` with a no-op spin callback. */
  bool wait() noexcept {
    return wait([] {});
  }

private:
  std::size_t m_threshold;
  std::atomic<std::size_t> m_arrived{0};
  std::atomic<std::uint32_t> m_generation{0};
};

} // namespace structo::sync
