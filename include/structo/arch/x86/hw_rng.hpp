// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hw_rng.hpp
 * @brief `hw_rng_traits` backends for x86/x86-64's hardware random-number
 * instructions: `rdrand_rng` (`RDRAND`) and `rdseed_rng` (`RDSEED`).
 *
 * Both are zero-sized tag types -- the instructions themselves carry no
 * state, so there is nothing to wrap -- bound through
 * `structo::hw::hw_rng_ref` exactly like any other `hw_rng_traits`
 * backend:
 *
 * @code
 * using namespace structo::arch::x86;
 * using namespace structo::hw;
 *
 * rdseed_rng seed{};
 * hw_rng_ref rng(seed);
 * if (rng.is_available()) {
 *   auto word = rng.try_generate64();
 *   // ...
 * }
 * @endcode
 *
 * ## `RDRAND` vs. `RDSEED`
 *
 * `RDRAND` draws from an AES-CTR-DRBG continuously reseeded from the
 * processor's physical entropy source -- fast, suitable for drawing many
 * words (e.g. seeding `structo::prng`'s generators). `RDSEED` instead
 * draws closer to the raw entropy source itself (via a conditioning
 * step) -- slower and more prone to transient exhaustion, intended
 * specifically for seeding a DRBG of your own rather than for bulk use.
 * Prefer `rdrand_rng` unless there is a specific reason to want
 * `RDSEED`'s stronger entropy guarantee.
 *
 * ## Availability
 *
 * Neither instruction is guaranteed present on every x86/x86-64 core;
 * `is_available()` checks the documented `CPUID` feature bit at runtime
 * (`CPUID.01H:ECX.RDRAND[bit 30]` for `RDRAND`,
 * `CPUID.(EAX=07H,ECX=0H):EBX.RDSEED[bit 18]` for `RDSEED`) rather than
 * assuming compile-target support, since `-mrdrnd`/`-mrdseed` are not
 * required to emit the raw instruction mnemonics used here.
 *
 * ## Retry semantics
 *
 * Both instructions set the carry flag (`CF`) to `1` if they produced a
 * valid value, `0` otherwise (a transient condition -- the entropy
 * conditioner hasn't produced a fresh value yet); `try_generate64`
 * reports that as `error::try_again`, which `hw_rng_ref::try_generate64`
 * retries up to its caller-chosen bound. Intel's guidance is to retry a
 * bounded number of times (the processor guarantees forward progress,
 * but not a bound on any single attempt), exactly the shape
 * `hw_rng_ref` already provides.
 */

#include <structo/hw/rng.hpp>

#include <cstdint>

#if defined(__i386__) || defined(__x86_64__)
#include <cstddef>
#endif

namespace structo::arch::x86 {

/** @brief Zero-sized `hw_rng_traits` backend tag for the `RDRAND` instruction. */
struct rdrand_rng {};

/** @brief Zero-sized `hw_rng_traits` backend tag for the `RDSEED` instruction. */
struct rdseed_rng {};

} // namespace structo::arch::x86

namespace structo::hw {

template <> struct hw_rng_traits<structo::arch::x86::rdrand_rng> {
  /** @brief Whether `CPUID.01H:ECX.RDRAND[bit 30]` reports `RDRAND` support on this core. */
  [[nodiscard]] static bool is_available(structo::arch::x86::rdrand_rng &) noexcept {
#if defined(__i386__) || defined(__x86_64__)
    std::uint32_t eax, ebx, ecx, edx;
#if defined(__x86_64__)
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
#else
    // %ebx is PIC's GOT base register on 32-bit x86; save/restore it around CPUID by hand.
    asm volatile("xchgl %%ebx, %1\n\t"
                 "cpuid\n\t"
                 "xchgl %%ebx, %1\n\t"
                 : "=a"(eax), "=r"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(1), "c"(0));
#endif
    return (ecx & (1u << 30)) != 0;
#else
    return false;
#endif
  }

  /**
   * @brief Executes `RDRAND` once. Reports `error::try_again` if the
   * carry flag was clear (no value produced this attempt -- transient,
   * safe to retry), compiled only for a genuine x86/x86-64 target.
   */
  [[nodiscard]] static result<std::uint64_t> try_generate64(structo::arch::x86::rdrand_rng &) noexcept {
#if defined(__x86_64__)
    std::uint64_t value;
    std::uint8_t ok;
    asm volatile("rdrand %0\n\t"
                 "setc %1"
                 : "=r"(value), "=qm"(ok));
    if (!ok)
      return unexpected(error::try_again);
    return value;
#elif defined(__i386__)
    std::uint32_t lo, hi;
    std::uint8_t ok_lo, ok_hi;
    asm volatile("rdrand %0\n\t"
                 "setc %1"
                 : "=r"(lo), "=qm"(ok_lo));
    if (!ok_lo)
      return unexpected(error::try_again);
    asm volatile("rdrand %0\n\t"
                 "setc %1"
                 : "=r"(hi), "=qm"(ok_hi));
    if (!ok_hi)
      return unexpected(error::try_again);
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#else
    return unexpected(error::unsupported_operation);
#endif
  }
};

template <> struct hw_rng_traits<structo::arch::x86::rdseed_rng> {
  /** @brief Whether `CPUID.(EAX=07H,ECX=0H):EBX.RDSEED[bit 18]` reports `RDSEED` support on this core. */
  [[nodiscard]] static bool is_available(structo::arch::x86::rdseed_rng &) noexcept {
#if defined(__i386__) || defined(__x86_64__)
    std::uint32_t eax, ebx, ecx, edx;
#if defined(__x86_64__)
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
#else
    asm volatile("xchgl %%ebx, %1\n\t"
                 "cpuid\n\t"
                 "xchgl %%ebx, %1\n\t"
                 : "=a"(eax), "=r"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(7), "c"(0));
#endif
    return (ebx & (1u << 18)) != 0;
#else
    return false;
#endif
  }

  /**
   * @brief Executes `RDSEED` once. Reports `error::try_again` if the
   * carry flag was clear (no value produced this attempt -- transient,
   * safe to retry), compiled only for a genuine x86/x86-64 target.
   */
  [[nodiscard]] static result<std::uint64_t> try_generate64(structo::arch::x86::rdseed_rng &) noexcept {
#if defined(__x86_64__)
    std::uint64_t value;
    std::uint8_t ok;
    asm volatile("rdseed %0\n\t"
                 "setc %1"
                 : "=r"(value), "=qm"(ok));
    if (!ok)
      return unexpected(error::try_again);
    return value;
#elif defined(__i386__)
    std::uint32_t lo, hi;
    std::uint8_t ok_lo, ok_hi;
    asm volatile("rdseed %0\n\t"
                 "setc %1"
                 : "=r"(lo), "=qm"(ok_lo));
    if (!ok_lo)
      return unexpected(error::try_again);
    asm volatile("rdseed %0\n\t"
                 "setc %1"
                 : "=r"(hi), "=qm"(ok_hi));
    if (!ok_hi)
      return unexpected(error::try_again);
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
#else
    return unexpected(error::unsupported_operation);
#endif
  }
};

} // namespace structo::hw
