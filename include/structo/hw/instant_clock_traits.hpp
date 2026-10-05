// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file instant_clock_traits.hpp
 * @brief Bridges `structo::hw::time_manager` to `reloco::instant`'s
 * `instant_clock_traits<Tag>` customization point (`reloco/instant.hpp`),
 * so `reloco::instant_clock_traits<Tag>::now()` -- and, if a kernel also
 * sets `RELOCO_INSTANT_CLOCK_TAG` to one of the tags below,
 * `reloco::instant::now()`/`elapsed()` themselves -- can be backed by a
 * kernel's own `time_manager` instead of `clock_gettime`/
 * `QueryPerformanceCounter`.
 *
 * ## The `time_manager` instance: a kernel-supplied getter, not an internal global
 *
 * `instant_clock_traits<Tag>::now()` is a stateless, zero-argument,
 * `noexcept` static function -- it has no way to receive a reference to
 * *which* `time_manager` to read from. Rather than own a hidden
 * global/singleton here (and the static-initialization-order questions
 * that would raise in a freestanding target), this header declares
 * @ref kernel_time_manager as a customization point with **no
 * definition** -- exactly one definition, returning a reference to
 * whatever `time_manager` instance the embedding kernel already
 * maintains (its own global, or a function-local `static` wrapping it;
 * already `start()`-ed before any use), must be linked into the final
 * binary.
 *
 * ## Failure handling is a per-kernel, per-architecture policy decision
 *
 * `time_manager::try_monotonic_now()`/`try_realtime_now()` are fallible
 * (`error::not_initialized` before `start()`, a propagated conversion
 * error, or an inherited seqlock-retry exhaustion -- see `try_now`'s own
 * docs in `time_manager.hpp`), but `instant_clock_traits<Tag>::now()`
 * must return a bare `reloco::duration` unconditionally, and typical
 * code calling it never expects (or checks for) a failure at all. There
 * is no single right answer for what to substitute instead -- it depends
 * on the architecture's own counter guarantees and what the rest of the
 * kernel assumes about monotonicity:
 *
 * - An architecture whose counter read essentially cannot fail except
 *   transiently (e.g. a momentarily-exhausted seqlock retry bound racing
 *   a writer) may prefer to retry in a tight loop until it succeeds,
 *   since any substitute value (even a cached one) could violate strict
 *   monotonic invariants other subsystems assert on.
 * - An architecture with a known-fragile primary counter may instead
 *   prefer a one-shot recovery action (e.g. `switch_source()` to a
 *   fallback counter) before retrying, falling back to the last-known
 *   instant only if that also fails.
 *
 * This is therefore itself an opt-in customization point, @ref
 * time_manager_clock_failure_policy<Tag>, mirroring every other
 * `*_traits<Tag>`-style hook in `structo` (`time_source_traits`,
 * `hw_rng_traits`, ...): **left undefined** until the embedding kernel
 * specializes it for whichever of @ref kernel_monotonic_clock_tag /
 * @ref kernel_realtime_clock_tag it uses -- a specialization must supply
 * exactly two functions:
 *
 * @code
 * template <> struct structo::hw::time_manager_clock_failure_policy<structo::hw::kernel_monotonic_clock_tag> {
 *   // Called only once try_monotonic_now()/try_realtime_now() has itself already failed; must still
 *   // return some duration unconditionally (never throws/aborts/returns a result<>).
 *   static reloco::duration recover(structo::hw::time_manager &mgr, reloco::error err) noexcept;
 *   // Called on every *successful* read (see @ref clock_now), so a policy that wants a fallback cache can
 *   // keep one warm; a policy that never needs one (e.g. the ARM example below) can leave this a no-op.
 *   static void observe(reloco::duration value) noexcept;
 * };
 * @endcode
 *
 * Note that @ref clock_now calls these two functions on whichever `time_manager_clock_failure_policy<Tag>`
 * is actually active for `Tag` -- never unconditionally on some other, unrelated template -- so a `Tag` that
 * doesn't want a fallback cache pays no cost for one, and nothing is implicitly instantiated for a `Tag`
 * behind the embedding kernel's back.
 *
 * One ready-made policy implementing both is provided for convenience, @ref trap_time_failure_policy --
 * opt into it by having your own `time_manager_clock_failure_policy<Tag>` specialization inherit from it,
 * rather than reimplementing the same trap by hand:
 *
 * @code
 * // ARM: an exhausted seqlock retry or not-yet-started manager is assumed transient; spin until the
 * // next attempt succeeds rather than ever return a value that could violate monotonicity elsewhere.
 * template <> struct structo::hw::time_manager_clock_failure_policy<structo::hw::kernel_monotonic_clock_tag> {
 *   static void observe(reloco::duration) noexcept {}
 *   static reloco::duration recover(structo::hw::time_manager &mgr, reloco::error) noexcept {
 *     for (;;) {
 *       if (auto now = mgr.try_monotonic_now())
 *         return *now;
 *     }
 *   }
 * };
 *
 * // x86: an unstable TSC occasionally needs a one-shot fallback switch before it is trustworthy
 * // again; fall back to the built-in trap if even that does not recover -- there is no sane substitute
 * // duration left to fabricate at that point.
 * template <> struct structo::hw::time_manager_clock_failure_policy<structo::hw::kernel_monotonic_clock_tag> {
 *   static void observe(reloco::duration) noexcept {}
 *   static reloco::duration recover(structo::hw::time_manager &mgr, reloco::error err) noexcept {
 *     static std::atomic<bool> switched_once{false};
 *     if (!switched_once.exchange(true, std::memory_order_relaxed)) {
 *       (void)mgr.switch_source(fallback_platform_timer_ref(), structo::hw::vdso_clock_source::riscv_time);
 *       if (auto now = mgr.try_monotonic_now())
 *         return *now;
 *     }
 *     return structo::hw::trap_time_failure_policy<structo::hw::kernel_monotonic_clock_tag>::recover(mgr, err);
 *   }
 * };
 * @endcode
 */

