// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file cpu_load.hpp
 * @brief `structo::cpu_load<Rep, FracBits>`: a caller-owned, per-CPU
 * instantaneous-and-decayed load tracker -- the per-CPU sibling of
 * `load_average.hpp`'s own *systemwide* `load_average`, meant to be
 * embedded directly inside each CPU's own scheduler state (e.g. a field
 * of whatever a `sched.hpp` policy's `PerCpu::get()` resolves) and fed
 * from that CPU's own runqueue depth, so a work-stealing/load-balancing
 * decision (see `work_steal.hpp`) has an actual per-core quantity to
 * compare across CPUs.
 *
 * ## Deliberately *not* just instantiating `load_average` directly
 *
 * `cpu_load` is built on exactly the same tickless,
 * `reloco::fixed_point`-based exponential decay `load_average` already
 * implements (see that header's own docs for the full derivation) -- it
 * holds one internally and forwards `sample()`/`average()` straight to
 * it. The reason this is its own small type, rather than a caller simply
 * instantiating `load_average` once per CPU directly, is that
 * `load_average`'s own docs explicitly warn against exactly that: one
 * instance per CPU answers a *different* question than `/proc/loadavg`
 * does. `cpu_load` exists to *be* that different, equally legitimate
 * question, under its own name and its own doc, so a reader is never
 * left wondering which of the two (deliberately incompatible) usage
 * conventions a given `load_average`-shaped instance in some kernel's
 * source is actually following.
 *
 * ## Two numbers, two different questions
 *
 * - `current()` -- the exact, un-decayed runnable count as of the most
 *   recent `sample()` call: "is there actually a task sitting on this
 *   CPU's queue *right now* to steal?"
 * - `average()` -- the decayed exponential moving average, exactly as
 *   `load_average::value()` computes it: "has this CPU been
 *   *consistently* busier than its neighbors, or is `current()`'s
 *   nonzero count just a transient blip not worth migrating a task
 *   over for?"
 *
 * A work-stealing policy is expected to gate on `current()` (there must
 * be something to actually steal right now) and rank candidate CPUs (or
 * decide whether to bother stealing at all, under a power-saving
 * policy) by `average()`.
 *
 * ## Caller-driven sampling, same as `load_average`
 *
 * Exactly like `load_average`, this is a plain value type with no
 * internal synchronization and no notion of "now" -- `sample(elapsed,
 * runnable_count)` must only ever be called by the one context that
 * owns this CPU's own scheduler state (i.e. only ever from code running
 * *on* that CPU itself, e.g. from inside `enqueue`/`pick_next`/an
 * idle-loop poll), at whatever cadence is convenient; there is no
 * requirement to sample on every single enqueue/dequeue, just often
 * enough that `average()` stays meaningful. A *different* CPU wanting to
 * read this tracker's already-sampled `current()`/`average()` values
 * (e.g. while looking for a steal candidate) only ever reads them, never
 * calls `sample()` itself -- exactly like reading any other foreign
 * CPU's state, any synchronization that read needs against a concurrent
 * local `sample()` is the caller's responsibility (see `work_steal.hpp`).
 *
 * @code
 * struct my_percpu_sched_state {
 *   my_runqueue_type rq;
 *   structo::cpu_load<> load; // this CPU's own tracker; never shared
 *   reloco::instant last_sample;
 * };
 *
 * // Called from code running on this CPU, whenever convenient (e.g.
 * // right before pick_next(), or from an idle-loop poll):
 * void update_load(my_percpu_sched_state &state, reloco::instant now) {
 *   state.load.sample(now - state.last_sample, state.rq.size());
 *   state.last_sample = now;
 * }
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <reloco/duration.hpp>
#include <structo/load_average.hpp>

namespace structo {

/**
 * @brief Per-CPU load tracker: an exact instantaneous runnable count
 * plus a decayed moving average of it, both updated by the same
 * `sample()` call. See the @file-level docs above for the intended
 * one-instance-per-CPU usage and the `current()`/`average()` distinction.
 */
template <typename Rep = std::uint32_t, unsigned FracBits = 11> class cpu_load {
public:
  using fixed = typename load_average<Rep, FracBits>::fixed;

  constexpr cpu_load() noexcept = default;

  /** @brief @p time_constant is the decay window's time constant (see `load_average`'s own ctor docs). */
  explicit constexpr cpu_load(reloco::duration time_constant) noexcept : average_(time_constant) {}

  /**
   * @brief Records @p runnable_count as this CPU's exact current load
   * (`current()`), and folds it into the decayed `average()`, exactly
   * as `load_average::sample` does.
   */
  template <typename Count> constexpr void sample(reloco::duration elapsed, Count runnable_count) noexcept {
    current_ = static_cast<std::size_t>(runnable_count);
    average_.sample(elapsed, runnable_count);
  }

  /** @brief The exact runnable count as of the most recent `sample()` call (0 if never sampled). */
  [[nodiscard]] constexpr std::size_t current() const noexcept { return current_; }

  /** @brief The decayed exponential moving average of every `sample()`'s `runnable_count`, as a `fixed_point`. */
  [[nodiscard]] constexpr fixed average() const noexcept { return average_.value(); }

private:
  std::size_t current_ = 0;
  load_average<Rep, FracBits> average_{};
};

} // namespace structo
