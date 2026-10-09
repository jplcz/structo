// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file time_source_ref.hpp
 * @brief `structo::hw::time_source_ref`: a type-erased, non-owning
 * handle over a free-running hardware counter (the TSC, ARM's
 * `CNTVCT_EL0`/`CNTPCT_EL0`, RISC-V's `mtime`/`rdtime`, ...), plus the
 * `time_source_traits<Backend>` customization point a concrete backend
 * specializes to be bindable through it.
 *
 * ## Why this is separate from `timer_ref.hpp`
 *
 * `timer_ref` models a countdown/interval timer: arm it for a period,
 * get notified (via polling or a callback) when that period elapses.
 * `time_source_ref` models the opposite, complementary kind of hardware
 * clock primitive: a free-running counter that never fires anything and
 * is never "armed" -- it only ever answers "what is your raw count
 * right now", via @ref try_now. This is exactly the primitive a tickless
 * kernel-level timekeeping facility (a "timehands"/timecounter manager,
 * see the forthcoming `timehands.hpp`) needs to correlate against a
 * wall-clock/monotonic reference instant and publish through a
 * `structo::hw::vdso_clock_page` (`vdso_clock_page.hpp`) -- `timer_ref`
 * is the wrong tool for that job, since it has no notion of "the current
 * count", only "time remaining until expiry".
 *
 * ## Customization point: `time_source_traits<Backend>`
 *
 * `time_source_traits<Backend>` is left undefined for any `Backend`
 * that hasn't opted in (mirroring `uart_traits`/`hw_rng_traits`). A
 * specialization must supply exactly two functions:
 *
 * @code
 * template <> struct structo::hw::time_source_traits<my_backend> {
 *   static reloco::result<structo::hw::cycles> try_now(my_backend &) noexcept;
 *   static structo::hw::time_source_capabilities capabilities(my_backend &) noexcept;
 * };
 * @endcode
 *
 * `try_now` samples the counter's current raw value. It is expected to
 * (almost) never fail in practice -- reading a free-running counter is
 * normally a single instruction with no transient "not ready" condition
 * -- but still returns `reloco::result<cycles>` rather than a bare
 * `cycles`, both for consistency with every other fallible operation in
 * `structo`/`reloco` and to leave room for a backend that genuinely can
 * fail (e.g. one gated behind a trap-and-emulate hypervisor intercept
 * that can report `error::unsupported_operation` if trapped away).
 *
 * `capabilities` is plain, infallible static information about the
 * counter -- see @ref time_source_capabilities -- used by a caller (like
 * the forthcoming tickless timehands manager) to reason about
 * conversion precision and safe update cadence *before* ever calling
 * `try_now`, so it is not wrapped in `result<T>` at the backend level;
 * `time_source_ref::capabilities()` itself still reports
 * `error::unsupported_operation` when unbound, matching `timer_ref::
 * capabilities()`'s own convention.
 *
 * ## Example `Backend` (ARMv8-A virtual counter)
 *
 * @code
 * struct arm_cntvct_backend {};
 *
 * template <> struct structo::hw::time_source_traits<arm_cntvct_backend> {
 *   static reloco::result<structo::hw::cycles> try_now(arm_cntvct_backend &) noexcept {
 *     std::uint64_t val;
 *     asm volatile("mrs %0, cntvct_el0" : "=r"(val));
 *     return structo::hw::cycles{val};
 *   }
 *
 *   static structo::hw::time_source_capabilities capabilities(arm_cntvct_backend &) noexcept {
 *     std::uint64_t freq;
 *     asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
 *     return {.clock_hz = freq,
 *             .max_value = structo::hw::cycles{UINT64_MAX},
 *             .is_monotonic = true,
 *             .is_per_cpu = false};
 *   }
 * };
 * @endcode
 */

#include <structo/hw/clock_cycles.hpp>

#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace structo {

using namespace reloco;

namespace hw {

/**
 * @brief Static, infallible information about a free-running hardware
 * counter -- its frequency, its wraparound bound, and whether it is
 * trustworthy as a single, system-wide monotonic time source.
 */
struct time_source_capabilities {
  /** @brief The counter's frequency, in Hz. Pairs with `clock_cycles.hpp`'s `checked_duration_to_cycles`/
   * `checked_cycles_to_duration`, exactly like `timer_capabilities::clock_hz`. `0` if genuinely unknown (a
   * backend should still prefer reporting its best estimate, since `0` disables any duration conversion). */
  std::uint64_t clock_hz = 0;

  /** @brief The largest raw value `try_now` can ever return before the counter wraps back to `0` (inclusive --
   * e.g. `UINT64_MAX` for a full-width 64-bit counter, or `(1ull << 56) - 1` for a 56-bit counter packed into a
   * 64-bit register). The wrap period is therefore `(max_value.raw() + 1) / clock_hz` seconds -- used to bound how
   * long a cached reference sample may go un-refreshed before a subsequent `checked_sub` against it could
   * underflow/wrap, e.g. by the forthcoming tickless timehands manager. */
  cycles max_value{UINT64_MAX};

  /** @brief Whether this counter is guaranteed to never run backward (per read, on the core it was read from).
   * Almost always `true` -- the rare `false` case is a counter known to stutter/reset under some condition a
   * caller needs to know about (e.g. around certain low-power/suspend transitions) rather than a hard guarantee of
   * the architecture. */
  bool is_monotonic = true;

  /** @brief Whether this counter's value is specific to the current CPU core (`true` -- e.g. a non-invariant,
   * unsynchronized TSC) rather than a single, globally-consistent count shared by every core (`false` -- e.g.
   * ARM's `CNTVCT_EL0`, an invariant/synchronized TSC). A caller choosing a single system-wide time source should
   * generally require `false` here, same rationale as `timer_capabilities::is_per_cpu`. */
  bool is_per_cpu = false;

  [[nodiscard]] friend bool operator==(const time_source_capabilities &a, const time_source_capabilities &b) noexcept {
    return a.clock_hz == b.clock_hz && a.max_value == b.max_value && a.is_monotonic == b.is_monotonic &&
           a.is_per_cpu == b.is_per_cpu;
  }
  [[nodiscard]] friend bool operator!=(const time_source_capabilities &a, const time_source_capabilities &b) noexcept {
    return !(a == b);
  }
};

/**
 * @brief Opt-in customization point describing how to read a concrete
 * backend's free-running counter and describe its capabilities, through
 * @ref time_source_ref.
 *
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted, mirroring `uart_traits`/`hw_rng_traits`. See the @file-level
 * docs above for the required member list and an example.
 */
template <typename Backend> struct time_source_traits;

namespace detail {

template <typename Backend, typename = void> struct has_time_source_traits : std::false_type {};

template <typename Backend>
struct has_time_source_traits<Backend, std::void_t<decltype(time_source_traits<Backend>::try_now),
                                                   decltype(time_source_traits<Backend>::capabilities)>>
    : std::true_type {};

} // namespace detail

/**
 * @brief Type-erased, non-owning handle over a free-running hardware
 * counter, for whatever concrete backend it is bound to.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation on one fails with
 * `error::unsupported_operation` rather than trapping, mirroring
 * `timer_ref`/`hw_rng_ref`'s null-safety convention.
 *
 * Follows the single-`vtable`, resolved-once-per-`Backend` shape every
 * `structo` `*_ref` handle uses -- see
 * [`type-erased-base-containers.md`](https://github.com/jplcz/reloco/blob/master/docs/type-erased-base-containers.md)
 * and `docs/coding-guide.md`'s "Type-erase a `*_ref` handle's backend
 * behind one `vtable`" section.
 */
class RELOCO_POINTER time_source_ref {
public:
  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    result<cycles> (*now)(void *ctx) noexcept;
    time_source_capabilities (*capabilities)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr time_source_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have a
   * @ref time_source_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy of
   * it. Marked `explicit`: binding a backend is always a deliberate step,
   * never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_time_source_traits<Backend>::value, int> = 0>
  constexpr explicit time_source_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  time_source_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /**
   * @brief Samples the counter's current raw value.
   * Fails with `error::unsupported_operation` if this ref is unbound, or
   * whatever error the backend itself reports (see the @file-level
   * docs -- expected to be rare in practice).
   */
  [[nodiscard]] result<cycles> try_now() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->now(ctx_);
  }

  /**
   * @brief The bound backend's static capabilities (frequency, wraparound
   * bound, monotonicity/per-CPU flags).
   * Fails with `error::unsupported_operation` if this ref is unbound.
   */
  [[nodiscard]] result<time_source_capabilities> capabilities() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->capabilities(ctx_);
  }

private:
  template <typename Backend> static result<cycles> now_entry(void *ctx) noexcept {
    return time_source_traits<Backend>::try_now(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static time_source_capabilities capabilities_entry(void *ctx) noexcept {
    return time_source_traits<Backend>::capabilities(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static constexpr vtable s_vtbl{&now_entry<Backend>, &capabilities_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace hw
} // namespace structo
