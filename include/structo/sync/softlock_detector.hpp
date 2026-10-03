// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file softlock_detector.hpp
 * @brief `structo::sync::softlock_detector`: a tiny, stack-only tick
 * counter for spin-wait loops that traps (via `RELOCO_ASSERT`) once it
 * has been ticked more than some configurable limit of times without a
 * matching `reset()` -- turning a silent, infinite soft lockup (a spin
 * loop whose condition will in fact never become true, e.g. a deadlock
 * or a lost wakeup) into an immediate, loud failure instead of a hung
 * core that never reports anything.
 *
 * ## Why this exists
 *
 * A spin loop is, by construction, indistinguishable from a true
 * deadlock while it is running: both look like "still spinning". On
 * real hardware that just means a wedged CPU with no diagnostic; in a
 * test, it means a hung test process. `softlock_detector` gives every
 * spin-wait loop a cheap, opt-in way to bound how long it is willing to
 * believe the wait is still legitimate before giving up and trapping,
 * the same role a kernel's own softlockup/hung-task watchdog plays, but
 * local to one spin loop instead of a whole-system timer-driven one.
 *
 * ## Usage
 *
 * @code
 * structo::sync::softlock_detector detector;
 * structo::sync::backoff bo;
 * while (!condition_met()) {
 *   detector.tick(); // traps once `limit()` iterations are recorded
 *   bo.spin();
 * }
 * @endcode
 *
 * `kernel_spin_lock<Traits>` (see `kernel_spin_lock.hpp`) wires one of
 * these into its own contended `lock()` spin loop automatically, ticking
 * it once per failed acquisition attempt; supply an optional
 * `Traits::softlock_limit` to override the default limit for a
 * particular lock policy.
 *
 * ## The process-wide default limit
 *
 * A fixed, hardcoded tick limit cannot be "a reasonable number of
 * iterations" across every target this library runs on: the same spin
 * loop burns through its iteration budget orders of magnitude faster on
 * a multi-GHz server core than on a slow embedded one. Rather than
 * guess, `default_limit()`/`set_default_limit()` expose a single
 * process/kernel-wide default (`std::uint64_t`, initially a generous,
 * architecture-agnostic placeholder) a kernel port is expected to
 * override once, early at boot, with a value it derives from its own
 * measured CPU cycle rate (e.g. `structo::hw::cycles`/
 * `clock_cycles.hpp`'s calibrated frequency times however many seconds
 * of spinning should count as "stuck") -- the same way a real kernel
 * tunes its own hung-task/softlockup watchdog threshold to the hardware
 * it is actually running on rather than shipping one constant for every
 * target.
 *
 * Construct one `softlock_detector` per spin-wait loop (it is
 * non-atomic, non-shared, exactly one spinning thread/core ever touches
 * one instance) and call `reset()` whenever the loop has otherwise made
 * forward progress (e.g. observed a changed generation counter) and the
 * remaining wait should no longer count against the original limit.
 */

#include <reloco/detail/assert.hpp>

#include <atomic>
#include <cstdint>

namespace structo::sync {

/**
 * @brief Tick counter that traps once ticked more than `limit()` times
 * since construction or the last `reset()`; see the @file-level docs
 * above. Not thread-safe to share -- construct one per spinning
 * thread/core.
 */
class softlock_detector {
public:
  using counter_type = std::uint64_t;

  /**
   * @brief The current process/kernel-wide default tick limit, used by
   * the constructor when no explicit `limit` is supplied. See the
   * @file-level docs' "process-wide default limit" section --
   * override via `set_default_limit()` once at boot with a value
   * derived from the platform's actual CPU cycle rate.
   */
  [[nodiscard]] static counter_type default_limit() noexcept {
    return default_limit_.load(std::memory_order_relaxed);
  }

  /**
   * @brief Overrides the process/kernel-wide default tick limit
   * returned by `default_limit()`. Intended to be called once, early at
   * boot (before any contended spin loop can observe the old value),
   * with a value derived from the platform's measured CPU cycle rate.
   */
  static void set_default_limit(counter_type limit) noexcept {
    default_limit_.store(limit, std::memory_order_relaxed);
  }

  /** @param limit Number of `tick()` calls this instance tolerates before trapping. */
  explicit softlock_detector(counter_type limit = default_limit()) noexcept : limit_(limit) {}

  /**
   * @brief Records one spin-wait iteration. Traps (via `RELOCO_ASSERT`)
   * once this has now been called more than `limit()` times since
   * construction or the last `reset()`.
   */
  void tick() noexcept {
    ++count_;
    RELOCO_ASSERT(count_ <= limit_, "softlock_detector: spin loop exceeded its iteration limit (possible soft lockup)");
  }

  /** @brief Clears the tick count, e.g. once the loop observes forward progress. */
  void reset() noexcept { count_ = 0; }

  /** @brief Number of `tick()` calls recorded since construction or the last `reset()`. */
  [[nodiscard]] constexpr counter_type count() const noexcept { return count_; }

  /** @brief The configured tick limit this instance traps beyond. */
  [[nodiscard]] constexpr counter_type limit() const noexcept { return limit_; }

private:
  // Generous, architecture-agnostic placeholder until a kernel port
  // calls `set_default_limit()` with a value derived from its own
  // measured CPU cycle rate; see the @file-level docs.
  static inline std::atomic<counter_type> default_limit_{100'000'000};

  counter_type limit_;
  counter_type count_{0};
};

} // namespace structo::sync
