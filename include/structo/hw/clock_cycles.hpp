// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file clock_cycles.hpp
 * @brief `structo::hw::cycles`, an opaque raw hardware clock-cycle count,
 * plus `checked_duration_to_cycles`/`checked_cycles_to_duration`:
 * fallible, integer-only conversion between it and a `reloco::duration`
 * (`duration.hpp`), given a runtime clock frequency in Hz.
 *
 * ## Why `cycles` is its own type, not a bare `std::uint64_t`
 *
 * A raw cycle count (a free-running counter sample, like `CNTVCT_EL0`/
 * the TSC/RISC-V `mtime`, or a span between two such samples) is a
 * different *unit* from a nanosecond count, a `duration`, or an
 * unrelated plain integer -- exactly the kind of unit confusion
 * `target_ptr`/`phys_addr` wrap a raw integer address to prevent (see
 * `target_ptr.hpp`'s file-level docs). `cycles` is the same idea applied
 * to a tick count: a thin, zero-overhead wrapper that can only be
 * compared/converted/arithmetic'd through its own explicit API, never
 * accidentally added to a byte offset or passed where a millisecond
 * count was expected.
 *
 * Matching `target_ptr`'s own choice, `cycles` deliberately has **no
 * bare `operator+`/`operator-`**: a plain `+`/`-` on a wrapped unsigned
 * integer silently wraps on overflow with no visible indication at the
 * call site, exactly the implicit failure mode reloco's
 * `checked_*`/`wrapping_*`/`saturating_*` family (`int_ops.hpp`) exists
 * to make explicit. Pick whichever of `checked_add`/`wrapping_add`/
 * `saturating_add` (and their `_sub`/`_mul` counterparts) states the
 * intended overflow behavior at the call site -- `wrapping_add`/
 * `wrapping_sub` in particular also directly matches how the free-
 * running hardware counters `cycles` models actually behave in silicon
 * (a `CNTVCT_EL0`/TSC sample wraps modulo its own counter width, not
 * `std::uint64_t`'s full width, but modulo-2^64 wraparound is the
 * closest portable match available here).
 *
 * ## Why `checked_duration_to_cycles`/`checked_cycles_to_duration` are
 * separate from `reloco::duration_cast<T>`
 *
 * `duration.hpp`'s own `duration_cast<T>(d)` converts a `duration` to
 * another fixed *type* (`struct timespec`, `struct timeval`, a kernel
 * `sbintime_t`, ...) via the `duration_converter<T>` compile-time
 * customization point -- there is exactly one conversion per `T`, known
 * at compile time. Converting to/from `cycles` is a genuinely different
 * problem: the "exchange rate" between a duration and a cycle count is a
 * *runtime* value -- the counting clock's frequency in Hz -- read off
 * real hardware (a generic timer's `CNTFRQ_EL0`, a calibrated TSC rate,
 * a RISC-V `mtime` tick rate, `timer_capabilities::clock_hz` from
 * `timer_ref.hpp`, ...), not something `duration_converter<T>`'s
 * one-specialization-per-type shape can express. This header's two
 * functions take that frequency as a plain runtime argument instead.
 *
 * Kept independent of `timer_ref.hpp` (`structo::hw`'s type-erased timer
 * handle): plenty of raw cycle counters (`CNTVCT_EL0`, the TSC, RISC-V
 * `mtime`) are read directly, with no `timer_ref`/`timer_traits` backend
 * of their own, and still need exactly this conversion.
 *
 * ## Why fallible (`result<T>`, not a plain return)
 *
 * Both directions are plain integer multiply/divide -- no floating
 * point, matching `duration.hpp`'s own no-FPU rationale -- but a cycle
 * count close to `UINT64_MAX` at a multi-GHz frequency can genuinely
 * overflow a 64-bit intermediate product. Rather than silently wrapping
 * or trapping, both conversions here use `int_ops.hpp`'s
 * `checked_mul`/`checked_add` throughout and report
 * `error::integer_overflow` on overflow, and `error::invalid_argument`
 * for an invalid (`0`) clock frequency.
 */

#include <reloco/detail/compat.hpp>
#include <reloco/duration.hpp>
#include <reloco/error.hpp>
#include <reloco/int_ops.hpp>

#include <cstdint>

namespace structo {

using namespace reloco;

namespace hw {

/**
 * @brief An opaque, raw hardware clock-cycle count -- either an absolute
 * counter reading (e.g. a free-running `CNTVCT_EL0`/TSC sample) or a
 * span between two such readings (e.g. the "how many cycles did this
 * take" result of `checked_sub`-ing one reading from another). See the
 * file-level docs above for the full rationale.
 */
class cycles {
public:
  using value_type = std::uint64_t;

  /** @brief The "zero cycles" value -- a valid span (no time elapsed),
   * though not a meaningful absolute counter reading on its own. */
  constexpr cycles() noexcept = default;

  /** @brief Wraps an existing raw tick count (e.g. a value just read
   * from a hardware counter register). Marked `explicit`: wrapping a raw
   * integer is always a deliberate step, never an implicit conversion. */
  constexpr explicit cycles(value_type raw) noexcept : raw_(raw) {}

  /** @brief The wrapped raw tick count. */
  [[nodiscard]] constexpr value_type raw() const noexcept { return raw_; }

  [[nodiscard]] friend constexpr bool operator==(const cycles &a, const cycles &b) noexcept { return a.raw_ == b.raw_; }
  [[nodiscard]] friend constexpr bool operator!=(const cycles &a, const cycles &b) noexcept { return a.raw_ != b.raw_; }
  [[nodiscard]] friend constexpr bool operator<(const cycles &a, const cycles &b) noexcept { return a.raw_ < b.raw_; }
  [[nodiscard]] friend constexpr bool operator<=(const cycles &a, const cycles &b) noexcept { return a.raw_ <= b.raw_; }
  [[nodiscard]] friend constexpr bool operator>(const cycles &a, const cycles &b) noexcept { return a.raw_ > b.raw_; }
  [[nodiscard]] friend constexpr bool operator>=(const cycles &a, const cycles &b) noexcept { return a.raw_ >= b.raw_; }

  // --------------------------------------------------------------------
  // Rust-flavored checked/wrapping/saturating arithmetic (int_ops.hpp),
  // matching target_ptr.hpp's own pointer-arithmetic family -- see the
  // file-level docs above for why there is no bare operator+/operator-.
  // --------------------------------------------------------------------

  /** @brief Adds @p rhs, failing with `error::integer_overflow` instead
   * of wrapping, matching Rust's `u64::checked_add`. */
  [[nodiscard]] result<cycles> checked_add(cycles rhs) const noexcept {
    auto sum = reloco::checked_add<value_type>(raw_, rhs.raw_);
    if (!sum)
      return unexpected(sum.error());
    return cycles{sum.value()};
  }

  /** @brief Subtracts @p rhs, failing with `error::integer_overflow`
   * (this wrapper's underlying `std::uint64_t` has no negative value, so
   * `rhs > *this` always fails) instead of wrapping, matching Rust's
   * `u64::checked_sub`. The usual "cycles elapsed" computation between
   * two counter readings is `later.checked_sub(earlier)`. */
  [[nodiscard]] result<cycles> checked_sub(cycles rhs) const noexcept {
    auto diff = reloco::checked_sub<value_type>(raw_, rhs.raw_);
    if (!diff)
      return unexpected(diff.error());
    return cycles{diff.value()};
  }

  /** @brief Scales by @p factor, failing with `error::integer_overflow`
   * instead of wrapping, matching Rust's `u64::checked_mul`. */
  [[nodiscard]] result<cycles> checked_mul(value_type factor) const noexcept {
    auto prod = reloco::checked_mul<value_type>(raw_, factor);
    if (!prod)
      return unexpected(prod.error());
    return cycles{prod.value()};
  }

  /** @brief Adds @p rhs with well-defined modulo-2^64 wraparound,
   * matching Rust's `u64::wrapping_add` -- also the natural operation
   * for the free-running hardware counters `cycles` models, which
   * themselves wrap on overflow rather than trap. */
  [[nodiscard]] constexpr cycles wrapping_add(cycles rhs) const noexcept {
    return cycles{reloco::wrapping_add<value_type>(raw_, rhs.raw_)};
  }

  /** @copydoc wrapping_add */
  [[nodiscard]] constexpr cycles wrapping_sub(cycles rhs) const noexcept {
    return cycles{reloco::wrapping_sub<value_type>(raw_, rhs.raw_)};
  }

  /** @brief Scales by @p factor with well-defined modulo-2^64
   * wraparound, matching Rust's `u64::wrapping_mul`. */
  [[nodiscard]] constexpr cycles wrapping_mul(value_type factor) const noexcept {
    return cycles{reloco::wrapping_mul<value_type>(raw_, factor)};
  }

  /** @brief Adds @p rhs, clamping to `UINT64_MAX` instead of
   * overflowing, matching Rust's `u64::saturating_add`. */
  [[nodiscard]] constexpr cycles saturating_add(cycles rhs) const noexcept {
    return cycles{reloco::saturating_add<value_type>(raw_, rhs.raw_)};
  }

  /** @brief Subtracts @p rhs, clamping to `0` instead of overflowing
   * (negative), matching Rust's `u64::saturating_sub`. */
  [[nodiscard]] constexpr cycles saturating_sub(cycles rhs) const noexcept {
    return cycles{reloco::saturating_sub<value_type>(raw_, rhs.raw_)};
  }

  /** @brief Scales by @p factor, clamping to `UINT64_MAX` instead of
   * overflowing, matching Rust's `u64::saturating_mul`. */
  [[nodiscard]] constexpr cycles saturating_mul(value_type factor) const noexcept {
    return cycles{reloco::saturating_mul<value_type>(raw_, factor)};
  }

private:
  value_type raw_ = 0;
};

/**
 * @brief Converts @p d to a @ref cycles count at @p clock_hz, matching
 * Rust's `checked_*` naming (`int_ops.hpp`'s own `checked_mul`/
 * `checked_add`, which this is built from).
 *
 * Computed as `d.as_secs() * clock_hz + (d.subsec_nanos() * clock_hz) /
 * duration::nanos_per_sec`, with every intermediate multiplication/
 * addition overflow-checked rather than silently wrapping.
 *
 * @param d The duration to convert. A `duration` shorter than one cycle
 * at @p clock_hz converts to `cycles{0}` (floored, never rounded up),
 * matching @ref checked_cycles_to_duration's own floor-toward-zero
 * remainder handling.
 * @param clock_hz The clock's frequency, in Hz. Fails with
 * `error::invalid_argument` if `0`.
 * @return The cycle count, or `error::integer_overflow` if it would not
 * fit in a `std::uint64_t`.
 */
[[nodiscard]] inline RELOCO_CONSTEXPR20 result<cycles> checked_duration_to_cycles(duration d,
                                                                                  std::uint64_t clock_hz) noexcept {
  if (clock_hz == 0)
    return unexpected(error::invalid_argument);

  auto whole_secs_cycles = checked_mul<std::uint64_t>(d.as_secs(), clock_hz);
  if (!whole_secs_cycles)
    return unexpected(whole_secs_cycles.error());

  auto subsec_scaled = checked_mul<std::uint64_t>(static_cast<std::uint64_t>(d.subsec_nanos()), clock_hz);
  if (!subsec_scaled)
    return unexpected(subsec_scaled.error());
  std::uint64_t subsec_cycles = subsec_scaled.value() / duration::nanos_per_sec;

  auto total = checked_add<std::uint64_t>(whole_secs_cycles.value(), subsec_cycles);
  if (!total)
    return unexpected(total.error());
  return cycles{total.value()};
}

/**
 * @brief Converts @p c back to a `duration`, the inverse of
 * @ref checked_duration_to_cycles.
 *
 * Computed as `c.raw() / clock_hz` whole seconds plus `(c.raw() %
 * clock_hz) * duration::nanos_per_sec / clock_hz` sub-second
 * nanoseconds, with the one intermediate multiplication
 * overflow-checked rather than silently wrapping (the whole-seconds
 * division/modulo by a nonzero `clock_hz` can never itself overflow).
 *
 * @param c The cycle count to convert.
 * @param clock_hz The clock's frequency, in Hz. Fails with
 * `error::invalid_argument` if `0`.
 * @return The equivalent `duration`, or `error::integer_overflow` in the
 * (practically unreachable at any real hardware clock frequency) case
 * where `(c.raw() % clock_hz) * duration::nanos_per_sec` would not fit
 * in a `std::uint64_t`.
 */
[[nodiscard]] inline RELOCO_CONSTEXPR20 result<duration> checked_cycles_to_duration(cycles c,
                                                                                    std::uint64_t clock_hz) noexcept {
  if (clock_hz == 0)
    return unexpected(error::invalid_argument);

  std::uint64_t whole_secs = c.raw() / clock_hz;
  std::uint64_t remainder_cycles = c.raw() % clock_hz;

  auto scaled_remainder = checked_mul<std::uint64_t>(remainder_cycles, duration::nanos_per_sec);
  if (!scaled_remainder)
    return unexpected(scaled_remainder.error());
  std::uint64_t subsec_nanos = scaled_remainder.value() / clock_hz;

  return duration::from_secs(whole_secs) + duration::from_nanos(subsec_nanos);
}

} // namespace hw
} // namespace structo
