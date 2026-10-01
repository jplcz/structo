// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hw_rng.hpp
 * @brief `hw_rng_traits` fallback backends for ARMv7-A/AArch32: a weak
 * jitter combiner built on the (optional) Generic Timer's physical
 * (`CNTPCT`) and virtual (`CNTVCT`) counters.
 *
 * ARMv7-A has no baseline architectural hardware-RNG instruction
 * (unlike AArch64's `FEAT_RNG`/`RNDR` or x86's `RDRAND`) -- a genuine
 * entropy source on an ARMv7-A core is always a platform-specific TRNG
 * peripheral, out of scope for this header (wire it up with your own
 * `hw_rng_traits` specialization over your platform's MMIO driver,
 * following this file as a layout template). This header provides only
 * the same *fallback* jitter combiner AArch64's `cntpct_rng`/
 * `cntvct_rng` provide (see `structo/arch/arm64/hw_rng.hpp`'s
 * "Fallback" section for the full rationale) -- `cntpct_rng` and
 * `cntvct_rng` are zero-sized tag types bound through
 * `structo::hw::hw_rng_ref` exactly like any other `hw_rng_traits`
 * backend:
 *
 * @code
 * using namespace structo::arch::arm;
 * using namespace structo::hw;
 *
 * cntpct_rng ct{};
 * hw_rng_ref rng(ct);
 * if (rng.is_available()) {
 *   auto word = rng.try_generate64(); // weak -- combine, don't use alone
 * }
 * @endcode
 *
 * **Never use `cntpct_rng`/`cntvct_rng` as your only randomness
 * source**: the counters themselves are fully predictable, ordinary
 * monotonic timers. Each draw reads the counter, spins briefly, reads
 * it again, and mixes both readings plus their delta through
 * `hw::detail::avalanche_mix64` -- the delta captures a small amount of
 * genuine execution-timing jitter (cache misses, bus contention,
 * interrupts), weak but not zero. Wire these into
 * `hw::hw_rng_combinator` (`hw/rng_combinator.hpp`) alongside a real
 * entropy source (platform TRNG) or several other independent weak
 * sources -- combining is what makes the jitter useful. Matches the
 * "fallback ... if asked by the user to do so" framing: nothing in this
 * library reaches for these automatically.
 *
 * ## Availability
 *
 * The Generic Timer is an optional ARMv7-A extension; `is_available()`
 * checks `ID_PFR1.GenTimer` (bits `[19:16]`, nonzero means present)
 * rather than assuming compile-target support.
 */

#include <structo/hw/rng.hpp>

#include <cstdint>

namespace structo::arch::arm {

/**
 * @brief Zero-sized `hw_rng_traits` backend tag for a weak,
 * `CNTPCT`-jitter-based fallback source. See the @file-level docs above
 * -- combine with a real entropy source via `hw::hw_rng_combinator`,
 * never use alone.
 */
struct cntpct_rng {};

/**
 * @brief Zero-sized `hw_rng_traits` backend tag for a weak,
 * `CNTVCT`-jitter-based fallback source. See the @file-level docs above
 * -- combine with a real entropy source via `hw::hw_rng_combinator`,
 * never use alone.
 */
struct cntvct_rng {};

} // namespace structo::arch::arm

namespace structo::hw {

namespace detail {

#if defined(__arm__) && !defined(__aarch64__)
[[nodiscard]] inline bool arm32_generic_timer_available() noexcept {
  std::uint32_t pfr1;
  asm volatile("mrc p15, 0, %0, c0, c1, 1" : "=r"(pfr1));
  return ((pfr1 >> 16) & 0xFu) != 0;
}
#endif

} // namespace detail

template <> struct hw_rng_traits<structo::arch::arm::cntpct_rng> {
  /** @brief Whether `ID_PFR1.GenTimer` reports the Generic Timer extension is present. */
  [[nodiscard]] static bool is_available(structo::arch::arm::cntpct_rng &) noexcept {
#if defined(__arm__) && !defined(__aarch64__)
    return detail::arm32_generic_timer_available();
#else
    return false;
#endif
  }

  /**
   * @brief Reads the 64-bit `CNTPCT` (via `MRRC`) twice around a short
   * spin, mixing both readings and their delta through
   * `avalanche_mix64`. Never fails once compiled in (always returns a
   * value) -- see the @file-level docs for why this must still be
   * combined with a real entropy source before use.
   */
  [[nodiscard]] static result<std::uint64_t> try_generate64(structo::arch::arm::cntpct_rng &) noexcept {
#if defined(__arm__) && !defined(__aarch64__)
    std::uint32_t lo0, hi0, lo1, hi1;
    asm volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo0), "=r"(hi0));
    for (int i = 0; i < 8; ++i)
      asm volatile("" ::: "memory"); // optimization barrier, not a real delay
    asm volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo1), "=r"(hi1));
    std::uint64_t const t0 = (static_cast<std::uint64_t>(hi0) << 32) | lo0;
    std::uint64_t const t1 = (static_cast<std::uint64_t>(hi1) << 32) | lo1;
    return detail::avalanche_mix64(t0 ^ t1 ^ (t1 - t0));
#else
    return unexpected(error::unsupported_operation);
#endif
  }
};

template <> struct hw_rng_traits<structo::arch::arm::cntvct_rng> {
  /** @brief Whether `ID_PFR1.GenTimer` reports the Generic Timer extension is present. */
  [[nodiscard]] static bool is_available(structo::arch::arm::cntvct_rng &) noexcept {
#if defined(__arm__) && !defined(__aarch64__)
    return detail::arm32_generic_timer_available();
#else
    return false;
#endif
  }

  /**
   * @brief Reads the 64-bit `CNTVCT` (via `MRRC`) twice around a short
   * spin, mixing both readings and their delta through
   * `avalanche_mix64`. Never fails once compiled in (always returns a
   * value) -- see the @file-level docs for why this must still be
   * combined with a real entropy source before use.
   */
  [[nodiscard]] static result<std::uint64_t> try_generate64(structo::arch::arm::cntvct_rng &) noexcept {
#if defined(__arm__) && !defined(__aarch64__)
    std::uint32_t lo0, hi0, lo1, hi1;
    asm volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo0), "=r"(hi0));
    for (int i = 0; i < 8; ++i)
      asm volatile("" ::: "memory"); // optimization barrier, not a real delay
    asm volatile("mrrc p15, 1, %0, %1, c14" : "=r"(lo1), "=r"(hi1));
    std::uint64_t const t0 = (static_cast<std::uint64_t>(hi0) << 32) | lo0;
    std::uint64_t const t1 = (static_cast<std::uint64_t>(hi1) << 32) | lo1;
    return detail::avalanche_mix64(t0 ^ t1 ^ (t1 - t0));
#else
    return unexpected(error::unsupported_operation);
#endif
  }
};

} // namespace structo::hw
