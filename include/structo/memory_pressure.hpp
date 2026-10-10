// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file memory_pressure.hpp
 * @brief Memory-pressure measurement and page-daemon pacing, all integer, allocation-free and constexpr.
 *
 * Pressure is expressed as an *urgency* in `[0, 256]`: 0 = plenty of memory, 256 = out of memory. Inputs:
 * - free pages versus watermarks (`memory_watermarks`, `pressure_urgency`),
 * - how fruitless the last reclaim round was (`reclaim_inefficiency`, `combine_pressure`),
 * - a fast-attack / slow-decay average (`pressure_average`) so one lucky round does not end the alarm.
 *
 * `page_daemon_backoff` turns the urgency into the sleep time of the page daemon: short when pressure is high
 * and reclaim works, longer when idle, and exponentially longer while rounds scan pages but reclaim nothing
 * (spinning on an unreclaimable system only burns CPU). Time is a `reloco::duration` (wall-clock), never a tick count.
 *
 * @code
 * // Watermarks for a zone of 1M pages: min 0.5 %, low = min*5/4, high = min*3/2 (floor of 64 pages).
 * auto wm = structo::memory_watermarks::for_total(1'000'000, 5, 64);
 *
 * structo::page_daemon_backoff_config cfg{};   // min 1 ms, max 1 s, futile rounds back off up to 8x
 * structo::page_daemon_backoff backoff{cfg};
 * structo::pressure_average avg;               // smoothed urgency
 *
 * for (;;) {
 *   // Effective free memory: free pages plus half of the clean reclaimable ones (cheap to drop).
 *   auto eff    = structo::effective_free_pages(free_pages(), clean_inactive_pages());
 *   auto urgent = structo::pressure_urgency(eff, wm);              // 0..256 from the watermarks
 *   avg.update(urgent);                                            // fast attack, slow decay
 *
 *   auto [scanned, reclaimed] = run_one_round(avg.value());        // e.g. decay_scan_count() sized
 *
 *   // Blend in reclaim efficiency: scanning a lot and freeing little means the real pressure is higher.
 *   auto pressure = structo::combine_pressure(avg.value(),
 *                                             structo::reclaim_inefficiency(scanned, reclaimed));
 *   timed_wait(daemon_wakeup, backoff.next_interval(pressure, scanned, reclaimed));
 *   if (structo::pressure_level_of(free_pages(), wm) == structo::pressure_level::critical) wake_now();
 * }
 * @endcode
 *
 * Integration:
 * - **Page daemon / decay:** `decay_urgency()` in page_decay.hpp is `pressure_urgency` over a (low, high) pair;
 *   feed `avg.value()` into the scan sizing and `backoff.next_interval()` into the daemon sleep
 *   ([page_decay.md](page_decay.md), [page_queue_scan.md](page_queue_scan.md)).
 * - **Allocators:** call `pressure_level_of()` on the allocation slow path; `critical` should wake the daemon
 *   immediately (and may block the allocator), `low` is the "start background reclaim" level.
 * - **Hotplug / balloon:** after pages are claimed (`buddy_allocator::claim_range`) or onlined, rebuild the
 *   watermarks with `for_total()` from the new total, or `operator+` the per-segment sets
 *   ([memory_hotplug.md](memory_hotplug.md)).
 */

#include <reloco/detail/assert.hpp>
#include <reloco/duration.hpp>

#include <cstdint>

namespace structo {

/** Free-page thresholds of a zone / pool. Invariant: `min <= low <= high`. */
struct memory_watermarks {
  std::uint64_t min{0};  //!< Below this the system is critically short; only emergency allocations succeed.
  std::uint64_t low{0};  //!< Below this background reclaim must run.
  std::uint64_t high{0}; //!< At or above this reclaim may stop.

