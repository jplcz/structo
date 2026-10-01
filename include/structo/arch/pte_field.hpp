// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file pte_field.hpp
 * @brief `structo::arch::pte_bit_field<LowBit, NumBits, Int>`: a single,
 * named, compile-time-checked bit-field accessor (`get`/`set`/`test`)
 * used to build every per-architecture page-table-entry field
 * descriptor in `structo::arch::<arch>::pte_*.hpp`.
 *
 * ## Why this exists
 *
 * A hardware page-table entry is a flat integer where a handful of
 * fixed-position bit ranges (present, access permission, memory
 * attribute index, output address, ...) each mean something different --
 * the same "decompose a flat integer into named, fixed-position fields"
 * problem `page_table_traits.hpp` solves for *virtual addresses*, just
 * applied to *entries* instead. This header supplies the one small,
 * reusable primitive every per-architecture entry-field header
 * (`arm64::pte_stage1`, `x86::pte_ept`, `riscv::pte`, ...) builds its
 * named accessors out of, so each of those headers only has to state
 * *which* bits a field occupies, not re-derive the shift-and-mask logic
 * for extracting/replacing them.
 *
 * This header has zero architecture-specific knowledge -- it doesn't
 * know what "AF" or "S2AP" mean, only how to get/set an arbitrary
 * `[LowBit, LowBit + NumBits)` range out of an integer. Field *meaning*
 * belongs entirely to the per-architecture headers built on top of it.
 *
 * ## Example
 *
 * @code
 * using access_flag = structo::arch::pte_bit_field<10, 1, std::uint64_t>; // AF, bit 10
 * using ap_bits = structo::arch::pte_bit_field<6, 2, std::uint64_t>;      // AP[2:1], bits [7:6]
 *
 * std::uint64_t raw = 0;
 * raw = access_flag::set(raw, 1);       // set AF
 * raw = ap_bits::set(raw, 0b11);        // AP = 0b11
 * bool af = access_flag::test(raw);     // true
 * auto ap = ap_bits::get(raw);          // 0b11
 * @endcode
 */

#include <cstddef>
#include <cstdint>

namespace structo::arch {

/**
 * @brief A single named `[LowBit, LowBit + NumBits)` bit-field inside an
 * `Int`-wide raw value, with compile-time-checked get/set/test accessors.
 * @tparam LowBit Index (from bit 0) of the field's least-significant bit.
 * @tparam NumBits Width of the field, in bits (1 for a plain flag bit).
 * @tparam Int Underlying raw integer type the field lives in (typically
 * `std::uint64_t`, matching `page_table_entry<Tag, Int>::raw_type`).
 */
template <std::size_t LowBit, std::size_t NumBits, typename Int = std::uint64_t> struct pte_bit_field {
  static_assert(NumBits >= 1, "a bit-field must occupy at least one bit");
  static_assert(LowBit + NumBits <= sizeof(Int) * 8, "bit-field must fit within Int's width");

  static constexpr std::size_t low_bit = LowBit;
  static constexpr std::size_t num_bits = NumBits;

  /** @brief A mask of `NumBits` ones, shifted down to bit 0 (the field's max representable value). */
  [[nodiscard]] static constexpr Int value_mask() noexcept {
    return NumBits >= sizeof(Int) * 8 ? ~Int(0) : static_cast<Int>((Int(1) << NumBits) - Int(1));
  }

  /** @brief The field's mask in-place (`value_mask() << LowBit`). */
  [[nodiscard]] static constexpr Int field_mask() noexcept { return static_cast<Int>(value_mask() << LowBit); }

  /** @brief Extracts this field's value out of `raw`, right-justified. */
  [[nodiscard]] static constexpr Int get(Int raw) noexcept {
    return static_cast<Int>((raw >> LowBit) & value_mask());
  }

  /** @brief Returns `raw` with this field replaced by `value` (low `NumBits` bits of `value`, others ignored). */
  [[nodiscard]] static constexpr Int set(Int raw, Int value) noexcept {
    return static_cast<Int>((raw & ~field_mask()) | ((value & value_mask()) << LowBit));
  }

  /** @brief For a 1-bit field: whether the bit is set. */
  [[nodiscard]] static constexpr bool test(Int raw) noexcept {
    static_assert(NumBits == 1, "test() is only meaningful for a single-bit field; use get() for a wider one");
    return get(raw) != Int(0);
  }

  /** @brief For a 1-bit field: `raw` with the bit set (`value == true`) or cleared (`value == false`). */
  [[nodiscard]] static constexpr Int set_bit(Int raw, bool value) noexcept {
    static_assert(NumBits == 1, "set_bit() is only meaningful for a single-bit field; use set() for a wider one");
    return set(raw, value ? Int(1) : Int(0));
  }
};

} // namespace structo::arch
