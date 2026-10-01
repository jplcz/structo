// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file prng.hpp
 * @brief `structo::prng`: small, fast, fully deterministic pseudo-random
 * generators (`splitmix64`, `xoshiro256ss`, `pcg32`), each directly
 * seedable and each with a `from_hw_rng` factory that draws its initial
 * state from a `structo::hw::hw_rng_ref` (see `hw/rng.hpp`).
 *
 * ## Why these three
 *
 * - `splitmix64` -- a single 64-bit-state generator with excellent
 *   avalanche behavior for *seed expansion*: every other generator in
 *   this file expands a single 64-bit seed into its (wider) internal
 *   state by drawing successive `splitmix64` outputs, the standard
 *   technique recommended by `xoshiro256ss`'s own reference
 *   implementation. Also usable standalone as a minimal, very fast
 *   generator when 64 bits of state is enough.
 * - `xoshiro256ss` (xoshiro256**) -- 256 bits of state, `2^256 - 1`
 *   period, passes every standard empirical randomness test suite
 *   (BigCrush, PractRand); the recommended general-purpose default in
 *   this file for anything that isn't latency- or memory-critical.
 * - `pcg32` (PCG-XSH-RR 64/32) -- 64 bits of state plus a 64-bit stream
 *   selector, 32-bit output; smaller and faster to seed than
 *   `xoshiro256ss`, and its stream parameter gives cheaply-constructed,
 *   statistically-independent *parallel* streams from unrelated seeds
 *   (e.g. one stream per CPU) without needing extra entropy per stream.
 *
 * None of these are cryptographically secure -- do not use them to
 * generate keys, nonces, or anything else a security boundary depends
 * on. They are meant for everything else a kernel/hypervisor needs
 * randomness for: scheduling jitter, ASLR-style slot selection, hash
 * table seeding, retry backoff, test-data generation, and similar.
 *
 * ## Seeding
 *
 * Every generator supports two equally-direct ways to seed it:
 *
 * - A fixed-seed constructor, entirely deterministic and reproducible
 *   (useful for tests and any scenario that explicitly wants
 *   repeatable output).
 * - `from_hw_rng(structo::hw::hw_rng_ref, ...)`, which draws the needed
 *   number of 64-bit words from a bound hardware RNG (see `hw/rng.hpp`)
 *   and returns `reloco::result<Self>`, propagating the first draw
 *   failure (e.g. `error::unsupported_operation` if the ref is unbound,
 *   or `error::try_again` if the hardware's retry budget was
 *   exhausted).
 *
 * @code
 * #include <structo/arch/x86/hw_rng.hpp>
 * #include <structo/prng.hpp>
 *
 * structo::arch::x86::rdrand_rng rd{};
 * structo::hw::hw_rng_ref hw(rd);
 *
 * auto rng = structo::prng::xoshiro256ss::from_hw_rng(hw);
 * if (rng) {
 *   std::uint64_t word = rng->next();
 *   // ...
 * }
 * @endcode
 */

#include <structo/hw/rng.hpp>

#include <array>
#include <cstdint>

namespace structo::prng {

using namespace reloco;

namespace detail {

[[nodiscard]] constexpr std::uint64_t rotl64(std::uint64_t x, int k) noexcept {
  return (x << k) | (x >> (64 - k));
}

[[nodiscard]] constexpr std::uint32_t rotr32(std::uint32_t x, int k) noexcept {
  return (x >> k) | (x << ((32 - k) & 31));
}

} // namespace detail

// ============================================================================
// splitmix64
// ============================================================================

/**
 * @brief A single-64-bit-state generator (Vigna/Steele's `splitmix64`).
 *
 * Minimal and very fast, with excellent avalanche behavior -- the
 * standard way every other generator in this file expands a single
 * 64-bit seed into its own (wider) internal state. Not a
 * high-quality general-purpose generator on its own (its output passes
 * most, but not all, standard empirical test batteries) -- prefer
 * `xoshiro256ss` unless 64 bits of state specifically is what you want.
 */
class splitmix64 {
public:
  /** @brief Constructs a deterministic generator from a fixed 64-bit seed. */
  constexpr explicit splitmix64(std::uint64_t seed) noexcept : state_(seed) {}

  /**
   * @brief Draws one 64-bit seed word from @p hw and constructs a
   * generator from it.
   * @return The constructed generator, or whatever error `hw`'s draw
   * failed with (see `hw_rng_ref::try_generate64`).
   */
  [[nodiscard]] static result<splitmix64> from_hw_rng(const hw::hw_rng_ref &hw,
                                                        std::uint32_t max_retries = hw::hw_rng_ref::default_max_retries) noexcept {
    auto seed = hw.try_generate64(max_retries);
    if (!seed)
      return unexpected(seed.error());
    return splitmix64(seed.value());
  }