  /**
   * Derives watermarks from the pool size. `min = max(total * min_permille / 1000, min_floor)`,
   * `low = min * 5 / 4`, `high = min * 3 / 2`.
   * @param total_pages  Managed pages in the zone.
   * @param min_permille Share of `total_pages` kept as the minimum, in 1/1000.
   * @param min_floor    Lower bound for `min`, so tiny pools still get a margin.
   */
  [[nodiscard]] static constexpr memory_watermarks for_total(std::uint64_t total_pages, std::uint32_t min_permille,
                                                             std::uint64_t min_floor = 0) noexcept {
    std::uint64_t m = total_pages / 1000 * min_permille + (total_pages % 1000) * min_permille / 1000;
    if (m < min_floor) {
      m = min_floor;
    }
    return {m, m + m / 4, m + m / 2};
  }

  /** Sum of two sets, for combining per-segment / per-zone watermarks into a pool-wide one. */
  [[nodiscard]] friend constexpr memory_watermarks operator+(const memory_watermarks &a,
                                                             const memory_watermarks &b) noexcept {
    return {a.min + b.min, a.low + b.low, a.high + b.high};
  }

  [[nodiscard]] constexpr bool valid() const noexcept { return min <= low && low <= high; }
};

/** Coarse pressure class for allocation slow paths. */
enum class pressure_level : std::uint8_t {
  none,     //!< free >= high
  low,      //!< low <= free < high: background reclaim should run
  medium,   //!< min <= free < low: reclaim is urgent, daemon should be awake
  critical, //!< free < min: wake the daemon now, restrict allocations
};

[[nodiscard]] constexpr pressure_level pressure_level_of(std::uint64_t free_pages,
                                                         const memory_watermarks &wm) noexcept {
  if (free_pages >= wm.high) {
    return pressure_level::none;
  }
  if (free_pages >= wm.low) {
    return pressure_level::low;
  }
  if (free_pages >= wm.min) {
    return pressure_level::medium;
  }
  return pressure_level::critical;
}

/** Maximum urgency value. */
inline constexpr std::uint32_t pressure_max = 256;

/** Urgency 0..256: 0 at/above `high`, 256 at/below `min`, linear in between. */
[[nodiscard]] constexpr std::uint32_t pressure_urgency(std::uint64_t free_pages,
                                                       const memory_watermarks &wm) noexcept {
  if (free_pages >= wm.high) {
    return 0;
  }
  if (free_pages <= wm.min || wm.high <= wm.min) {
    return pressure_max;
  }
  std::uint64_t span = wm.high - wm.min;
  std::uint64_t below = wm.high - free_pages;
  while (span > (UINT64_MAX >> 8)) { // keep below * 256 from overflowing
    span >>= 1;
    below >>= 1;
  }
  if (span == 0) {
    return pressure_max;
  }
  return static_cast<std::uint32_t>(below * 256 / span);
}

/** Free pages plus half of the clean reclaimable pages (they can be dropped without I/O). */
[[nodiscard]] constexpr std::uint64_t effective_free_pages(std::uint64_t free_pages,
                                                           std::uint64_t clean_reclaimable) noexcept {
  return free_pages + clean_reclaimable / 2;
}

/**
 * How fruitless a reclaim round was, 0..256: 0 when everything scanned was reclaimed, 256 when nothing was.
 * Returns 0 when nothing was scanned (no information).
 */
[[nodiscard]] constexpr std::uint32_t reclaim_inefficiency(std::uint64_t scanned, std::uint64_t reclaimed) noexcept {
  if (scanned == 0) {
    return 0;
  }
  if (reclaimed > scanned) {
    reclaimed = scanned;
  }
  std::uint64_t miss = scanned - reclaimed;
  while (scanned > (UINT64_MAX >> 8)) { // keep miss * 256 from overflowing
    scanned >>= 1;
    miss >>= 1;
  }
  if (scanned == 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(miss * 256 / scanned);
}

/**
 * Blends watermark pressure with reclaim inefficiency: no watermark pressure stays 0 (nothing to reclaim for),
 * otherwise the result moves from `wm_urgency` toward 256 as reclaim gets less effective.
 */
[[nodiscard]] constexpr std::uint32_t combine_pressure(std::uint32_t wm_urgency, std::uint32_t inefficiency) noexcept {
  if (wm_urgency == 0) {
    return 0;
  }
  if (wm_urgency > pressure_max) {
    wm_urgency = pressure_max;
  }
  if (inefficiency > pressure_max) {
    inefficiency = pressure_max;
  }
  return wm_urgency + (pressure_max - wm_urgency) * inefficiency / pressure_max;
}

/** Fast-attack, slow-decay average of urgency samples (rises at once, falls by 1/2^decay_shift per update). */
class pressure_average {
public:
  explicit constexpr pressure_average(std::uint32_t decay_shift = 2) noexcept : shift_(decay_shift) {}

  /** Feeds a 0..256 sample and returns the new average. */
  constexpr std::uint32_t update(std::uint32_t sample) noexcept {
    if (sample > pressure_max) {
      sample = pressure_max;
    }
    const std::uint32_t target = sample << 8; // 8 fractional bits keep slow decay from sticking above 0
    if (target >= value_) {
      value_ = target;
    } else {
      const std::uint32_t step = (value_ - target) >> shift_;
      value_ -= step ? step : (value_ - target);
    }
    return value();
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_ >> 8; }
  constexpr void reset() noexcept { value_ = 0; }

private:
  std::uint32_t value_{0};
  std::uint32_t shift_;
};

/** Tuning for `page_daemon_backoff`. Wall-clock based: the kernel's tick rate never enters the policy. */
struct page_daemon_backoff_config {
  reloco::duration min_interval{reloco::duration::from_millis(1)};    //!< Sleep at urgency 256.
  reloco::duration max_interval{reloco::duration::from_millis(1000)}; //!< Sleep at urgency 0.
  std::uint32_t max_futile_shift{3}; //!< Futile rounds double the sleep up to `1 << max_futile_shift` times.
};

/** Sleep time for a given urgency, linear from `max_interval` (0) to `min_interval` (256). */
[[nodiscard]] constexpr reloco::duration page_daemon_interval(const page_daemon_backoff_config &cfg,
                                                              std::uint32_t urgency) noexcept {
  if (urgency >= pressure_max) {
    return cfg.min_interval;
  }
  const std::uint64_t lo = cfg.min_interval.as_nanos();
  const std::uint64_t range = cfg.max_interval.as_nanos() - lo;
  const std::uint64_t cut = (range >> 8) * urgency + (((range & 0xFF) * urgency) >> 8);
  return reloco::duration::from_nanos(lo + range - cut);
}

/**
 * Stateful pacing for the page daemon. Rounds that scanned pages but reclaimed none are *futile*; each
 * consecutive one doubles the sleep (up to `max_futile_shift` doublings, never above `max_interval`) so an
 * unreclaimable system does not make the daemon spin. Any progress, or an idle round, clears the streak.
 */
class page_daemon_backoff {
public:
  explicit constexpr page_daemon_backoff(const page_daemon_backoff_config &cfg = {}) noexcept : cfg_(cfg) {
    RELOCO_ASSERT(cfg.min_interval <= cfg.max_interval, "page_daemon_backoff: min_interval > max_interval");
  }

  /**
   * Reports a finished round and returns how long to sleep before the next one. Hand the result to the
   * kernel's timed wait (condvar / callout / timer wheel), not to a tick counter.
   */
  [[nodiscard]] constexpr reloco::duration next_interval(std::uint32_t urgency, std::uint64_t scanned,
                                                         std::uint64_t reclaimed) noexcept {
    if (scanned != 0 && reclaimed == 0) {
      if (futile_ < cfg_.max_futile_shift) {
        ++futile_;
      }
    } else {
      futile_ = 0;
    }
    const std::uint64_t max_ns = cfg_.max_interval.as_nanos();
    std::uint64_t t = page_daemon_interval(cfg_, urgency).as_nanos();
    for (std::uint32_t i = 0; i < futile_; ++i) { // saturating doubling
      if (t >= max_ns - t) {
        return cfg_.max_interval;
      }
      t <<= 1;
    }
    return reloco::duration::from_nanos(t);
  }

  [[nodiscard]] constexpr std::uint32_t futile_rounds() const noexcept { return futile_; }
  constexpr void reset() noexcept { futile_ = 0; }

private:
  page_daemon_backoff_config cfg_;
  std::uint32_t futile_{0};
};

} // namespace structo
