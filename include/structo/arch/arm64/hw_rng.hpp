// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hw_rng.hpp
 * @brief `hw_rng_traits` backends for AArch64's `FEAT_RNG` hardware
 * random-number registers: `rndr_rng` (`RNDR`) and `rndrrs_rng`
 * (`RNDRRS`).
 *
 * Both are zero-sized tag types -- the registers carry no state of
 * their own -- bound through `structo::hw::hw_rng_ref` exactly like any
 * other `hw_rng_traits` backend:
 *
 * @code
 * using namespace structo::arch::arm64;
 * using namespace structo::hw;
 *
 * rndr_rng rndr{};
 * hw_rng_ref rng(rndr);
 * if (rng.is_available()) {
 *   auto word = rng.try_generate64();
 *   // ...
 * }
 * @endcode
 *
 * ## `RNDR` vs. `RNDRRS`
 *
 * `RNDR` draws from a continuously-reseeded DRBG -- fast, suitable for
 * drawing many words (e.g. seeding `structo::prng`'s generators).
 * `RNDRRS` forces an immediate reseed from the underlying entropy
 * source before drawing -- slower, intended specifically for seeding a
 * DRBG of your own rather than for bulk use. Prefer `rndr_rng` unless
 * there is a specific reason to want `RNDRRS`'s stronger reseed
 * guarantee. Both registers are encoded as `MRS` reads to read-only
 * system registers (`S3_3_C2_C4_0` for `RNDR`, `S3_3_C2_C4_1` for
 * `RNDRRS`), emitted here via the raw `Sop0_op1_CRn_CRm_op2` encoding so
 * this header doesn't require an assembler recent enough to recognize
 * the `rndr`/`rndrrs` mnemonics.
 *
 * ## Availability
 *
 * `FEAT_RNG` is an optional AArch64 extension; `is_available()` reads
 * `ID_AA64ISAR0_EL1.RNDR` (bits `[63:60]`, a nonzero value means
 * present) rather than assuming compile-target support. That read is
 * itself unprivileged at EL0 under the standard `ID_*` trap
 * configuration used by every mainstream AArch64 OS, matching the
 * access pattern `structo::arch::arm64::mmu_regs.hpp`'s own
 * documentation describes for other `ID_*` feature registers.
 *
 * ## Retry semantics
 *
 * Both registers report success via `PSTATE.Z`: `Z == 0` (i.e. the
 * condition `ne`) means a valid value was returned, `Z == 1` means the
 * draw failed (transient -- the entropy source or DRBG wasn't ready).
 * `try_generate64` reports a failed draw as `error::try_again`, which
 * `hw_rng_ref::try_generate64` retries up to its caller-chosen bound,
 * matching the Arm ARM's own guidance to retry a bounded number of
 * times on failure.
 *
 * ## Fallback: `CNTPCT_EL0`/`CNTVCT_EL0` jitter combiners
 *
 * `cntpct_rng` and `cntvct_rng` are **not** hardware entropy sources --
 * the Generic Timer's physical (`CNTPCT_EL0`) and virtual
 * (`CNTVCT_EL0`) counters are ordinary monotonic counters, fully
 * predictable to any observer who knows roughly what time it is. They
 * exist purely as a *last-resort fallback* for cores without
 * `FEAT_RNG` (no `RNDR`/`RNDRRS`) and no other hardware entropy source
 * available at all: each draw reads the counter, spins briefly, reads
 * it again, and mixes the two readings and their delta through
 * `hw::detail::avalanche_mix64`. The delta captures a small amount of
 * genuine execution-timing jitter (cache misses, bus contention,
 * interrupts landing mid-loop) -- weak, but not zero -- while the
 * raw counter values contribute no real entropy at all on their own.
 *
 * **Never use `cntpct_rng`/`cntvct_rng` as your only randomness
 * source.** They are intended to be wired into
 * `hw::hw_rng_combinator` (`hw/rng_combinator.hpp`) alongside at least
 * one real hardware source (or, failing that, several independent weak
 * sources from unrelated subsystems) -- combining is what makes the
 * small amount of jitter they do carry useful, matching the "if asked
 * by the user to do so" opt-in framing: nothing in this library reaches
 * for them automatically.
 */

#include <structo/hw/rng.hpp>

#include <cstdint>

namespace structo::arch::arm64 {

/** @brief Zero-sized `hw_rng_traits` backend tag for the `RNDR` system register. */
struct rndr_rng {};

/** @brief Zero-sized `hw_rng_traits` backend tag for the `RNDRRS` system register. */
struct rndrrs_rng {};

/**
 * @brief Zero-sized `hw_rng_traits` backend tag for a weak,
 * `CNTPCT_EL0`-jitter-based fallback source. See the @file-level
 * "Fallback" section above -- combine with a real entropy source via
 * `hw::hw_rng_combinator`, never use alone.
 */
struct cntpct_rng {};

/**
 * @brief Zero-sized `hw_rng_traits` backend tag for a weak,
 * `CNTVCT_EL0`-jitter-based fallback source. See the @file-level
 * "Fallback" section above -- combine with a real entropy source via
 * `hw::hw_rng_combinator`, never use alone.
 */
struct cntvct_rng {};

} // namespace structo::arch::arm64

namespace structo::hw {

template <> struct hw_rng_traits<structo::arch::arm64::rndr_rng> {
  /** @brief Whether `ID_AA64ISAR0_EL1.RNDR` reports `FEAT_RNG` support on this core. */
  [[nodiscard]] static bool is_available(structo::arch::arm64::rndr_rng &) noexcept {
#if defined(__aarch64__)
    std::uint64_t isar0;
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    return ((isar0 >> 60) & 0xFu) != 0;
#else
    return false;
#endif
  }

  /**
   * @brief Executes `RNDR` once. Reports `error::try_again` if
   * `PSTATE.Z` was set (no value produced this attempt -- transient,
   * safe to retry), compiled only for a genuine AArch64 target.
   */
  [[nodiscard]] static result<std::uint64_t> try_generate64(structo::arch::arm64::rndr_rng &) noexcept {
#if defined(__aarch64__)
    std::uint64_t value;
    std::uint32_t ok;
    asm volatile("mrs %0, s3_3_c2_c4_0\n\t"
                 "cset %w1, ne"
                 : "=r"(value), "=r"(ok)::"cc");
    if (!ok)
      return unexpected(error::try_again);
    return value;
#else
    return unexpected(error::unsupported_operation);
#endif
  }
};

template <> struct hw_rng_traits<structo::arch::arm64::rndrrs_rng> {
  /** @brief Whether `ID_AA64ISAR0_EL1.RNDR` reports `FEAT_RNG` support on this core. */
  [[nodiscard]] static bool is_available(structo::arch::arm64::rndrrs_rng &) noexcept {
#if defined(__aarch64__)
    std::uint64_t isar0;
    asm volatile("mrs %0, id_aa64isar0_el1" : "=r"(isar0));
    return ((isar0 >> 60) & 0xFu) != 0;
#else
    return false;
#endif
  }

  /**
   * @brief Executes `RNDRRS` once. Reports `error::try_again` if
   * `PSTATE.Z` was set (no value produced this attempt -- transient,
   * safe to retry), compiled only for a genuine AArch64 target.
   */
  [[nodiscard]] static result<std::uint64_t> try_generate64(structo::arch::arm64::rndrrs_rng &) noexcept {
#if defined(__aarch64__)
    std::uint64_t value;
    std::uint32_t ok;
    asm volatile("mrs %0, s3_3_c2_c4_1\n\t"
                 "cset %w1, ne"
                 : "=r"(value), "=r"(ok)::"cc");
    if (!ok)
      return unexpected(error::try_again);
    return value;
#else
    return unexpected(error::unsupported_operation);
#endif
  }
};

template <> struct hw_rng_traits<structo::arch::arm64::cntpct_rng> {
  /** @brief The Generic Timer is a mandatory AArch64 baseline feature; always `true`. */
  [[nodiscard]] static bool is_available(structo::arch::arm64::cntpct_rng &) noexcept { return true; }

  /**
   * @brief Reads `CNTPCT_EL0` twice around a short spin, mixing both
   * readings and their delta through `avalanche_mix64`. Never fails
   * once compiled in (always returns a value) -- see the @file-level
   * "Fallback" section for why this must still be combined with a real
   * entropy source before use.
   */
  [[nodiscard]] static result<std::uint64_t> try_generate64(structo::arch::arm64::cntpct_rng &) noexcept {
#if defined(__aarch64__)
    std::uint64_t t0, t1;
    asm volatile("isb\n\t"
                 "mrs %0, cntpct_el0"
                 : "=r"(t0));
    // Spin briefly so the delta below captures execution-timing jitter
    // (cache misses, bus contention, interrupts) rather than a fixed,
    // fully predictable instruction count.
    for (int i = 0; i < 8; ++i)
      asm volatile("" ::: "memory"); // optimization barrier, not a real delay
    asm volatile("isb\n\t"
                 "mrs %0, cntpct_el0"
                 : "=r"(t1));
    return detail::avalanche_mix64(t0 ^ t1 ^ (t1 - t0));
#else
    return unexpected(error::unsupported_operation);
#endif
  }
};

template <> struct hw_rng_traits<structo::arch::arm64::cntvct_rng> {
  /** @brief The Generic Timer is a mandatory AArch64 baseline feature; always `true`. */
  [[nodiscard]] static bool is_available(structo::arch::arm64::cntvct_rng &) noexcept { return true; }

  /**
   * @brief Reads `CNTVCT_EL0` twice around a short spin, mixing both
   * readings and their delta through `avalanche_mix64`. Never fails
   * once compiled in (always returns a value) -- see the @file-level
   * "Fallback" section for why this must still be combined with a real
   * entropy source before use.
   */
  [[nodiscard]] static result<std::uint64_t> try_generate64(structo::arch::arm64::cntvct_rng &) noexcept {
#if defined(__aarch64__)
    std::uint64_t t0, t1;
    asm volatile("isb\n\t"
                 "mrs %0, cntvct_el0"
                 : "=r"(t0));
    for (int i = 0; i < 8; ++i)
      asm volatile("" ::: "memory"); // optimization barrier, not a real delay
    asm volatile("isb\n\t"
                 "mrs %0, cntvct_el0"
                 : "=r"(t1));
    return detail::avalanche_mix64(t0 ^ t1 ^ (t1 - t0));
#else
    return unexpected(error::unsupported_operation);
#endif
  }
};

} // namespace structo::hw