#include <structo/hw/time_manager.hpp>

#include <reloco/detail/assert.hpp>
#include <reloco/duration.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/instant.hpp>

namespace structo {
namespace hw {

/**
 * @brief Customization point: the system-wide `time_manager` instance @ref kernel_monotonic_clock_tag /
 * @ref kernel_realtime_clock_tag's `instant_clock_traits` specializations read from. **No definition is
 * provided here** -- the embedding kernel must supply exactly one, returning a reference to whichever
 * `time_manager` it already maintains. See the @file-level docs.
 */
[[nodiscard]] time_manager &kernel_time_manager() noexcept;

/** @brief Tag selecting `time_manager::try_monotonic_now()` (via @ref kernel_time_manager) as a clock source for
 * `reloco::instant_clock_traits`. See the @file-level docs. */
struct kernel_monotonic_clock_tag {};

/** @brief Tag selecting `time_manager::try_realtime_now()` (via @ref kernel_time_manager) as a clock source for
 * `reloco::instant_clock_traits` -- prefer @ref kernel_monotonic_clock_tag unless wall-clock time is
 * specifically needed (see `instant.hpp`'s own file-level docs on what `reloco::instant` otherwise models). */
struct kernel_realtime_clock_tag {};

/**
 * @brief Customization point invoked by @ref clock_now on every `time_manager::try_monotonic_now()`/
 * `try_realtime_now()` read for @p Tag -- @ref recover decides what `instant_clock_traits<Tag>::now()` reports
 * once a read has itself already failed (it must still return some `reloco::duration`, unconditionally, never
 * throw/abort); @ref observe is called on every *successful* read instead, so a policy that wants a fallback
 * cache can keep one warm (a policy that never needs one can make it a no-op). **Left undefined** for any
 * `Tag` that hasn't opted in -- a kernel that uses @ref kernel_monotonic_clock_tag/@ref
 * kernel_realtime_clock_tag without specializing this for it gets a compile error (incomplete type), not a
 * silently-chosen default. See the @file-level docs for worked architecture-specific examples and @ref
 * trap_time_failure_policy for a ready-made, deliberately unforgiving option.
 */
template <typename Tag> struct time_manager_clock_failure_policy;

/**
 * @brief Ready-made @ref time_manager_clock_failure_policy implementation: @ref recover traps
 * (`RELOCO_ASSERT`) unconditionally. A fabricated fallback duration (zero, stale/cached, or otherwise) can
 * silently violate monotonicity/invariants elsewhere just as badly as a crash -- if a `Tag` has no real
 * recovery path (an ARM-style retry loop or an x86-style `switch_source()` fallback), that is treated as a
 * genuine, unrecoverable condition rather than something to paper over.
 *
 * Opt in the same way any other policy would:
 *
 * @code
 * template <> struct structo::hw::time_manager_clock_failure_policy<structo::hw::kernel_monotonic_clock_tag>
 *     : structo::hw::trap_time_failure_policy<structo::hw::kernel_monotonic_clock_tag> {};
 * @endcode
 */
template <typename Tag> struct trap_time_failure_policy {
  /** @brief No-op: this policy has no fallback cache to maintain. */
  static void observe(reloco::duration) noexcept {}

  /** @brief Traps via `RELOCO_ASSERT` -- see the class-level docs. Only returns (the "zero" duration) if
   * assertions are compiled out for this target and no trap mechanism is available either. */
  [[nodiscard]] static reloco::duration recover(time_manager &, reloco::error) noexcept {
    RELOCO_ASSERT(false, "instant_clock_traits: time_manager clock read failed and "
                          "time_manager_clock_failure_policy<Tag> resolved to trap_time_failure_policy, "
                          "which has no recovery path by design");
    return reloco::duration{};
  }
};

namespace detail {

/** @brief Shared implementation for both `instant_clock_traits` specializations below: on success, notifies @p
 * Tag's active @ref time_manager_clock_failure_policy via `observe()` and returns the reading; on failure,
 * defers entirely to that same policy's `recover()`. */
template <typename Tag>
[[nodiscard]] inline reloco::duration clock_now(reloco::result<reloco::duration> value, time_manager &mgr) noexcept {
  if (value) {
    time_manager_clock_failure_policy<Tag>::observe(value.value());
    return value.value();
  }
  return time_manager_clock_failure_policy<Tag>::recover(mgr, value.error());
}

} // namespace detail

} // namespace hw
} // namespace structo

