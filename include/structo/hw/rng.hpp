// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file rng.hpp
 * @brief `structo::hw::hw_rng_ref`: a type-erased, non-owning handle over
 * a hardware random/entropy source (`RDRAND`/`RDSEED`, `RNDR`/`RNDRRS`,
 * the RISC-V Zkr `seed` CSR, ...), plus the `hw_rng_traits<Backend>`
 * customization point a concrete backend specializes to be bindable
 * through it.
 *
 * This header is deliberately abstraction-only, the hardware-entropy
 * counterpart of `uart_ref.hpp`: there is no `RDRAND`/`RNDR` instruction
 * encoding here, nothing arch-specific. A concrete backend (one of the
 * per-architecture tag types in `arch/x86/hw_rng.hpp`,
 * `arch/arm64/hw_rng.hpp`, `arch/riscv/hw_rng.hpp`, or a unit test's
 * deterministic fake) implements `hw_rng_traits<Backend>`, and
 * `structo/prng.hpp`'s pseudo-random generators seed themselves from
 * whatever concrete backend a `hw_rng_ref` is bound to.
 *
 * ## Why this exists separately from `structo::prng`
 *
 * A real hardware RNG instruction only ever answers "here are a few raw
 * entropy bits, drawn directly from a physical noise source or a
 * continuously-reseeded DRBG" -- small, slow (by CPU standards), and
 * sometimes transiently unavailable (an empty entropy pool, a busy
 * conditioning engine). It is deliberately *not* meant to be drawn from
 * in a tight loop for every random byte a program needs. `structo::prng`
 * (`prng.hpp`) instead provides fast, portable pseudo-random generators
 * (no real entropy, fully deterministic given their seed) *seeded once*
 * from a `hw_rng_ref` at startup -- the general pattern every
 * off-the-shelf CSPRNG/PRNG library (seed the fast generator from a
 * platform entropy source once, then draw from the fast generator) uses,
 * kept here as two separate, independently testable layers instead of
 * one monolithic "random number" header.
 *
 * ## Customization point: `hw_rng_traits<Backend>`
 *
 * `hw_rng_traits<Backend>` is left undefined for any `Backend` that
 * hasn't opted in (mirroring `uart_traits`/`io_space_traits`). A
 * specialization must supply exactly one function:
 *
 * @code
 * template <> struct structo::hw::hw_rng_traits<my_backend> {
 *   static reloco::result<std::uint64_t> try_generate64(my_backend &) noexcept;
 * };
 * @endcode
 *
 * `try_generate64` draws one 64-bit word of hardware randomness. It
 * fails with `error::try_again` if the hardware reports a transient
 * "not ready yet" condition (e.g. `RDRAND`/`RDSEED`'s carry flag clear,
 * `RNDR`/`RNDRRS`'s `PSTATE.Z` set, the Zkr `seed` CSR's `OPST` field
 * reporting `WAIT`/`BIST`) -- `hw_rng_ref` retries on exactly this error,
 * up to a caller-chosen bound, never on any other error (which is
 * assumed non-transient, e.g. the Zkr `seed` CSR's `OPST_DEAD`, or
 * `error::unsupported_operation` from a backend compiled out on the
 * wrong architecture).
 *
 * Optionally, a backend may also supply `is_available`, reporting
 * whether the underlying instruction/CSR is actually present on the
 * running core (e.g. an x86 `CPUID` feature-bit check, an AArch64
 * `ID_AA64ISAR0_EL1.RNDR` check) rather than merely compiled in for the
 * target architecture:
 *
 * @code
 * static bool is_available(my_backend &) noexcept;
 * @endcode
 *
 * Detected via SFINAE (the same optional-member idiom
 * `uart_traits::current_config` uses); if absent, `hw_rng_ref::is_available()`
 * reports `true` whenever bound (the backend is assumed always present).
 */

#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace structo {

using namespace reloco;

