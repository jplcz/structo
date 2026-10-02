// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file timer_ref.hpp
 * @brief `structo::hw::timer_ref`: a type-erased, non-owning handle over
 * the typical operations of a hardware countdown/interval timer --
 * arming a one-shot or auto-reload expiry, canceling it, checking
 * whether it is currently armed, polling for expiry, and querying the
 * backend's capabilities -- plus the `timer_traits<Backend>`
 * customization point a concrete backend specializes to be bindable
 * through it.
 *
 * This header is deliberately abstraction-only, the timer counterpart of
 * `uart_ref.hpp`: there is no architectural-timer/PIT/HPET register
 * poking here, no clock-to-divisor math, nothing chip-specific. A
 * concrete backend (ARM generic timer `CNTP_*_EL0`, x86 LAPIC timer/
 * HPET/TSC-deadline, RISC-V `mtimecmp`, or a unit test's software fake)
 * implements `timer_traits<Backend>` however it needs to.
 *
 * Only polled completion is modeled, matching `uart_ref`'s `tx_ready`/
 * `rx_ready`: whether a timer has expired is always checked explicitly
 * (`try_wait`), never awaited via a completion callback -- wiring an
 * actual interrupt to call back into software (if the backend supports
 * one at all) is left entirely to the backend/platform, outside this
 * header's scope.
 *
 * ## Why type-erased, like `uart_ref`/`hw_rng_ref`
 *
 * Exactly as `uart_ref`/`hw_rng_ref` erase their concrete backend behind
 * a small, fixed vtable (a two-word handle: an untyped context pointer
 * plus a `const vtable *`, no virtual base class, no RTTI, no allocation
 * of its own), `timer_ref` erases the concrete timer backend the same
 * way. This lets a single scheduler tick/watchdog-kick/timeout routine be
 * written once against `timer_ref` and handed whatever concrete timer a
 * given boot stage/platform/test actually has, decided at runtime.
 *
 * ## Rust-inspired API shape
 *
 * Two pieces of this header deliberately mirror well-known Rust API
 * shapes rather than inventing a new one:
 *
 * - **Periods and readback are `reloco::duration`** (`duration.hpp`),
 *   matching Rust's `std::time::Duration` -- see that header's own
 *   file-level docs for why `std::chrono` is not used instead
 *   (integer-only arithmetic, no `<chrono>` dependency on freestanding
 *   targets).
 * - **`try_wait()`/`wait()` follow the `embedded-hal`
 *   `nb`/non-blocking crate's `nb::Result<T, E>` "would block" shape**
 *   (also the same shape Rust's `Future::poll` uses, modulo the explicit
 *   polling vs. being driven by a waker): a non-blocking poll that
 *   returns `error::try_again` -- reused here rather than inventing a
 *   dedicated `error::would_block`, since it is exactly what
 *   `error::try_again` already documents ("the operation could not
 *   complete right now for a transient reason and may succeed if
 *   retried") -- when the timer has not expired yet, and `wait()` is the
 *   blocking spin-loop built on top, exactly mirroring `nb`'s own
 *   `nb::block!` macro and `uart_ref::put_byte`'s spin loop over
 *   `try_put_byte`.
 *
 * ## Customizing: `timer_traits<Backend>`
 *
 * `timer_traits<Backend>` is left undefined for any `Backend` that
 * hasn't opted in (mirroring `uart_traits`/`hw_rng_traits`). A
 * specialization must supply exactly four functions -- the minimal
 * "abstract operations" contract, everything else (`start`/
 * `start_periodic`/`wait`/`restart`) is synthesized generically on top
 * of these by `timer_ref` itself:
 *
 * @code
 * template <> struct structo::hw::timer_traits<my_backend> {
 *   static reloco::result<void> try_start(my_backend &, structo::hw::timer_mode, reloco::duration) noexcept;
 *   static reloco::result<void> cancel(my_backend &) noexcept;
 *   static reloco::result<bool> is_active(my_backend &) noexcept;
 *   static reloco::result<void> try_wait(my_backend &) noexcept;
 * };
 * @endcode
 *
 * Optionally, a backend may also supply `remaining` (time left until the
 * next expiry, for hardware that can read its own down-counter back) and
 * `capabilities` (the backend's supported modes, period range,
 * resolution, counting-clock frequency in Hz, and whether it is a
 * per-CPU or single shared instance):
 *
 * @code
 * static reloco::result<reloco::duration> remaining(my_backend &) noexcept;
 * static reloco::result<structo::hw::timer_capabilities> capabilities(my_backend &) noexcept;
 * @endcode
 *
 * Detected via SFINAE (the same optional-member idiom
 * `uart_traits::current_config`/`hw_rng_traits::is_available` use); if
 * absent, `timer_ref::remaining()`/`timer_ref::capabilities()` fail with
 * `error::unsupported_operation`.
 *
 * `timer_capabilities::clock_hz` pairs with the separate
 * `clock_cycles.hpp` header's `checked_duration_to_cycles`/
 * `checked_cycles_to_duration` to convert a `duration` to/from this
 * backend's own opaque `structo::hw::cycles` count (e.g. to program a
 * down-counter register directly, or interpret one read back) -- kept in
 * its own header since
 * it is a plain, `timer_ref`-independent frequency/duration conversion
 * utility also useful for a raw cycle-counter backend (`CNTVCT_EL0`,
 * the TSC, RISC-V `mtime`) that isn't a `timer_ref` backend at all.
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/duration.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace structo {

using namespace reloco;

namespace hw {

// ============================================================================
// Timer Settings/Capabilities
// ============================================================================

/**
 * @brief Whether an armed timer fires once and then stops, or
 * automatically re-arms itself with the same period after every expiry.
 */
enum class timer_mode : std::uint8_t {
  /** @brief Fires once, `period` after `try_start`, then becomes inactive. */
  one_shot,
  /** @brief Fires every `period`, indefinitely, until `cancel`ed. */
  periodic,
};

/**
 * @brief The typical static capabilities of a hardware timer backend:
 * which modes it supports, the period range it can be armed for, and its
 * counting resolution.
 *
 * Aggregate, value-type metadata -- deliberately chip-agnostic: it says
 * nothing about prescaler/divisor registers or counter width, only the
 * settings every timer user cares about regardless of the concrete
 * hardware underneath.
 */
struct timer_capabilities {
  /** @brief Whether `timer_mode::one_shot` is supported by `try_start`. */
  bool supports_one_shot = true;
  /** @brief Whether `timer_mode::periodic` is supported by `try_start`. */
  bool supports_periodic = true;
  /** @brief Whether each CPU core has its own independent instance of
   * this timer (`true`, e.g. the ARM generic timer's per-core `CNTP_*`
   * registers, an x86 LAPIC timer/TSC-deadline), as opposed to a single
   * instance shared -- and requiring arbitration -- across every core
   * (`false`, e.g. a PIT/HPET). A caller binding a per-CPU backend's
   * `timer_ref` must do so once per core (the same restriction
   * `per_cpu_ptr.hpp` documents for its own per-CPU resolvers); a
   * non-per-CPU backend's single `timer_ref` may be shared/serialized
   * across cores by whatever external locking the caller already uses
   * for it. */
  bool is_per_cpu = false;
  /** @brief The shortest period `try_start` can reliably arm for. */
  duration min_period{};
  /** @brief The longest period `try_start` can reliably arm for; the
   * default-constructed "zero" duration conventionally means "no known
   * upper bound" rather than "zero". */
  duration max_period{};
  /** @brief The smallest time increment the backend's underlying counter
   * can distinguish (e.g. one tick of its counting clock); a requested
   * period is rounded to a multiple of this by the backend. The
   * default-constructed "zero" duration conventionally means "unknown". */
  duration resolution{};
  /** @brief The frequency, in Hz, of the clock the backend counts
   * against (e.g. a generic timer's `CNTFRQ_EL0`, a TSC-deadline
   * backend's calibrated TSC rate). `0` conventionally means "unknown".
   * Pairs with `clock_cycles.hpp`'s `checked_duration_to_cycles`/
   * `checked_cycles_to_duration` to convert a `duration` to/from this
   * backend's own raw cycle count. */
  std::uint64_t clock_hz = 0;

  [[nodiscard]] friend constexpr bool operator==(const timer_capabilities &a, const timer_capabilities &b) noexcept {
    return a.supports_one_shot == b.supports_one_shot && a.supports_periodic == b.supports_periodic &&
           a.min_period == b.min_period && a.max_period == b.max_period && a.resolution == b.resolution &&
           a.clock_hz == b.clock_hz && a.is_per_cpu == b.is_per_cpu;
  }
  [[nodiscard]] friend constexpr bool operator!=(const timer_capabilities &a, const timer_capabilities &b) noexcept {
    return !(a == b);
  }
};

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Opt-in customization point describing how to arm, cancel, and
 * poll a concrete hardware timer backend, through @ref timer_ref.
 *
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted, mirroring `uart_traits`/`hw_rng_traits`. See the @file-level
 * docs above for the complete required/optional member list.
 */
template <typename Backend> struct timer_traits;

namespace detail {

template <typename Backend, typename = void> struct has_timer_traits : std::false_type {};

template <typename Backend>
struct has_timer_traits<
    Backend, std::void_t<decltype(timer_traits<Backend>::try_start), decltype(timer_traits<Backend>::cancel),
                         decltype(timer_traits<Backend>::is_active), decltype(timer_traits<Backend>::try_wait)>>
    : std::true_type {};

// Detects the optional Traits::remaining readback.
template <typename Traits, typename = void> struct timer_has_remaining : std::false_type {};
template <typename Traits>
struct timer_has_remaining<Traits, std::void_t<decltype(Traits::remaining)>> : std::true_type {};

// Detects the optional Traits::capabilities readback.
template <typename Traits, typename = void> struct timer_has_capabilities : std::false_type {};
template <typename Traits>
struct timer_has_capabilities<Traits, std::void_t<decltype(Traits::capabilities)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Type-Erased Timer Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over the typical operations of a
 * hardware countdown/interval timer, for whatever concrete backend it is
 * bound to.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation on one fails with
 * `error::unsupported_operation` rather than trapping, mirroring
 * `uart_ref`/`hw_rng_ref`'s null-safety convention.
 */
class RELOCO_POINTER timer_ref {
public:
  /** @brief Spin-loop iteration bound used by the default-argument overload of `wait`. */
  static constexpr std::uint32_t default_max_spins = 1'000'000;

  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    result<void> (*try_start)(void *ctx, timer_mode mode, duration period) noexcept;
    result<void> (*cancel)(void *ctx) noexcept;
    result<bool> (*is_active)(void *ctx) noexcept;
    result<void> (*try_wait)(void *ctx) noexcept;
    result<duration> (*remaining)(void *ctx) noexcept;
    result<timer_capabilities> (*capabilities)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr timer_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have a
   * @ref timer_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy of
   * it. Marked `explicit`: binding a backend is always a deliberate step,
   * never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_timer_traits<Backend>::value, int> = 0>
  constexpr explicit timer_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  timer_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  // --------------------------------------------------------------------
  // Mandatory backend operations (directly forwarded).
  // --------------------------------------------------------------------

  /**
   * @brief Arms the timer to expire `period` from now, in mode @p mode
   * (replacing whatever arming was previously in effect, if any -- a
   * backend need not require an explicit `cancel()` first).
   */
  [[nodiscard]] result<void> try_start(timer_mode mode, duration period) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->try_start(ctx_, mode, period);
  }

  /**
   * @brief Disarms the timer. Idempotent: canceling an already-inactive
   * timer is not an error.
   */
  [[nodiscard]] result<void> cancel() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->cancel(ctx_);
  }

  /**
   * @brief Whether the timer is currently armed and counting down. A
   * `timer_mode::one_shot` timer that has already fired, or a timer that
   * was never started/was `cancel`ed, reports `false`.
   */
  [[nodiscard]] result<bool> is_active() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->is_active(ctx_);
  }

  /**
   * @brief Non-blocking poll for expiry, in the same "would block" shape
   * as Rust's `embedded-hal`/`nb` crate (see the @file-level docs
   * above): fails with `error::try_again` if the timer has not expired
   * since the last successful `try_wait`/`wait`, otherwise reports the
   * expiry (clearing whatever pending-expiry latch the backend uses, so
   * the next poll again reports `error::try_again` until the next
   * expiry -- including, for `timer_mode::periodic`, the *next* period's
   * expiry).
   */
  [[nodiscard]] result<void> try_wait() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->try_wait(ctx_);
  }

  // --------------------------------------------------------------------
  // Optional backend operations (directly forwarded, with a generic
  // `error::unsupported_operation` fallback when the backend does not
  // implement them).
  // --------------------------------------------------------------------

  /**
   * @brief The time remaining until the next expiry.
   * Fails with `error::unsupported_operation` if this ref is unbound, if
   * the bound backend does not implement the optional
   * `timer_traits::remaining`, or if the timer is not currently active.
   */
  [[nodiscard]] result<duration> remaining() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->remaining(ctx_);
  }

  /**
   * @brief The bound backend's static capabilities (supported modes,
   * period range, resolution).
   * Fails with `error::unsupported_operation` if this ref is unbound, or
   * if the bound backend does not implement the optional
   * `timer_traits::capabilities`.
   */
  [[nodiscard]] result<timer_capabilities> capabilities() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->capabilities(ctx_);
  }

  // --------------------------------------------------------------------
  // Generic conveniences, synthesized purely from the four mandatory
  // operations above -- no further backend support is required for any
  // of these.
  // --------------------------------------------------------------------

  /** @brief `try_start(timer_mode::one_shot, period)`: arms a single
   * expiry `period` from now. */
  [[nodiscard]] result<void> start(duration period) const noexcept { return try_start(timer_mode::one_shot, period); }

  /** @brief `try_start(timer_mode::periodic, period)`: arms a
   * recurring expiry, every `period`, until `cancel`ed. */
  [[nodiscard]] result<void> start_periodic(duration period) const noexcept {
    return try_start(timer_mode::periodic, period);
  }

  /** @brief `cancel()` followed by `try_start(mode, period)`: disarms
   * whatever was previously armed (if anything) and arms a fresh
   * `period`-from-now expiry, discarding any partially-elapsed previous
   * countdown. Most backends don't actually require the `cancel()`
   * (`try_start` already replaces prior arming, see its own docs above),
   * but issuing it explicitly keeps `restart`'s intent unambiguous even
   * on a backend that needs it. */
  [[nodiscard]] result<void> restart(timer_mode mode, duration period) const noexcept {
    auto c = cancel();
    if (!c)
      return c;
    return try_start(mode, period);
  }

  /**
   * @brief Blocks (spinning on `try_wait`) until the timer expires, for
   * up to `max_spins` iterations -- the `nb::block!`-equivalent blocking
   * wrapper over `try_wait`'s non-blocking poll (see the @file-level
   * docs above).
   * Fails with `error::timed_out` if `max_spins` is exhausted, or
   * whatever the backend itself reports.
   */
  [[nodiscard]] result<void> wait(std::uint32_t max_spins = default_max_spins) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    for (std::uint32_t i = 0; i < max_spins; ++i) {
      auto r = vtbl_->try_wait(ctx_);
      if (r || r.error() != error::try_again)
        return r;
    }
    return unexpected(error::timed_out);
  }

private:
  template <typename Backend>
  static result<void> try_start_entry(void *ctx, timer_mode mode, duration period) noexcept {
    return timer_traits<Backend>::try_start(*static_cast<Backend *>(ctx), mode, period);
  }

  template <typename Backend> static result<void> cancel_entry(void *ctx) noexcept {
    return timer_traits<Backend>::cancel(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static result<bool> is_active_entry(void *ctx) noexcept {
    return timer_traits<Backend>::is_active(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static result<void> try_wait_entry(void *ctx) noexcept {
    return timer_traits<Backend>::try_wait(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static result<duration> remaining_entry(void *ctx) noexcept {
    using traits = timer_traits<Backend>;
    if constexpr (detail::timer_has_remaining<traits>::value) {
      return traits::remaining(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend> static result<timer_capabilities> capabilities_entry(void *ctx) noexcept {
    using traits = timer_traits<Backend>;
    if constexpr (detail::timer_has_capabilities<traits>::value) {
      return traits::capabilities(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return unexpected(error::unsupported_operation);
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{&try_start_entry<Backend>, &cancel_entry<Backend>,    &is_active_entry<Backend>,
                                 &try_wait_entry<Backend>,  &remaining_entry<Backend>, &capabilities_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace hw
} // namespace structo