  /** @brief Advances the generator and returns its next 64-bit output. */
  [[nodiscard]] constexpr std::uint64_t next() noexcept {
    state_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }

private:
  std::uint64_t state_;
};

// ============================================================================
// xoshiro256ss (xoshiro256**)
// ============================================================================

/**
 * @brief A 256-bit-state generator (Blackman/Vigna's `xoshiro256**`).
 *
 * `2^256 - 1` period, passes every standard empirical randomness test
 * suite (BigCrush, PractRand); the recommended general-purpose default
 * in this file.
 */
class xoshiro256ss {
public:
  /**
   * @brief Constructs a deterministic generator by expanding @p seed
   * into all 4 state words via successive `splitmix64` draws (the
   * technique the reference implementation itself recommends).
   */
  constexpr explicit xoshiro256ss(std::uint64_t seed) noexcept {
    splitmix64 sm(seed);
    for (auto &word : state_)
      word = sm.next();
  }

  /**
   * @brief Draws 4 seed words directly from @p hw and constructs a
   * generator from them (no `splitmix64` expansion -- every word is
   * independently drawn hardware entropy).
   * @return The constructed generator, or whatever error the first
   * failing draw reported.
   */
  [[nodiscard]] static result<xoshiro256ss> from_hw_rng(const hw::hw_rng_ref &hw,
                                                           std::uint32_t max_retries = hw::hw_rng_ref::default_max_retries) noexcept {
    std::array<std::uint64_t, 4> words{};
    for (auto &word : words) {
      auto draw = hw.try_generate64(max_retries);
      if (!draw)
        return unexpected(draw.error());
      word = draw.value();
    }
    return xoshiro256ss(words);
  }

  /** @brief Advances the generator and returns its next 64-bit output. */
  [[nodiscard]] constexpr std::uint64_t next() noexcept {
    std::uint64_t const result = detail::rotl64(state_[1] * 5, 7) * 9;
    std::uint64_t const t = state_[1] << 17;

    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = detail::rotl64(state_[3], 45);

    return result;
  }

private:
  /** @brief Constructs directly from 4 already-random state words (used by `from_hw_rng`). */
  constexpr explicit xoshiro256ss(const std::array<std::uint64_t, 4> &state) noexcept : state_(state) {}

  std::array<std::uint64_t, 4> state_;
};

// ============================================================================
// pcg32 (PCG-XSH-RR 64/32)
// ============================================================================

/**
 * @brief A 64-bit-state, 32-bit-output generator (O'Neill's PCG family,
 * the `pcg32`/PCG-XSH-RR variant).
 *
 * Smaller and faster to seed than `xoshiro256ss`; its 64-bit stream
 * selector (@p sequence) gives cheaply-constructed, statistically
 * independent *parallel* streams from unrelated seeds (e.g. one stream
 * per CPU) without needing extra entropy per stream -- two generators
 * built from the same @p seed but different @p sequence values produce
 * uncorrelated output.
 */
class pcg32 {
public:
  /**
   * @brief Constructs a deterministic generator from a fixed 64-bit
   * seed and stream selector, following the reference PCG seeding
   * procedure.
   * @param seed Initial state.
   * @param sequence Stream selector; only its value modulo `2^63`
   * matters (the low bit is forced to make the increment odd, as the
   * algorithm requires). Different @p sequence values with the same
   * @p seed yield statistically independent streams.
   */
  constexpr explicit pcg32(std::uint64_t seed, std::uint64_t sequence = 1) noexcept
      : state_(0), inc_((sequence << 1) | 1u) {
    (void)next();
    state_ += seed;
    (void)next();
  }

  /**
   * @brief Draws 2 words from @p hw (one initial state, one stream
   * selector) and constructs a generator from them.
   * @return The constructed generator, or whatever error the first
   * failing draw reported.
   */
  [[nodiscard]] static result<pcg32> from_hw_rng(const hw::hw_rng_ref &hw,
                                                   std::uint32_t max_retries = hw::hw_rng_ref::default_max_retries) noexcept {
    auto seed = hw.try_generate64(max_retries);
    if (!seed)
      return unexpected(seed.error());
    auto sequence = hw.try_generate64(max_retries);
    if (!sequence)
      return unexpected(sequence.error());
    return pcg32(seed.value(), sequence.value());
  }

  /** @brief Advances the generator and returns its next 32-bit output. */
  [[nodiscard]] constexpr std::uint32_t next() noexcept {
    std::uint64_t const old_state = state_;
    state_ = old_state * 6364136223846793005ull + inc_;
    std::uint32_t const xorshifted = static_cast<std::uint32_t>(((old_state >> 18) ^ old_state) >> 27);
    int const rot = static_cast<int>(old_state >> 59);
    return detail::rotr32(xorshifted, rot);
  }

private:
  std::uint64_t state_;
  std::uint64_t inc_;
};

} // namespace structo::prng
