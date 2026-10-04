// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file load_average.hpp
 * @brief `structo::load_average<Rep, FracBits>`: a single, Linux-
 * `calc_load()`-style exponentially-decayed moving average of an
 * "active count" (runnable tasks, in-flight requests, ...), and
 * `structo::unix_load_average<Rep, FracBits>`, the classic Unix
 * `/proc/loadavg` triple of 1/5/15-minute averages built on top of it.
 *
 * ## Caller-driven (tickless) sampling, not a fixed periodic cadence
 *
 * Linux's own `calc_load()` samples at a fixed 5-second cadence, which
 * lets it precompute one constant decay factor per averaging window
 * (`EXP_1`/`EXP_5`/`EXP_15`) once and reuse it forever. This header
 * makes no such assumption: `sample(elapsed, active_count)` takes the
 * actual `reloco::duration` elapsed since the previous sample, and
 * recomputes `decay = exp(-elapsed/time_constant)` -- via
 * `reloco::fixed_point::exp()` -- on every call. This is the whole
 * point: a caller on a tickless kernel (dispatched from "the scheduler
 * happened to run", not a periodic timer interrupt) can sample whenever
 * it is convenient, at an irregular cadence, and the decay still comes
 * out correct for however much (or little) time actually passed --
 * exactly the same exponential-decay math a fixed cadence would have
 * used, generalized to an arbitrary elapsed interval.
 *
 * ## Why this lives on top of `reloco::fixed_point`
 *
 * `load_n = load_0 * decay + active*(1 - decay)` is the classic
 * exponential-moving-average update (see Unix `calc_load()`'s own
 * commentary for the derivation); `decay = exp(-elapsed/time_constant)`
 * needs a floating-point-free `exp()`, which is exactly
 * `fixed_point.hpp`'s own motivation (see that header's docs) --
 * `load_average` is, in fact, the originally-motivating use case
 * `fixed_point.hpp`/`fixed_int.hpp` were built for.
 *
 * Every operator `load_average` performs through `fixed_point` inherits
 * that header's own silent-wraparound-on-overflow caveats; `decay` is
 * additionally clamped to `0` once `elapsed` so far exceeds
 * `time_constant` that `exp()`'s own squaring-driven intermediate could
 * otherwise silently overflow before the reciprocal brought it back
 * down to (the correctly negligible) zero anyway.
 *
 * ## Using this on SMP: one systemwide instance, one serialized sampler
 *
 * `load_average`/`unix_load_average` are plain value types with no
 * internal synchronization at all (matching every other stateful,
 * caller-owned type in this library, e.g. `sched.hpp`'s policies) --
 * `sample()` is not safe to call concurrently from more than one CPU at
 * once. This is deliberate, not an oversight: exactly like Linux's own
 * global `avenrun`/`calc_global_load()`, a load average is a *systemwide*
 * quantity (`/proc/loadavg` reports one number, not one per CPU), so it
 * should be driven by exactly one logical "sampler", never by every CPU
 * independently:
 *
 * - Keep exactly **one** `unix_load_average` (or `load_average`)
 *   instance for the whole system (e.g. a file-scope/singleton
 *   variable, or a member of whatever boot-time-initialized global
 *   scheduler state already exists) -- never one per CPU; a per-CPU
 *   instance would each decay/track only that CPU's own local count,
 *   which is a different (also sometimes useful, but not
 *   `/proc/loadavg`-equivalent) quantity.
 * - Feed `sample()` the **sum of every CPU's runnable count**, gathered
 *   across all online CPUs at the sampling instant (e.g. each CPU's own
 *   `nr_running`-equivalent reachable through
 *   `structo::arch::per_cpu_ptr<Tag, T>` -- see `sched.hpp`'s own docs
 *   for that pattern), not any single CPU's own count.
 * - Restrict the actual `sample()` call itself to a single context at a
 *   time -- the simplest options already in this library are picking
 *   one fixed CPU to own it (`structo::arch::cpu_index::is_bsp()`-style
 *   "only the boot CPU calls this"), or guarding the call with a
 *   `structo::sync::kernel_spin_lock`/`reloco::spin_lock` if more than
 *   one context could plausibly race to sample at once (e.g. a periodic
 *   callout racing a syscall-driven forced recompute). Either way, the
 *   cross-CPU *gather* step (summing every CPU's count) only needs to
 *   be consistent enough for a human-facing approximate metric --
 *   exactly like Linux's own `calc_global_load()`, which deliberately
 *   does not stop every CPU to get a perfectly atomic snapshot either.
 *
 * @code
 * // One systemwide instance (e.g. file-scope, or a field of a global
 * // scheduler-state singleton) -- never one of these per CPU.
 * structo::unix_load_average<> g_load_average;
 *
 * // Called periodically from exactly one context (e.g. only ever from
 * // the boot CPU's own periodic callout/housekeeping path).
 * void update_systemwide_load_average(reloco::duration elapsed) {
 *   std::uint32_t total_runnable = 0;
 *   for (auto cpu : online_cpus()) // however this kernel enumerates online CPUs
 *     total_runnable += my_percpu_runqueue[cpu].nr_running(); // e.g. via per_cpu_ptr<Tag, runqueue>
 *   g_load_average.sample(elapsed, total_runnable);
 * }
 * @endcode
 */

#include <reloco/duration.hpp>
#include <reloco/fixed_int.hpp>
#include <reloco/fixed_point.hpp>

#include <cstdint>

namespace structo {

/**
 * @brief A single exponentially-decayed moving average of an "active
 * count", sampled by the caller at arbitrary (tickless) intervals. See
 * the @file-level docs above for the full rationale; `unix_load_average`
 * below is the classic Unix 1/5/15-minute triple built on three of
 * these.
 */
template <typename Rep = std::uint32_t, unsigned FracBits = 11> class load_average {
  // decay_for()'s 128-bit widened ratio intermediate (elapsed-in-milliseconds * one_raw) needs `Rep` no
  // wider than 64 bits to stay correct; load_average's own precision need is far smaller than
  // fixed_point/fixed_int's own general-purpose one, so this is not a meaningful restriction in practice --
  // construct a plain fixed_point<Rep, FracBits> directly (without load_average) if a wider Rep is ever needed.
  static_assert(sizeof(Rep) <= sizeof(std::uint64_t), "load_average<Rep, FracBits> requires Rep no wider than 64 bits");

public:
  using fixed = reloco::fixed_point<Rep, FracBits>;

  constexpr load_average() noexcept = default;

  /** @brief @p time_constant is the averaging window's time constant
   * `tau` (e.g. `60`/`300`/`900` seconds for the classic Unix 1/5/15-
   * minute windows) -- *not* the half-life; `decay` after exactly one
   * `tau` elapses is always `1/e`, matching `exp()`'s own definition.
   * Treated as `1` millisecond if given a zero duration, instead of
   * dividing by zero. */
  explicit constexpr load_average(reloco::duration time_constant) noexcept
      : time_constant_ms_(time_constant.as_millis() == 0 ? 1U : time_constant.as_millis()) {}

  /** @brief Folds @p active_count (sampled "right now") into the moving
   * average, decayed by however much @p elapsed time has passed since
   * the previous `sample` call (or since construction, for the first
   * call). */
  template <typename Count> constexpr void sample(reloco::duration elapsed, Count active_count) noexcept {
    fixed decay = decay_for(elapsed);
    fixed one = fixed::from_int(1);
    value_ = value_ * decay + fixed::from_int(active_count) * (one - decay);
  }

  /** @brief The current average, as a `fixed_point` -- `to_int()`/
   * `fractional_percent()` render it the way `/proc/loadavg` does. */
  [[nodiscard]] constexpr fixed value() const noexcept { return value_; }

private:
  // `exp(-ratio) == 1/exp(ratio)`: computing the reciprocal of exp() of
  // the (always non-negative) elapsed/time_constant ratio, rather than
  // negating the ratio first, means `Rep` never has to be a signed
  // type (unlike negating would require, since `exp()` only recognizes
  // "negative" for a signed `Rep`) -- a plain `std::uint32_t` `Rep`
  // works exactly as well as a signed one here.
  [[nodiscard]] constexpr fixed decay_for(reloco::duration elapsed) const noexcept {
    using wide_ratio = reloco::fixed_uint<128>;
    wide_ratio numerator = wide_ratio(elapsed.as_millis()) * wide_ratio(fixed::one_raw);
    wide_ratio ratio_raw = numerator / wide_ratio(time_constant_ms_);
    // Once elapsed/time_constant is large enough that exp(-ratio) is already far below what FracBits
    // fractional bits can resolve, the decayed-away contribution is indistinguishable from zero -- clamp
    // here instead of letting a huge ratio drive exp()'s own repeated-squaring intermediate to silently
    // overflow before the reciprocal below would have brought it back down to (the correctly negligible) zero.
    constexpr wide_ratio max_ratio = wide_ratio(fixed::one_raw) * wide_ratio(32U);
    if (ratio_raw >= max_ratio)
      return fixed();
    fixed ratio = fixed::from_raw(static_cast<Rep>(ratio_raw));
    return fixed::from_int(1) / ratio.exp();
  }

  std::uint64_t time_constant_ms_ = 1;
  fixed value_{};
};

/**
 * @brief The classic Unix `/proc/loadavg` triple: three `load_average`s
 * sharing one `sample(elapsed, active_count)` call, at the traditional
 * 1/5/15-minute time constants.
 */
template <typename Rep = std::uint32_t, unsigned FracBits = 11> class unix_load_average {
public:
  using fixed = reloco::fixed_point<Rep, FracBits>;

  constexpr unix_load_average() noexcept = default;

  /** @brief Folds @p active_count into all three averages at once, each decayed by @p elapsed. */
  template <typename Count> constexpr void sample(reloco::duration elapsed, Count active_count) noexcept {
    one_minute_.sample(elapsed, active_count);
    five_minute_.sample(elapsed, active_count);
    fifteen_minute_.sample(elapsed, active_count);
  }

  [[nodiscard]] constexpr fixed one_minute() const noexcept { return one_minute_.value(); }
  [[nodiscard]] constexpr fixed five_minute() const noexcept { return five_minute_.value(); }
  [[nodiscard]] constexpr fixed fifteen_minute() const noexcept { return fifteen_minute_.value(); }

private:
  load_average<Rep, FracBits> one_minute_{reloco::duration::from_secs(60)};
  load_average<Rep, FracBits> five_minute_{reloco::duration::from_secs(300)};
  load_average<Rep, FracBits> fifteen_minute_{reloco::duration::from_secs(900)};
};

} // namespace structo