namespace reloco {

// `now()` below is deliberately a function template (with a single, always-defaulted dummy parameter)
// rather than an ordinary static member function. `instant_clock_traits<kernel_monotonic_clock_tag>` /
// `<kernel_realtime_clock_tag>` are themselves *explicit specializations* of `instant_clock_traits`, so an
// ordinary member function body here would be a fully concrete (non-dependent) function -- the compiler
// would have to finish instantiating everything it calls, including `time_manager_clock_failure_policy<Tag>`,
// as soon as this header is parsed, regardless of whether any kernel ever actually calls `now()`. That would
// turn the "opt-in, incomplete-type-until-specialized" contract documented above into "every translation unit
// that merely includes this header must have already specialized both policies", which defeats the point.
// Giving `now()` a template parameter makes its body dependent, deferring instantiation (and therefore the
// `time_manager_clock_failure_policy<Tag>` completeness check) to each call site -- exactly where the kernel's
// own specialization is expected to already be visible -- without changing how callers invoke it (a
// defaulted template parameter still lets plain `now()` calls deduce it).

template <> struct instant_clock_traits<structo::hw::kernel_monotonic_clock_tag> {
  template <typename = void>
  [[nodiscard]] static duration now() noexcept {
    auto &mgr = structo::hw::kernel_time_manager();
    return structo::hw::detail::clock_now<structo::hw::kernel_monotonic_clock_tag>(mgr.try_monotonic_now(), mgr);
  }
};

template <> struct instant_clock_traits<structo::hw::kernel_realtime_clock_tag> {
  template <typename = void>
  [[nodiscard]] static duration now() noexcept {
    auto &mgr = structo::hw::kernel_time_manager();
    return structo::hw::detail::clock_now<structo::hw::kernel_realtime_clock_tag>(mgr.try_realtime_now(), mgr);
  }
};

} // namespace reloco
