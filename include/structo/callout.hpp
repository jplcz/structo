// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file callout.hpp
 * @brief `structo::callout<Subsystem, Capacity>`: a memory-safe,
 * FreeBSD-`callout(9)`-like one-shot/periodic deferred-callback object,
 * built directly on top of `structo::async_kernel_object` -- the
 * "`callout` on top of `async_kernel_object`" this library's own
 * `async_kernel_object.hpp` docs forward-reference.
 *
 * Exactly like `callout(9)` itself, this header says nothing about *how*
 * a duration becomes "the callback runs now": that is entirely the
 * concern of a `Subsystem` -- the **callout subsystem** customization
 * point -- which owns the actual scheduling policy (a software timer
 * wheel, a single hardware `timer_ref` multiplexed across every live
 * `callout`, a priority queue keyed by deadline, ...). `callout` itself
 * only ever does three things a subsystem cannot safely do on its own
 * behalf: own the callback, present the `callout(9)`-shaped safe API
 * (`reset`/`stop`/`drain`/`pending`/`active`/`deactivate`), and -- via
 * the `async_kernel_object` it wraps -- guarantee the same use-after-
 * free-proof semantics `async_kernel_object.hpp` documents in full.
 *
 * ## Relationship to `async_kernel_object`
 *
 * `callout<Subsystem, Capacity>` is a thin, composed wrapper around an
 * `async_kernel_object<Subsystem, Capacity>` data member -- not a
 * subclass (`Subsystem::submit`/`cancel` are always instantiated by
 * `async_kernel_object` with *its own* type as `Obj`, never a derived
 * type, so composition is the only correct shape here; inheriting from
 * `async_kernel_object` would silently leave `hook()`/`state()` bound to
 * the base sub-object regardless, but would mislead readers into
 * thinking a `callout` *is-an* `async_kernel_object` with its own
 * identity for `Traits` purposes, which it is not). `callout` narrows
 * `async_kernel_object::try_submit`'s free-form `Args...` down to
 * exactly one `reloco::duration` (the one thing every deadline-based
 * subsystem needs to know), and renames the generic operations to their
 * `callout(9)` counterparts:
 *
 * | `async_kernel_object`  | `callout`      | `callout(9)`          |
 * |------------------------|----------------|-----------------------|
 * | `try_submit(cb, dur)`  | `reset(dur,cb)`| `callout_reset`       |
 * | `cancel()`             | `stop()`       | `callout_stop`        |
 * | `drain()`              | `drain()`      | `callout_drain`       |
 * | `deactivate()`         | `deactivate()` | `callout_deactivate`  |
 * | `is_pending()`         | `pending()`    | `callout_pending`     |
 * | `is_active()`          | `active()`     | `callout_active`      |
 * | `is_firing()`          | `firing()`     | (no direct equivalent)|
 *
 * ## The callback: `void(callout &)`, not `void(async_kernel_object &)`
 *
 * The callback a caller gives to `reset`/`reset_periodic` is invoked
 * with a reference to the owning `callout` itself (`void(callout &)`),
 * not the internal `async_kernel_object` -- callers should never need to
 * see that implementation detail. `callout` achieves this by owning a
 * *second*, independently-sized `inplace_function<void(callout &),
 * Capacity>` for the caller's callback, and handing the wrapped
 * `async_kernel_object` only a tiny, fixed-size glue closure that
 * captures nothing but `this` and forwards the call -- so `Capacity`
 * sizes exactly what the caller's own callback needs to capture, with
 * no hidden overhead from the wrapping.
 *
 * ## Periodic reset: `reset_periodic`
 *
 * `callout(9)` itself has no native "periodic" mode -- the idiomatic
 * BSD pattern is for the callback to call `callout_reset` on itself
 * again as its very last action. `reset_periodic` is exactly that
 * pattern, pre-packaged: it re-`reset()`s with the same period and a
 * fresh copy of the same callback immediately after invoking it, unless
 * the callback itself called `stop()`/`deactivate()` (checked via
 * `active()` right before the re-`reset()`). Exactly like manual self-
 * rearming from inside `async_kernel_object`'s own firing callback (see
 * its docs), this is safe -- only one firing can ever be in flight for
 * a given `callout` at a time -- but the callback must not read any of
 * its own captured state *after* the point `reset_periodic` re-arms it,
 * since the storage holding the currently-running callback instance is
 * reclaimed and reused for the next one as part of that re-arm.
 *
 * ## Customizing: the callout subsystem (`Subsystem`)
 *
 * `Subsystem` is the exact same compile-time `Traits` policy
 * `async_kernel_object` itself defines (see `async_kernel_object.hpp`),
 * specialized to a single `reloco::duration` argument:
 *
 * @code
 * struct my_callout_subsystem {
 *   template <typename Obj>
 *   static reloco::result<void> submit(Obj &self, reloco::duration period) noexcept;
 *   template <typename Obj>
 *   static reloco::result<void> cancel(Obj &self) noexcept;
 *   // optional: using hook_type = ...;    (e.g. a wheel bucket's intrusive link)
 *   // optional: using state_type = ...;   (e.g. an absolute deadline/generation count)
 * };
 * @endcode
 *
 * `Obj` here is `async_kernel_object<my_callout_subsystem, Capacity>`
 * (the member `callout` wraps), *not* `callout` itself -- `submit`
 * enqueues `&self` into whatever concrete wheel/queue structure the
 * subsystem maintains (using `self.hook()`/`self.state()`, reachable
 * only to `my_callout_subsystem`'s own member templates via
 * `friend Traits`), to later call `self.fire()` back once `period` has
 * elapsed, from whatever context that subsystem fires from -- a
 * hardware timer interrupt, a software wheel-sweep routine driven by a
 * periodic tick, etc. A single `my_callout_subsystem` works uniformly
 * for every live `callout<my_callout_subsystem, N>` regardless of `N`.
 */

