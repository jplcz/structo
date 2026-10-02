// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file async_kernel_object.hpp
 * @brief `structo::async_kernel_object<Traits, Capacity>`: a memory-safe,
 * owning building block for *any* kernel object whose completion is
 * signaled asynchronously -- from an interrupt handler, another CPU, a
 * deferred-work/softirq runner, a timer wheel's expiry sweep, or any
 * other execution context outside the arming caller's control -- rather
 * than being awaited synchronously in the same call stack that armed it.
 *
 * This header is deliberately subsystem-agnostic: it says nothing about
 * *when* `fire()` is called or *why* (a timer becoming due, a work item
 * being dequeued, a DPC being run, ...), only the one problem every such
 * subsystem shares and must get right: safely owning a callback that may
 * be invoked from a racing, uncontrolled context, while still letting
 * the arming caller cancel it or safely reclaim its memory. A concrete
 * subsystem (a future `callout.hpp` timer-wheel object, a future
 * workqueue/tasklet-style deferred-work object, ...) is expected to wrap
 * this type, adding only what it actually needs on top: a `duration`
 * parameter to `try_submit` for a timer, a priority level for a work
 * queue, an intrusive wheel-bucket/queue hook via `Traits::hook_type`,
 * whatever per-object bookkeeping it needs via `Traits::state_type`.
 *
 * ## The hazard this exists to prevent
 *
 * A callback registered with *some* subsystem to be invoked later, from
 * a context the registering caller does not control, is one of the most
 * reliable sources of use-after-free bugs in kernel code: the object
 * holding the callback gets freed while a fire is still in flight on
 * another core, or a caller re-arms/destroys it believing a prior fire
 * has already finished when it has not. FreeBSD's `callout(9)` --
 * `callout_stop()` (non-blocking, "best-effort" cancel) vs.
 * `callout_drain()` (blocking: wait for any in-flight invocation to
 * actually finish before returning, specifically so it is safe to free
 * the memory the callout lives in right after) -- is the canonical
 * precedent this header's `cancel()`/`drain()` split mirrors exactly.
 *
 * ## State machine
 *
 * Every `async_kernel_object` tracks exactly three independent, atomic
 * bits (`std::atomic<std::uint32_t>`, so a concurrent `fire()` on
 * another core and a `cancel()`/`drain()`/query on the arming caller's
 * core never race on plain, unsynchronized reads/writes of ordinary
 * member fields):
 *
 * - **active**: this object has been armed (`try_submit`) and not yet
 *   `deactivate`d/`cancel`ed. A `fire()` that observes this bit cleared
 *   (racing a concurrent `cancel()`/`deactivate()`) skips invoking the
 *   callback entirely, even if it still observed **pending** set.
 * - **pending**: currently submitted to -- and not yet picked up by --
 *   whatever concrete subsystem `Traits::submit` enqueued it with.
 *   Cleared the instant `fire()` picks it up (atomically, together with
 *   setting **firing**, so a racing `cancel()` can never observe both
 *   cleared-pending-not-yet-firing and correctly reports whether it
 *   actually prevented a fire).
 * - **firing**: `fire()` is currently invoking the callback (on
 *   whatever core/context called it). `drain()` spins on this bit
 *   clearing -- *never* on the subsystem's own queue state, which this
 *   header has no visibility into -- to know when it is safe to return
 *   (and therefore safe for the caller to free memory this object
 *   lives in).
 *
 * ## Why not safe to `reset()`/`cancel()` concurrently with each other
 *
 * Exactly like `callout(9)` (which requires the caller to hold its own
 * lock across competing `callout_reset`/`callout_stop` calls --
 * `callout`'s internal lock only protects the shared wheel/queue, not
 * caller-level races), this header's own atomics make `cancel()`/
 * `drain()` *safe to call concurrently with `fire()` itself* (that is
 * the entire point: no missed/doubled invocation, no racing the
 * callback's own in-flight execution), but do **not** make concurrent
 * `try_submit()`/`cancel()` calls from two different threads safe
 * against each other without the caller's own external serialization.
 * Re-arming from *inside* the firing callback itself (the common
 * "periodic" pattern: call `try_submit` again as the last thing the
 * callback does) is always safe -- only one `fire()` can ever be
 * in-flight for a given object at a time, by construction.
 *
 * ## Customizing: `Traits`
 *
 * `Traits` is a plain compile-time policy (no SFINAE-detected optional
 * members, no runtime vtable, matching `irq_guard<Traits>`/
 * `preemption_guard<Traits>` rather than the type-erased `*_ref` handles
 * in `hw/`) supplying two mandatory member *templates* -- templated on
 * the object type itself rather than fixed to one `Capacity`, so a
 * single `Traits` specialization works for every `async_kernel_object<
 * Traits, N>` regardless of `N`:
 *
 * @code
 * struct my_subsystem_traits {
 *   template <typename Obj, typename... Args>
 *   static reloco::result<void> submit(Obj &self, Args &&...args) noexcept;
 *   template <typename Obj>
 *   static reloco::result<void> cancel(Obj &self) noexcept;
 * };
 * @endcode
 *
 * - `submit` enqueues `self` into the concrete subsystem's own
 *   scheduling structure (a timer wheel bucket, a workqueue list, ...),
 *   to be `fire()`d back from whatever context that subsystem fires
 *   from; `Args...` is whatever that subsystem needs to know how/when
 *   (e.g. a single `reloco::duration` for a timer).
 * - `cancel` removes `self` from that structure -- only ever called by
 *   `async_kernel_object::cancel()` once it has already confirmed, via
 *   its own atomic `pending` bit, that `self` genuinely was still
 *   sitting there un-fired, so a correct `Traits::cancel` need not
 *   re-check this itself and need not report "wasn't pending" as its
 *   own outcome (`async_kernel_object::cancel()`'s `result<bool>`
 *   already reports that from the bit it observed).
 *
 * Optionally, `Traits` may also supply `hook_type`/`state_type` member
 * type aliases -- an intrusive link type (e.g. a wheel bucket's
 * `TAILQ_ENTRY`-equivalent) and/or whatever extra per-object bookkeeping
 * the subsystem needs (an expiry tick, a generation counter, an owning
 * CPU index, ...). Both default to an empty, zero-size placeholder type
 * if omitted. Neither is ever exposed to the arming caller: `hook()`/
 * `state()` are private, reachable only from `Traits`'s own member
 * templates via `friend Traits` -- the same "only the trusted
 * customization point may touch this" restriction `timer_ref.hpp`'s
 * vtable dispatch achieves through type erasure instead, here achieved
 * through C++ friendship, matching the compile-time (not runtime-bound)
 * nature of this customization point.
 *
 * ## Why owning, unlike `timer_ref`/`uart_ref`/`hw_rng_ref`
 *
 * Those `hw/` `*_ref` handles are two-word, non-owning *views* over a
 * caller-owned backend -- correct for a backend that already exists for
 * the handle's entire lifetime. An asynchronous callback is different:
 * by definition, something else may still be invoking it at a time the
 * arming caller does not control, so *something* must own the callback
 * storage for as long as that invocation needs it to remain valid --
 * either the caller (awkward: it must then independently track whether
 * a fire is still in flight before ever touching/freeing that storage,
 * exactly the hazard this header exists to prevent) or this object
 * itself. `async_kernel_object` owns its callback in a fixed-`Capacity`,
 * zero-allocation `reloco::inplace_function<void(async_kernel_object &),
 * Capacity>` -- never a borrowed `reloco::function_ref` (right for
 * `timer_ref::set_callback`, wrong here: nothing else keeps the
 * referenced callable alive once `try_submit` returns).
 */

#include <reloco/atomic_ops.hpp>
#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/inplace_function.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace structo {

using namespace reloco;

namespace detail {

// Zero-size placeholder used when `Traits` doesn't supply the optional
// `hook_type`/`state_type` member aliases.
struct async_kernel_object_no_aux {};

template <typename Traits, typename = void> struct async_kernel_object_hook_of {
  using type = async_kernel_object_no_aux;
};
template <typename Traits> struct async_kernel_object_hook_of<Traits, std::void_t<typename Traits::hook_type>> {
  using type = typename Traits::hook_type;
};

template <typename Traits, typename = void> struct async_kernel_object_state_of {
  using type = async_kernel_object_no_aux;
};
template <typename Traits> struct async_kernel_object_state_of<Traits, std::void_t<typename Traits::state_type>> {
  using type = typename Traits::state_type;
};

} // namespace detail

/**
 * @brief Memory-safe, owning base for any kernel object whose completion
 * fires asynchronously, from a context the arming caller does not
 * control. See the @file-level docs above for the full design rationale.
 *
 * Never copyable or movable: `Traits::submit`/`cancel` operate on this
 * object's own address (e.g. as an intrusive list/wheel-bucket node), so
 * its identity -- not merely its value -- must stay fixed for as long as
 * it may be `pending`/`firing`.
 */
template <typename Traits, std::size_t Capacity = 32> class RELOCO_OWNER async_kernel_object {
public:
  /** @brief Spin-loop iteration bound used by the default-argument overload of `drain`. */
  static constexpr std::uint32_t default_drain_max_spins = 1'000'000;

  using callback_type = inplace_function<void(async_kernel_object &), Capacity>;

  constexpr async_kernel_object() noexcept = default;

  async_kernel_object(const async_kernel_object &) = delete;
  async_kernel_object &operator=(const async_kernel_object &) = delete;
  async_kernel_object(async_kernel_object &&) = delete;
  async_kernel_object &operator=(async_kernel_object &&) = delete;

  /**
   * @brief Traps (via `RELOCO_ASSERT`) if this object is still
   * `pending`/`firing` at destruction time -- the exact use-after-free
   * hazard this header exists to prevent. Callers must `drain()` (or
   * confirm `cancel()` returned successfully and no concurrent `fire()`
   * could possibly still be in flight) before letting this object's
   * storage be reclaimed.
   */
  ~async_kernel_object() {
    RELOCO_ASSERT((flags_.load(std::memory_order_acquire) & (flag_pending | flag_firing)) == 0,
                  "async_kernel_object: destroyed while still pending/firing -- call drain() first");
  }

  /**
   * @brief Stores @p callback and submits this object to the subsystem
   * via `Traits::submit(*this, args...)`, forwarding @p args verbatim
   * (e.g. a single `reloco::duration` for a timer-wheel `Traits`).
   *
   * If this object is already `pending`, it is implicitly `cancel()`ed
   * first (mirroring `callout_reset`'s own "replace whatever was
   * previously armed" semantics) -- *not* safe to call concurrently with
   * another thread's `try_submit`/`cancel` on the same object without
   * external serialization (see the @file-level docs above); always
   * safe to call as the last thing a firing callback does, to re-arm
   * itself.
   *
   * On `Traits::submit` failure, this object is left `inactive`/
   * not-`pending` (not left half-armed) and the failure is propagated.
   */
  template <typename F, typename... Args> [[nodiscard]] result<void> try_submit(F &&callback, Args &&...args) noexcept {
    static_assert(std::is_invocable_v<std::decay_t<F> &, async_kernel_object &>,
                  "async_kernel_object: callback must be invocable as void(async_kernel_object &)");
    if (is_pending()) {
      auto c = cancel();
      if (!c)
        return unexpected(c.error());
    }
    callback_ = std::forward<F>(callback);
    flags_.fetch_or(flag_active | flag_pending, std::memory_order_release);
    auto r = Traits::template submit<async_kernel_object>(*this, std::forward<Args>(args)...);
    if (!r) {
      flags_.fetch_and(static_cast<std::uint32_t>(~(flag_active | flag_pending)), std::memory_order_release);
      return unexpected(r.error());
    }
    return {};
  }

  /**
   * @brief Non-blocking cancel: clears `active` unconditionally, then
   * atomically clears `pending` and -- only if it was actually still
   * set -- calls `Traits::cancel(*this)` to remove this object from the
   * subsystem's own scheduling structure.
   *
   * Does **not** wait for a concurrently in-flight `fire()` on another
   * context to finish (use `drain()` for that). Returns whether this
   * object was actually still `pending` (a scheduled fire was
   * genuinely prevented), mirroring `callout_stop()`'s own return
   * value -- `false` is the ordinary, non-error outcome when this
   * object had already fired or was never submitted.
   */
  [[nodiscard]] result<bool> cancel() noexcept {
    flags_.fetch_and(static_cast<std::uint32_t>(~flag_active), std::memory_order_release);
    auto prev = flags_.fetch_and(static_cast<std::uint32_t>(~flag_pending), std::memory_order_acq_rel);
    if ((prev & flag_pending) == 0)
      return false;
    auto r = Traits::template cancel<async_kernel_object>(*this);
    if (!r)
      return unexpected(r.error());
    return true;
  }

  /**
   * @brief Blocking cancel: `cancel()` followed by spinning (up to
   * @p max_spins iterations) until any concurrently in-flight `fire()`
   * finishes -- the one operation that actually guarantees it is safe
   * to free this object's memory afterwards, matching `callout_drain()`.
   *
   * Must **not** be called from inside this object's own firing
   * callback (it would spin forever waiting for its own, still-running
   * invocation to finish) -- exactly the restriction `callout_drain()`
   * itself documents.
   * Fails with `error::timed_out` if `max_spins` is exhausted while a
   * fire is still in flight, or whatever `cancel()` itself reports.
   */
  [[nodiscard]] result<void> drain(std::uint32_t max_spins = default_drain_max_spins) noexcept {
    auto c = cancel();
    if (!c)
      return unexpected(c.error());
    for (std::uint32_t i = 0; i < max_spins; ++i) {
      if ((flags_.load(std::memory_order_acquire) & flag_firing) == 0)
        return {};
    }
    return unexpected(error::timed_out);
  }

  /**
   * @brief Clears `active` without touching `pending`: a subsequent
   * `fire()` still fires (and still clears `pending`/the subsystem's
   * own schedule entry for this object), but silently skips invoking
   * the callback, exactly `callout_deactivate()`'s "let it expire
   * silently" semantics.
   */
  void deactivate() noexcept { flags_.fetch_and(static_cast<std::uint32_t>(~flag_active), std::memory_order_release); }

  /** @brief Whether this object is currently submitted to -- and not yet picked up by -- the subsystem. */
  [[nodiscard]] bool is_pending() const noexcept {
    return (flags_.load(std::memory_order_acquire) & flag_pending) != 0;
  }

  /** @brief Whether this object is armed and not yet `deactivate`d/`cancel`ed. */
  [[nodiscard]] bool is_active() const noexcept { return (flags_.load(std::memory_order_acquire) & flag_active) != 0; }

  /** @brief Whether `fire()` is currently invoking the callback (on some context). */
  [[nodiscard]] bool is_firing() const noexcept { return (flags_.load(std::memory_order_acquire) & flag_firing) != 0; }

private:
  friend Traits;

  static constexpr std::uint32_t flag_active = 1u << 0;
  static constexpr std::uint32_t flag_pending = 1u << 1;
  static constexpr std::uint32_t flag_firing = 1u << 2;

  /** @brief Intrusive hook reserved for `Traits`'s own scheduling structure. */
  [[nodiscard]] typename detail::async_kernel_object_hook_of<Traits>::type &hook() noexcept { return hook_; }

  /** @brief Extra per-object bookkeeping reserved for `Traits`. */
  [[nodiscard]] typename detail::async_kernel_object_state_of<Traits>::type &state() noexcept { return state_; }

  /**
   * @brief Called by `Traits::submit`'s underlying subsystem exactly
   * when this object becomes due -- from whatever context that is
   * (interrupt handler, softirq, wheel-sweep thread, ...).
   *
   * Atomically claims `pending` (clearing it, setting `firing`) in one
   * step: if `pending` was already clear (raced with a concurrent
   * `cancel()`), gives up immediately without invoking the callback.
   * Otherwise invokes the callback iff `active` was still set at the
   * moment of that same atomic claim, then clears `firing` -- preserving
   * whatever `active`/`pending` a self-re-`try_submit` from inside the
   * callback itself may have set in the meantime.
   */
  void fire() noexcept {
    auto claimed = atomic::fetch_update(flags_, std::memory_order_acq_rel, std::memory_order_acquire,
                                        [](std::uint32_t cur) -> optional<std::uint32_t> {
                                          if ((cur & flag_pending) == 0)
                                            return nullopt;
                                          return static_cast<std::uint32_t>((cur & ~flag_pending) | flag_firing);
                                        });
    if (!claimed)
      return;
    if ((claimed.value() & flag_active) != 0 && callback_.has_value())
      callback_(*this);
    flags_.fetch_and(static_cast<std::uint32_t>(~flag_firing), std::memory_order_release);
  }

  typename detail::async_kernel_object_hook_of<Traits>::type hook_{};
  typename detail::async_kernel_object_state_of<Traits>::type state_{};
  callback_type callback_;
  std::atomic<std::uint32_t> flags_{0};
};

} // namespace structo
