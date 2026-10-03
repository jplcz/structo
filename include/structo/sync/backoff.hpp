// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file backoff.hpp
 * @brief `structo::sync::backoff`: a tiny, stack-only exponential-backoff
 * state machine for spin-wait loops polling some externally-owned state
 * (an atomic flag, a generation counter, a ring-buffer slot, ...) --
 * pulling out, as a reusable primitive, the same "capped, doubling burst
 * of `reloco::hint::spin_loop()` between each poll" pattern
 * `arch/ipi_dispatcher.hpp`'s `ipi_message::wait()` uses, so every other
 * spin-wait loop in a kernel port doesn't need to hand-roll it again.
 *
 * ## Why back off at all
 *
 * A tight spin loop that re-polls the same atomic every single iteration
 * (`while (!flag.load()) {}`) keeps re-issuing loads against the one
 * cache line another core needs *exclusive* ownership of to write its
 * own update -- the spinning core's own read traffic can measurably slow
 * down the very write it is waiting for, especially on many-core/SMT
 * hardware. Backing off -- waiting progressively longer between polls,
 * via a growing burst of `reloco::hint::spin_loop()` calls rather than a
 * fixed sleep -- cuts that read traffic down once it becomes clear the
 * wait isn't going to resolve in the first instant, while still reacting
 * quickly (one spin-wait hint, no delay at all) in the overwhelmingly
 * common case where the wait resolves almost immediately.
 *
 * ## Sensible defaults, no configuration required
 *
 * `backoff{}` doubles its spin-wait burst from 1 up to a cap of 1024
 * `reloco::hint::spin_loop()` calls per `spin()` -- the same constants
 * `ipi_message::wait()` used before this was extracted, chosen simply as
 * "enough doublings (eleven) to meaningfully space out a long wait
 * without ever leaving a single `spin()` call so long it noticeably
 * delays reacting to the state finally changing". Pass different
 * bounds to the constructor only if a specific call site has measured a
 * reason to.
 *
 * ## Usage
 *
 * @code
 * structo::sync::backoff bo;
 * while (!condition_met()) {
 *   bo.spin();
 * }
 *
 * // Or, equivalently, the single-predicate convenience form:
 * structo::sync::backoff::spin_until([] { return condition_met(); });
 * @endcode
 *
 * Construct one `backoff` per spin-wait loop (it is non-atomic,
 * non-shared, exactly one spinning thread/core ever touches one
 * instance) and call `reset()` only if the very same instance is reused
 * across multiple, logically separate waits.
 */

#include <reloco/hint.hpp>

#include <cstdint>
#include <type_traits>

namespace structo::sync {

/**
 * @brief Exponential backoff for a spin-wait loop; see the @file-level
 * docs above. Not thread-safe to share -- construct one per spinning
 * thread/core.
 */
class backoff {
public:
  /**
   * @param initial_spins Number of `reloco::hint::spin_loop()` calls the
   * first `spin()` performs.
   * @param max_spins Cap the per-`spin()` burst never grows past, once
   * doubling would otherwise exceed it.
   */
  explicit constexpr backoff(std::uint32_t initial_spins = 1, std::uint32_t max_spins = 1024) noexcept
      : initial_spins_(initial_spins), max_spins_(max_spins), spins_(initial_spins) {}

  /** @brief Performs one backoff step: issues this call's spin-wait
   * burst (starting at `initial_spins`, doubling on every call, capped
   * at `max_spins`), then grows the burst for next time. */
  void spin() noexcept {
    for (std::uint32_t i = 0; i < spins_; ++i) {
      reloco::hint::spin_loop();
    }
    if (spins_ < max_spins_) {
      spins_ = (spins_ > max_spins_ / 2) ? max_spins_ : spins_ * 2;
    }
  }

  /** @brief Resets the burst back to `initial_spins`, so this instance
   * can be reused for another, logically separate wait. */
  void reset() noexcept { spins_ = initial_spins_; }

  /** @brief The spin-wait burst the *next* `spin()` call will perform. */
  [[nodiscard]] constexpr std::uint32_t pending_spins() const noexcept { return spins_; }

  /**
   * @brief Convenience: spins (with backoff) until @p pred returns
   * `true`, then returns. Equivalent to constructing a `backoff` and
   * calling `spin()` in a loop around @p pred by hand.
   * @tparam Pred Invocable as `bool Pred()` (or `Pred() noexcept`).
   */
  template <typename Pred> static void spin_until(Pred &&pred) noexcept(std::is_nothrow_invocable_v<Pred &>) {
    backoff bo;
    while (!pred()) {
      bo.spin();
    }
  }

private:
  std::uint32_t initial_spins_;
  std::uint32_t max_spins_;
  std::uint32_t spins_;
};

} // namespace structo::sync