namespace hw {

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Opt-in customization point describing how to draw one 64-bit
 * word of hardware randomness from a concrete backend, through
 * @ref hw_rng_ref.
 *
 * Intentionally left undefined for any `Backend` that hasn't been
 * adapted, mirroring `uart_traits`/`io_space_traits`. See the @file-level
 * docs above for the complete required/optional member list.
 */
template <typename Backend> struct hw_rng_traits;

namespace detail {

/**
 * @brief Shared 64-bit avalanche-mixing finalizer (the `splitmix64`
 * finalization step), reused by any backend/combinator in this library
 * that needs to scramble a weak or structured 64-bit value (e.g. a raw
 * timer counter, or several XOR-combined draws) into one that looks
 * uniformly random bit-for-bit. This is *mixing*, not entropy
 * generation: it cannot add entropy that wasn't already present in its
 * input, it only spreads whatever entropy is present across every
 * output bit and removes linear structure.
 */
[[nodiscard]] constexpr std::uint64_t avalanche_mix64(std::uint64_t z) noexcept {
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

template <typename Backend, typename = void> struct has_hw_rng_traits : std::false_type {};

template <typename Backend>
struct has_hw_rng_traits<Backend, std::void_t<decltype(hw_rng_traits<Backend>::try_generate64)>> : std::true_type {};

// Detects the optional Traits::is_available probe.
template <typename Traits, typename = void> struct hw_rng_has_is_available : std::false_type {};
template <typename Traits>
struct hw_rng_has_is_available<Traits, std::void_t<decltype(Traits::is_available)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Type-Erased Hardware RNG Handle
// ============================================================================

/**
 * @brief Type-erased, non-owning handle over a hardware random/entropy
 * source, for whatever concrete backend it is bound to.
 *
 * Default-constructed (or copied from a default-constructed) refs are
 * *unbound*: every operation on one fails with
 * `error::unsupported_operation` rather than trapping, mirroring
 * `io_space_ref`/`uart_ref`'s null-safety convention.
 *
 * Follows the single-`vtable`, resolved-once-per-`Backend` shape every
 * `structo` `*_ref` handle uses -- see
 * [`type-erased-base-containers.md`](https://github.com/jplcz/reloco/blob/master/docs/type-erased-base-containers.md)
 * and `docs/coding-guide.md`'s "Type-erase a `*_ref` handle's backend
 * behind one `vtable`" section.
 */
class RELOCO_POINTER hw_rng_ref {
public:
  /** @brief Retry bound used by the default-argument overloads of `try_generate64`/`try_generate32`/`try_fill`. */
  static constexpr std::uint32_t default_max_retries = 16;

  /** @brief Fixed, per-bound-backend-type dispatch table. */
  struct vtable {
    result<std::uint64_t> (*generate64)(void *ctx) noexcept;
    bool (*is_available)(void *ctx) noexcept;
  };

  /** @brief Constructs an unbound ref. */
  constexpr hw_rng_ref() noexcept = default;

  /**
   * @brief Binds this ref to an existing, adapted backend.
   * @tparam Backend Concrete backend type, deduced. Must have a
   * @ref hw_rng_traits specialization.
   * @param b Backend to bind. Must outlive this handle and every copy of
   * it. Marked `explicit`: binding a backend is always a deliberate step,
   * never an implicit conversion.
   */
  template <typename Backend, std::enable_if_t<detail::has_hw_rng_traits<Backend>::value, int> = 0>
  constexpr explicit hw_rng_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  /** @brief Rejects rvalue/temporary backend bindings. */
  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  hw_rng_ref(Backend &&) = delete;

  /** @brief Whether this ref is bound to a backend. */
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /**
   * @brief Whether the bound backend's hardware RNG is actually usable
   * right now. `false` if unbound; if the bound backend does not
   * implement the optional `hw_rng_traits::is_available`, assumed `true`
   * whenever bound.
   */
  [[nodiscard]] bool is_available() const noexcept {
    if (!vtbl_)
      return false;
    return vtbl_->is_available(ctx_);
  }

  // --------------------------------------------------------------------
  // Mandatory backend operation (directly forwarded), plus the retry
  // loop every backend's transient "not ready yet" condition needs.
  // --------------------------------------------------------------------

  /**
   * @brief Draws one 64-bit word of hardware randomness, retrying up to
   * `max_retries` times while the backend reports `error::try_again`
   * (a transient "not ready yet" condition).
   * Fails with `error::unsupported_operation` if this ref is unbound,
   * `error::try_again` if `max_retries` is exhausted, or whatever other
   * error the backend itself reports (assumed non-transient).
   */
  [[nodiscard]] result<std::uint64_t> try_generate64(std::uint32_t max_retries = default_max_retries) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    for (std::uint32_t i = 0; i < max_retries; ++i) {
      auto word = vtbl_->generate64(ctx_);
      if (word || word.error() != error::try_again)
        return word;
    }
    return unexpected(error::try_again);
  }

  // --------------------------------------------------------------------
  // Generic conveniences, synthesized purely from `try_generate64`
  // above -- no further backend support is required for either of
  // these.
  // --------------------------------------------------------------------

  /**
   * @brief Draws one 32-bit word of hardware randomness (the low half
   * of one `try_generate64` draw).
   */
  [[nodiscard]] result<std::uint32_t> try_generate32(std::uint32_t max_retries = default_max_retries) const noexcept {
    auto word = try_generate64(max_retries);
    if (!word)
      return unexpected(word.error());
    return static_cast<std::uint32_t>(word.value());
  }

  /**
   * @brief Fills every byte of `dst` with hardware randomness, drawing
   * one `try_generate64` word (retrying up to `max_retries_per_word`
   * times each) per 8 bytes (or part thereof) of `dst`.
   * Stops and fails (propagating `try_generate64`'s error) at the first
   * word that cannot be drawn; `dst` is left partially filled in that
   * case.
   */
  [[nodiscard]] result<void> try_fill(span<std::byte> dst,
                                      std::uint32_t max_retries_per_word = default_max_retries) const noexcept {
    std::size_t filled = 0;
    while (filled < dst.size()) {
      auto word = try_generate64(max_retries_per_word);
      if (!word)
        return unexpected(word.error());
      std::uint64_t raw = word.value();
      std::size_t chunk = dst.size() - filled < sizeof(raw) ? dst.size() - filled : sizeof(raw);
      // chunk <= dst.size() - filled, and sizeof(raw) bounds the source.
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
      std::memcpy(dst.data() + filled, &raw, chunk);
      RELOCO_END_UNSAFE_BUFFER_USAGE
      filled += chunk;
    }
    return {};
  }

private:
  template <typename Backend> static result<std::uint64_t> generate64_entry(void *ctx) noexcept {
    return hw_rng_traits<Backend>::try_generate64(*static_cast<Backend *>(ctx));
  }

  template <typename Backend> static bool is_available_entry(void *ctx) noexcept {
    using traits = hw_rng_traits<Backend>;
    if constexpr (detail::hw_rng_has_is_available<traits>::value) {
      return traits::is_available(*static_cast<Backend *>(ctx));
    } else {
      (void)ctx;
      return true;
    }
  }

  template <typename Backend> static constexpr vtable s_vtbl{&generate64_entry<Backend>, &is_available_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace hw
} // namespace structo
