// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fixed_bitmap.hpp
 * @brief `structo::fixed_bitmap<N>`: a fixed-capacity, compile-time-sized
 * bitmap over `N` bits, backed by an in-object `unsigned long[]` array
 * -- no allocator, no `reloco::vector`, safe to use in interrupt/trap
 * context or before an allocator even exists.
 *
 * This is the generic counterpart to `arch::cpu_mask<Tag, MaxCpus>`
 * (see `arch/cpu_mask.hpp`): same "fixed array embedded directly in the
 * object" storage strategy, same tri-tier (checked/`try_`/`unsafe_`)
 * per-bit accessor convention, but untagged (there is no phantom `Tag`
 * template parameter -- `fixed_bitmap<N>` is meant for generic "a fixed
 * pool of N slots" bookkeeping, not CPU-index-shaped domains) and with
 * two scan primitives `cpu_mask` doesn't need: `lowest_clear_from()`/
 * `lowest_clear()` and `find_and_set_from()`/`find_and_set()` -- the
 * "find a free slot in a fixed pool" primitives this type exists for
 * (see e.g. `arch::asid_allocator`'s hand-rolled cursor-scan, which this
 * type is meant to eventually replace/generalize).
 *
 * All operations are implemented once in `bitmap_utils` (span-based) and
 * once more in `bitmap_ops<Derived>` (the CRTP instance-method wrapper);
 * `fixed_bitmap<N>` itself only supplies the backing storage and the
 * `words()`/`nbits()` primitives `bitmap_ops` needs.
 *
 * @code
 * structo::fixed_bitmap<128> slots; // all clear
 * auto slot = slots.find_and_set(); // claim the lowest free slot
 * if (slot) {
 *   use(*slot);
 *   slots.clear(*slot);
 * }
 * @endcode
 */

#include "bitmap_ops.hpp"
#include "bitmap_utils.hpp"

#include <cstddef>
#include <reloco/array.hpp>
#include <reloco/span.hpp>

namespace structo {

using namespace reloco;

/**
 * @brief Fixed-capacity bitmap of exactly `N` bits, stored inline (no
 * allocation). Trivially copyable/movable, like `arch::cpu_mask`.
 */
template <std::size_t N> class fixed_bitmap : public bitmap_ops<fixed_bitmap<N>> {
public:
  static constexpr std::size_t bit_count = N;
  static constexpr std::size_t word_count = bitmap_utils::word_count_for(N);

  /** @brief Constructs an all-clear bitmap. */
  constexpr fixed_bitmap() noexcept = default;

  /** @brief An all-clear bitmap (same as the default constructor; spelled out for symmetry with `filled()`). */
  [[nodiscard]] static constexpr fixed_bitmap empty() noexcept { return fixed_bitmap{}; }

  /** @brief A bitmap with every one of its `N` bits set. */
  [[nodiscard]] static constexpr fixed_bitmap filled() noexcept {
    fixed_bitmap bm;
    for (std::size_t i = 0; i < word_count; ++i) {
      bm.words_[i] = ~0UL;
    }
    bitmap_utils::mask_tail_padding(span<unsigned long>(bm.words_.data(), word_count), N);
    return bm;
  }

  // ---- `bitmap_ops<Derived>` contract ----

  [[nodiscard]] span<unsigned long> words() & noexcept { return span<unsigned long>(words_.data(), word_count); }
  [[nodiscard]] span<const unsigned long> words() const & noexcept {
    return span<const unsigned long>(words_.data(), word_count);
  }
  [[nodiscard]] static constexpr std::size_t nbits() noexcept { return N; }

  // ---------------------------------------------------------------------------
  // Rust `bitflags`-flavored set algebra that returns a *new* `fixed_bitmap`
  // rather than mutating `*this`. These live here -- not in `bitmap_ops`
  // -- because constructing a fresh value requires `Derived` to be
  // freely copyable, which only `fixed_bitmap<N>` guarantees among the
  // three `bitmap_ops` users (`dynamic_bitmap` needs an allocator;
  // `bitmap_view` is deliberately non-copyable). For in-place mutation
  // use the inherited `operator|=`/`&=`/`^=`/`-=`/`invert()` instead.
  // ---------------------------------------------------------------------------

  /** @brief Returns a copy with every bit either `*this` or `other` has set (set union). */
  template <typename OtherBitmap> [[nodiscard]] fixed_bitmap union_with(const OtherBitmap &other) const noexcept {
    fixed_bitmap result = *this;
    result |= other;
    return result;
  }

  /** @brief Returns a copy with only the bits both `*this` and `other` have set (set intersection). */
  template <typename OtherBitmap> [[nodiscard]] fixed_bitmap intersection(const OtherBitmap &other) const noexcept {
    fixed_bitmap result = *this;
    result &= other;
    return result;
  }

  /** @brief Returns a copy with every bit `*this` has set that `other` does not (set difference). */
  template <typename OtherBitmap> [[nodiscard]] fixed_bitmap difference(const OtherBitmap &other) const noexcept {
    fixed_bitmap result = *this;
    result -= other;
    return result;
  }

  /** @brief Returns a copy with every bit set in exactly one of `*this`/`other` (symmetric difference). */
  template <typename OtherBitmap>
  [[nodiscard]] fixed_bitmap symmetric_difference(const OtherBitmap &other) const noexcept {
    fixed_bitmap result = *this;
    result ^= other;
    return result;
  }

  /** @brief Returns a copy with every bit flipped (set complement). */
  [[nodiscard]] fixed_bitmap complement() const noexcept {
    fixed_bitmap result = *this;
    result.invert();
    return result;
  }

  [[nodiscard]] friend fixed_bitmap operator|(const fixed_bitmap &a, const fixed_bitmap &b) noexcept {
    return a.union_with(b);
  }
  [[nodiscard]] friend fixed_bitmap operator&(const fixed_bitmap &a, const fixed_bitmap &b) noexcept {
    return a.intersection(b);
  }
  [[nodiscard]] friend fixed_bitmap operator^(const fixed_bitmap &a, const fixed_bitmap &b) noexcept {
    return a.symmetric_difference(b);
  }
  [[nodiscard]] friend fixed_bitmap operator-(const fixed_bitmap &a, const fixed_bitmap &b) noexcept {
    return a.difference(b);
  }
  [[nodiscard]] fixed_bitmap operator~() const noexcept { return complement(); }

  [[nodiscard]] friend bool operator==(const fixed_bitmap &a, const fixed_bitmap &b) noexcept {
    return bitmap_utils::equals(a.words(), b.words());
  }
  [[nodiscard]] friend bool operator!=(const fixed_bitmap &a, const fixed_bitmap &b) noexcept { return !(a == b); }

private:
  reloco::array<unsigned long, (word_count == 0 ? 1 : word_count)> words_{};
};

} // namespace structo