#include "async_kernel_object.hpp"

#include <reloco/duration.hpp>
#include <reloco/inplace_function.hpp>

#include <cstddef>
#include <type_traits>
#include <utility>

namespace structo {

using namespace reloco;

/**
 * @brief Memory-safe, owning, FreeBSD-`callout(9)`-like deferred
 * callback object. See the @file-level docs above for the full design.
 *
 * Never copyable or movable, exactly like the `async_kernel_object` it
 * wraps: `Subsystem::submit`/`cancel` operate on that member's own
 * address, so this object's identity must stay fixed for as long as it
 * may be `pending()`/`firing()`.
 */
template <typename Subsystem, std::size_t Capacity = 32> class RELOCO_OWNER callout {
public:
  using async_object = async_kernel_object<Subsystem, Capacity>;
  using callback_type = inplace_function<void(callout &), Capacity>;

  constexpr callout() noexcept = default;

  callout(const callout &) = delete;
  callout &operator=(const callout &) = delete;
  callout(callout &&) = delete;
  callout &operator=(callout &&) = delete;

  /**
   * @brief `callout_reset`: (re-)arms this callout to invoke @p callback
   * once @p period elapses, replacing whatever was previously armed (if
   * anything) exactly like `async_kernel_object::try_submit` does.
   *
   * Not safe to call concurrently with another thread's `reset`/`stop`
   * on the same `callout` without external serialization; always safe
   * to call as the last thing a firing callback does, to re-arm itself
   * (see `reset_periodic` for that pattern pre-packaged).
   */
  template <typename F> [[nodiscard]] result<void> reset(duration period, F &&callback) noexcept {
    static_assert(std::is_invocable_v<std::decay_t<F> &, callout &>,
                  "callout: callback must be invocable as void(callout &)");
    callback_ = std::forward<F>(callback);
    return obj_.try_submit([this](async_object &) noexcept { invoke_callback(); }, period);
  }

  /**
   * @brief `reset` pre-packaged to automatically re-arm itself with the
   * same @p period and a fresh copy of @p callback after every
   * invocation, unless the callback itself `stop()`/`deactivate()`d this
   * `callout` -- the idiomatic `callout(9)` periodic-timer pattern. See
   * the @file-level docs above for the exact safety caveat this implies
   * for @p callback's own captured state.
   */
  template <typename F> [[nodiscard]] result<void> reset_periodic(duration period, F callback) noexcept {
    static_assert(std::is_invocable_v<std::decay_t<F> &, callout &>,
                  "callout: callback must be invocable as void(callout &)");
    return reset(period, periodic_wrapper<std::decay_t<F>>{std::move(callback), period});
  }

  /**
   * @brief `callout_stop`: non-blocking; returns whether this `callout`
   * was actually still pending (a scheduled fire was genuinely
   * prevented) -- `false` is the ordinary, non-error outcome when it
   * had already fired or was never armed.
   */
  [[nodiscard]] result<bool> stop() noexcept { return obj_.cancel(); }

  /**
   * @brief `callout_drain`: blocking; `stop()` followed by waiting for
   * any concurrently in-flight invocation to finish -- the one
   * operation that actually guarantees it is safe to free this
   * `callout`'s memory afterwards. Must not be called from inside this
   * `callout`'s own firing callback.
   */
  [[nodiscard]] result<void> drain(std::uint32_t max_spins = async_object::default_drain_max_spins) noexcept {
    return obj_.drain(max_spins);
  }

  /**
   * @brief `callout_deactivate`: lets an already-scheduled fire still
   * happen (and still clear `pending()`), but silently skips invoking
   * the callback -- "let it expire silently" rather than removing it
   * from the subsystem's own schedule.
   */
  void deactivate() noexcept { obj_.deactivate(); }

  /** @brief `callout_pending`: whether this `callout` is currently armed and not yet fired. */
  [[nodiscard]] bool pending() const noexcept { return obj_.is_pending(); }

  /** @brief `callout_active`: whether this `callout` is armed and not yet `deactivate`d/`stop`ped. */
  [[nodiscard]] bool active() const noexcept { return obj_.is_active(); }

  /** @brief Whether this `callout`'s callback is currently executing (on some context). */
  [[nodiscard]] bool firing() const noexcept { return obj_.is_firing(); }

private:
  template <typename F> struct periodic_wrapper {
    F callback;
    duration period;

    void operator()(callout &self) {
      callback(self);
      if (self.active())
        (void)self.reset_periodic(period, callback);
    }
  };

  void invoke_callback() noexcept {
    if (callback_.has_value())
      callback_(*this);
  }

  async_object obj_;
  callback_type callback_;
};

} // namespace structo
