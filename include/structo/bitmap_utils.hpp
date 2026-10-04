// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file bitmap_utils.hpp
 * @brief `structo::bitmap_utils`: the stateless, span-based bit-twiddling
 * core meant to be shared by every fixed- or heap-backed bitmap type in
 * this library (`arch::cpu_mask<Tag, MaxCpus>`'s fixed-capacity bitset,
 * and the general-purpose `fixed_bitmap<N>`/`dynamic_bitmap`/
 * `bitmap_view` bitmaps, which derive their instance operations from
 * this class via the `bitmap_ops<Derived>` CRTP mixin -- see
 * `bitmap_ops.hpp` -- rather than re-implementing the same bit-scan/
 * atomic logic again) --
 * extracted so the same tri-tier (checked/`try_`/`unsafe_`) accessor
 * API, atomic `__atomic_*`-builtin operations, and `__builtin_ctzl`/
 * `__builtin_clzl`/`__builtin_popcountl`-based bit-scanning are
 * implemented and tested exactly once.
 *
 * Unlike `cpu_mask<Tag, MaxCpus>`, `bitmap_utils` owns no storage at
 * all: every method takes a `reloco::span<unsigned long>` (or `const`)
 * over the caller's own backing words -- a fixed in-class array, a
 * `reloco::vector<unsigned long>`, or anything else contiguous -- plus
 * an explicit `nbits` logical bit count (a span alone cannot distinguish
 * "every bit in this word is meaningful" from "this word is padded out
 * past a 100-bit logical size"). This mirrors the same "decay to a
 * type-erased view" pattern `cpu_mask::words()` already uses to hand its
 * own storage to code that isn't templated on `MaxCpus`, just inverted:
 * here the span is the primary interface, not an escape hatch.
 *
 * `unsigned long` (not a fixed-width `std::uint64_t`) is the word type
 * deliberately: it is the same word width `__builtin_ctzl`/`clzl`/
 * `popcountl` and a target's native bit-scan instruction operate on, so
 * a 32-bit target gets 32-bit words (and twice as many of them) rather
 * than this always assuming a 64-bit-register host -- the same reasoning
 * `slot_map_ptr.hpp` gives for dynamic remapping mattering most on
 * exactly those narrower-pointer targets.
 *
 * ## Usage shape
 *
 * `bitmap_utils` is purely a collection of `static` functions (never
 * instantiated); every call explicitly passes the words span and bit
 * count:
 *
 * @code
 * unsigned long words[structo::bitmap_utils::word_count_for(200)]{};
 * reloco::span<unsigned long> bits(words);
 *
 * structo::bitmap_utils::set(bits, 200, 5);
 * bool was_set = structo::bitmap_utils::test(bits, 200, 5);
 * auto free_slot = structo::bitmap_utils::lowest_clear(bits, 200); // "last free" scan
 * @endcode
 *
 * ## Range operations (`<sys/bitstring.h>`-style)
 *
 * Beyond single-bit access and whole-bitmap scans, `bitmap_utils` also
 * provides FreeBSD `<sys/bitstring.h>`-flavored range operations:
 * `set_range()`/`clear_range()` (`bit_nset()`/`bit_nclear()`),
 * `count_range()`/`all_set_in_range()`/`all_clear_in_range()`
 * (`bit_count()`/`bit_ntest()`), and `lowest_clear_run_from()`/
 * `lowest_set_run_from()`/`find_and_set_run_from()`
 * (`bit_ffc_area_at()`/`bit_ffs_area_at()`) for locating -- and
 * atomically-with-respect-to-the-caller's-own-lock claiming -- a
 * contiguous run of `size` consecutive bits, e.g. a multi-page-contiguous
 * allocation out of a page-frame bitmap:
 *
 * @code
 * // Find and claim 4 contiguous free page frames.
 * auto run = structo::bitmap_utils::find_and_set_run(bits, 200, 4);
 * if (run) {
 *   std::size_t first_frame = *run; // frames [first_frame, first_frame + 4) are now set
 * }
 * @endcode
 */

#include <atomic>
#include <cstddef>
#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>

#if !defined(__GNUC__) && !defined(__clang__)
#error "bitmap_utils's atomic_* operations require GCC or Clang (__atomic_* builtins)"
#endif

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace structo {

using namespace reloco;

/**
 * @brief Stateless, span-based bitmap operations: per-bit tri-tier
 * accessors, whole-bitmap scans/queries, and lock-free atomic variants.
 * See the @file-level docs for the rationale and usage shape. Never
 * instantiated -- every member is `static`.
 */
class bitmap_utils {
public:
  bitmap_utils() = delete;

  /** @brief Bits per backing word (`unsigned long`'s native width). */
  static constexpr std::size_t bits_per_word = sizeof(unsigned long) * 8;

  /** @brief Number of `unsigned long` words needed to hold `nbits` bits. */
  [[nodiscard]] static constexpr std::size_t word_count_for(std::size_t nbits) noexcept {
    return (nbits + bits_per_word - 1) / bits_per_word;
  }

  // ---------------------------------------------------------------------------
  // Per-Bit Tri-Tier Accessors
  // ---------------------------------------------------------------------------

  /** @brief Sets bit `index`. Traps (`RELOCO_ASSERT`) if `index >= nbits`. */
  static void set(span<unsigned long> words, std::size_t nbits, std::size_t index) noexcept {
    RELOCO_ASSERT(index < nbits, "bitmap_utils: set() index out of range");
    unsafe_set(words, index);
  }

  /** @brief Fallible variant of `set()`. */
  static result<void> try_set(span<unsigned long> words, std::size_t nbits, std::size_t index) noexcept {
    if (index >= nbits) {
      return unexpected(error::out_of_range);
    }
    unsafe_set(words, index);
    return {};
  }

  /** @brief Sets bit `index` without range-checking; UB if `index` doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static void unsafe_set(span<unsigned long> words, std::size_t index) noexcept {
    words.unsafe_at(index / bits_per_word) |= (1ul << (index % bits_per_word));
  }

  /** @brief Clears bit `index`. Traps (`RELOCO_ASSERT`) if `index >= nbits`. */
  static void clear(span<unsigned long> words, std::size_t nbits, std::size_t index) noexcept {
    RELOCO_ASSERT(index < nbits, "bitmap_utils: clear() index out of range");
    unsafe_clear(words, index);
  }

  /** @brief Fallible variant of `clear()`. */
  static result<void> try_clear(span<unsigned long> words, std::size_t nbits, std::size_t index) noexcept {
    if (index >= nbits) {
      return unexpected(error::out_of_range);
    }
    unsafe_clear(words, index);
    return {};
  }

  /** @brief Clears bit `index` without range-checking; UB if `index` doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static void unsafe_clear(span<unsigned long> words, std::size_t index) noexcept {
    words.unsafe_at(index / bits_per_word) &= ~(1ul << (index % bits_per_word));
  }

  /** @brief Returns whether bit `index` is set. Traps (`RELOCO_ASSERT`) if `index >= nbits`. */
  [[nodiscard]] static bool test(span<const unsigned long> words, std::size_t nbits, std::size_t index) noexcept {
    RELOCO_ASSERT(index < nbits, "bitmap_utils: test() index out of range");
    return unsafe_test(words, index);
  }

  /** @brief Fallible variant of `test()`. */
  [[nodiscard]] static result<bool> try_test(span<const unsigned long> words, std::size_t nbits,
                                             std::size_t index) noexcept {
    if (index >= nbits) {
      return unexpected(error::out_of_range);
    }
    return unsafe_test(words, index);
  }

  /** @brief Returns whether bit `index` is set, without range-checking; UB if `index` doesn't fit in `words`. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE static bool unsafe_test(span<const unsigned long> words,
                                                                   std::size_t index) noexcept {
    return (words.unsafe_at(index / bits_per_word) & (1ul << (index % bits_per_word))) != 0;
  }

  /** @brief Flips bit `index`. Traps (`RELOCO_ASSERT`) if `index >= nbits`. */
  static void toggle(span<unsigned long> words, std::size_t nbits, std::size_t index) noexcept {
    RELOCO_ASSERT(index < nbits, "bitmap_utils: toggle() index out of range");
    unsafe_toggle(words, index);
  }

  /** @brief Fallible variant of `toggle()`. */
  static result<void> try_toggle(span<unsigned long> words, std::size_t nbits, std::size_t index) noexcept {
    if (index >= nbits) {
      return unexpected(error::out_of_range);
    }
    unsafe_toggle(words, index);
    return {};
  }

  /** @brief Flips bit `index` without range-checking; UB if `index` doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static void unsafe_toggle(span<unsigned long> words, std::size_t index) noexcept {
    words.unsafe_at(index / bits_per_word) ^= (1ul << (index % bits_per_word));
  }

  // ---------------------------------------------------------------------------
  // Whole-Bitmap Queries
  // ---------------------------------------------------------------------------

  /** @brief Clears every word. */
  static void clear_all(span<unsigned long> words) noexcept {
    for (std::size_t i = 0; i < words.size(); ++i) {
      words.unsafe_at(i) = 0;
    }
  }

  /** @brief Sets every bit in `[0, nbits)` (and masks any trailing padding bits back to zero). */
  static void fill(span<unsigned long> words, std::size_t nbits) noexcept {
    for (std::size_t i = 0; i < words.size(); ++i) {
      words.unsafe_at(i) = ~0ul;
    }
    mask_tail_padding(words, nbits);
  }

  /**
   * @brief Clears any out-of-range bits in the last word (when `nbits`
   * isn't a multiple of `bits_per_word`), so `fill()`/`count()`/`all()`
   * never observe spurious bits past `nbits`.
   */
  static void mask_tail_padding(span<unsigned long> words, std::size_t nbits) noexcept {
    if (words.empty()) {
      return;
    }
    const std::size_t word_count = words.size();
    const std::size_t valid_bits_in_last_word = nbits - (word_count - 1) * bits_per_word;
    if (valid_bits_in_last_word < bits_per_word) {
      const unsigned long tail_mask = (1ul << valid_bits_in_last_word) - 1;
      words.unsafe_at(word_count - 1) &= tail_mask;
    }
  }

  /** @brief Number of set bits across `words`. */
  [[nodiscard]] static std::size_t count(span<const unsigned long> words) noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < words.size(); ++i) {
      total += static_cast<std::size_t>(__builtin_popcountl(words.unsafe_at(i)));
    }
    return total;
  }

  /** @brief `true` if at least one bit is set. */
  [[nodiscard]] static bool any(span<const unsigned long> words) noexcept {
    for (std::size_t i = 0; i < words.size(); ++i) {
      if (words.unsafe_at(i) != 0) {
        return true;
      }
    }
    return false;
  }

  /** @brief `true` if no bit is set. */
  [[nodiscard]] static bool none(span<const unsigned long> words) noexcept { return !any(words); }

  /** @brief `true` if every bit in `[0, nbits)` is set. */
  [[nodiscard]] static bool all(span<const unsigned long> words, std::size_t nbits) noexcept {
    return count(words) == nbits;
  }

  /** @brief Lowest set bit index at or after `start`, if any. */
  [[nodiscard]] static reloco::optional<std::size_t> lowest_set_from(span<const unsigned long> words, std::size_t nbits,
                                                                     std::size_t start) noexcept {
    if (start >= nbits || words.empty()) {
      return reloco::nullopt;
    }
    const std::size_t word_count = words.size();
    std::size_t word_idx = start / bits_per_word;
    unsigned long remaining = words.unsafe_at(word_idx) >> (start % bits_per_word);
    if (remaining != 0) {
      std::size_t bit = start + static_cast<std::size_t>(__builtin_ctzl(remaining));
      return bit < nbits ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
    }
    for (std::size_t i = word_idx + 1; i < word_count; ++i) {
      if (words.unsafe_at(i) != 0) {
        std::size_t bit = i * bits_per_word + static_cast<std::size_t>(__builtin_ctzl(words.unsafe_at(i)));
        return bit < nbits ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
      }
    }
    return reloco::nullopt;
  }

  /** @brief Lowest set bit index, if any. */
  [[nodiscard]] static reloco::optional<std::size_t> lowest_set(span<const unsigned long> words,
                                                                std::size_t nbits) noexcept {
    return lowest_set_from(words, nbits, 0);
  }

  /**
   * @brief Lowest CLEAR bit index at or after `start`, if any -- the
   * "find a free slot" scan a fixed-size pool allocator (e.g.
   * `shared_slot_map_mapper`'s busy bitmap) needs; the inverse of
   * `lowest_set_from()`.
   */
  [[nodiscard]] static reloco::optional<std::size_t> lowest_clear_from(span<const unsigned long> words,
                                                                       std::size_t nbits, std::size_t start) noexcept {
    if (start >= nbits || words.empty()) {
      return reloco::nullopt;
    }
    const std::size_t word_count = words.size();
    std::size_t word_idx = start / bits_per_word;
    unsigned long available = (~words.unsafe_at(word_idx)) >> (start % bits_per_word);
    if (available != 0) {
      std::size_t bit = start + static_cast<std::size_t>(__builtin_ctzl(available));
      return bit < nbits ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
    }
    for (std::size_t i = word_idx + 1; i < word_count; ++i) {
      unsigned long inverted = ~words.unsafe_at(i);
      if (inverted != 0) {
        std::size_t bit = i * bits_per_word + static_cast<std::size_t>(__builtin_ctzl(inverted));
        return bit < nbits ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
      }
    }
    return reloco::nullopt;
  }

  /** @brief Lowest clear bit index, if any. */
  [[nodiscard]] static reloco::optional<std::size_t> lowest_clear(span<const unsigned long> words,
                                                                  std::size_t nbits) noexcept {
    return lowest_clear_from(words, nbits, 0);
  }

  /** @brief Highest set bit index, if any. */
  [[nodiscard]] static reloco::optional<std::size_t> highest_set(span<const unsigned long> words,
                                                                 std::size_t nbits) noexcept {
    for (std::size_t word_idx = words.size(); word_idx-- > 0;) {
      unsigned long w = words.unsafe_at(word_idx);
      if (w == 0) {
        continue;
      }
      std::size_t bit = (bits_per_word - 1) - static_cast<std::size_t>(__builtin_clzl(w));
      std::size_t result = word_idx * bits_per_word + bit;
      return result < nbits ? reloco::optional<std::size_t>(result) : reloco::nullopt;
    }
    return reloco::nullopt;
  }

  /**
   * @brief Non-atomic "find the lowest clear bit at or after `start` and
   * set it" -- a scan-and-claim pool allocator primitive for callers
   * already holding whatever lock protects `words` (e.g.
   * `shared_slot_map_mapper::acquire()`'s free-slot scan).
   */
  [[nodiscard]] static reloco::optional<std::size_t> find_and_set_from(span<unsigned long> words, std::size_t nbits,
                                                                       std::size_t start) noexcept {
    auto bit = lowest_clear_from(words, nbits, start);
    if (bit.has_value()) {
      unsafe_set(words, bit.value());
    }
    return bit;
  }

  /** @brief `find_and_set_from()` starting at bit 0. */
  [[nodiscard]] static reloco::optional<std::size_t> find_and_set(span<unsigned long> words,
                                                                  std::size_t nbits) noexcept {
    return find_and_set_from(words, nbits, 0);
  }

  // ---------------------------------------------------------------------------
  // Range Operations (FreeBSD `bitstring.h`-style: bit_nset/bit_nclear/
  // bit_ntest/bit_count/bit_ffc_area_at/bit_ffs_area_at)
  // ---------------------------------------------------------------------------
  //
  // `[start, stop]` below is always INCLUSIVE of both endpoints, matching
  // `<sys/bitstring.h>`'s own `bit_nset()`/`bit_nclear()`/`bit_ntest()`
  // convention (rather than this library's usual half-open `[start, nbits)`
  // scan convention), since that is the natural way to describe "a run of
  // bits" once both ends are already known -- the "first contiguous run of
  // at least `size` bits" finders below stay half-open/`size`-based instead,
  // for the same reason `bit_ffc_area_at()`/`bit_ffs_area_at()` are.

  /** @brief Sets every bit in `[start, stop]`. Traps if `start > stop` or `stop >= nbits`. */
  static void set_range(span<unsigned long> words, std::size_t nbits, std::size_t start, std::size_t stop) noexcept {
    RELOCO_ASSERT(start <= stop && stop < nbits, "bitmap_utils: set_range() range out of bounds");
    unsafe_set_range(words, start, stop);
  }

  /** @brief Fallible variant of `set_range()`. */
  static result<void> try_set_range(span<unsigned long> words, std::size_t nbits, std::size_t start,
                                    std::size_t stop) noexcept {
    if (start > stop || stop >= nbits) {
      return unexpected(error::out_of_range);
    }
    unsafe_set_range(words, start, stop);
    return {};
  }

  /** @brief Sets every bit in `[start, stop]` without range-checking; UB if `start > stop` or the range doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static void unsafe_set_range(span<unsigned long> words, std::size_t start,
                                                          std::size_t stop) noexcept {
    const std::size_t first_word = start / bits_per_word;
    const std::size_t last_word = stop / bits_per_word;
    if (first_word == last_word) {
      words.unsafe_at(first_word) |= range_mask_(start % bits_per_word, stop % bits_per_word);
      return;
    }
    words.unsafe_at(first_word) |= range_mask_(start % bits_per_word, bits_per_word - 1);
    for (std::size_t w = first_word + 1; w < last_word; ++w) {
      words.unsafe_at(w) = ~0ul;
    }
    words.unsafe_at(last_word) |= range_mask_(0, stop % bits_per_word);
  }

  /** @brief Clears every bit in `[start, stop]`. Traps if `start > stop` or `stop >= nbits`. */
  static void clear_range(span<unsigned long> words, std::size_t nbits, std::size_t start,
                          std::size_t stop) noexcept {
    RELOCO_ASSERT(start <= stop && stop < nbits, "bitmap_utils: clear_range() range out of bounds");
    unsafe_clear_range(words, start, stop);
  }

  /** @brief Fallible variant of `clear_range()`. */
  static result<void> try_clear_range(span<unsigned long> words, std::size_t nbits, std::size_t start,
                                      std::size_t stop) noexcept {
    if (start > stop || stop >= nbits) {
      return unexpected(error::out_of_range);
    }
    unsafe_clear_range(words, start, stop);
    return {};
  }

  /** @brief Clears every bit in `[start, stop]` without range-checking; UB if `start > stop` or the range doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static void unsafe_clear_range(span<unsigned long> words, std::size_t start,
                                                            std::size_t stop) noexcept {
    const std::size_t first_word = start / bits_per_word;
    const std::size_t last_word = stop / bits_per_word;
    if (first_word == last_word) {
      words.unsafe_at(first_word) &= ~range_mask_(start % bits_per_word, stop % bits_per_word);
      return;
    }
    words.unsafe_at(first_word) &= ~range_mask_(start % bits_per_word, bits_per_word - 1);
    for (std::size_t w = first_word + 1; w < last_word; ++w) {
      words.unsafe_at(w) = 0;
    }
    words.unsafe_at(last_word) &= ~range_mask_(0, stop % bits_per_word);
  }

  /** @brief Number of set bits in `[start, stop]`. Traps if `start > stop` or `stop >= nbits`. */
  [[nodiscard]] static std::size_t count_range(span<const unsigned long> words, std::size_t nbits, std::size_t start,
                                               std::size_t stop) noexcept {
    RELOCO_ASSERT(start <= stop && stop < nbits, "bitmap_utils: count_range() range out of bounds");
    return unsafe_count_range(words, start, stop);
  }

  /** @brief Fallible variant of `count_range()`. */
  [[nodiscard]] static result<std::size_t> try_count_range(span<const unsigned long> words, std::size_t nbits,
                                                           std::size_t start, std::size_t stop) noexcept {
    if (start > stop || stop >= nbits) {
      return unexpected(error::out_of_range);
    }
    return unsafe_count_range(words, start, stop);
  }

  /** @brief `count_range()` without range-checking; UB if `start > stop` or the range doesn't fit in `words`. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE static std::size_t
  unsafe_count_range(span<const unsigned long> words, std::size_t start, std::size_t stop) noexcept {
    const std::size_t first_word = start / bits_per_word;
    const std::size_t last_word = stop / bits_per_word;
    if (first_word == last_word) {
      return static_cast<std::size_t>(
          __builtin_popcountl(words.unsafe_at(first_word) & range_mask_(start % bits_per_word, stop % bits_per_word)));
    }
    std::size_t total = static_cast<std::size_t>(
        __builtin_popcountl(words.unsafe_at(first_word) & range_mask_(start % bits_per_word, bits_per_word - 1)));
    for (std::size_t w = first_word + 1; w < last_word; ++w) {
      total += static_cast<std::size_t>(__builtin_popcountl(words.unsafe_at(w)));
    }
    total +=
        static_cast<std::size_t>(__builtin_popcountl(words.unsafe_at(last_word) & range_mask_(0, stop % bits_per_word)));
    return total;
  }

  /** @brief `true` if every bit in `[start, stop]` is set. Traps if `start > stop` or `stop >= nbits`. */
  [[nodiscard]] static bool all_set_in_range(span<const unsigned long> words, std::size_t nbits, std::size_t start,
                                             std::size_t stop) noexcept {
    return count_range(words, nbits, start, stop) == (stop - start + 1);
  }

  /** @brief Fallible variant of `all_set_in_range()`. */
  [[nodiscard]] static result<bool> try_all_set_in_range(span<const unsigned long> words, std::size_t nbits,
                                                         std::size_t start, std::size_t stop) noexcept {
    auto n = try_count_range(words, nbits, start, stop);
    if (!n) {
      return unexpected(n.error());
    }
    return *n == (stop - start + 1);
  }

  /** @brief `all_set_in_range()` without range-checking; UB if `start > stop` or the range doesn't fit in `words`. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE static bool
  unsafe_all_set_in_range(span<const unsigned long> words, std::size_t start, std::size_t stop) noexcept {
    return unsafe_count_range(words, start, stop) == (stop - start + 1);
  }

  /** @brief `true` if every bit in `[start, stop]` is clear. Traps if `start > stop` or `stop >= nbits`. */
  [[nodiscard]] static bool all_clear_in_range(span<const unsigned long> words, std::size_t nbits, std::size_t start,
                                               std::size_t stop) noexcept {
    return count_range(words, nbits, start, stop) == 0;
  }

  /** @brief Fallible variant of `all_clear_in_range()`. */
  [[nodiscard]] static result<bool> try_all_clear_in_range(span<const unsigned long> words, std::size_t nbits,
                                                           std::size_t start, std::size_t stop) noexcept {
    auto n = try_count_range(words, nbits, start, stop);
    if (!n) {
      return unexpected(n.error());
    }
    return *n == 0;
  }

  /** @brief `all_clear_in_range()` without range-checking; UB if `start > stop` or the range doesn't fit in `words`. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE static bool
  unsafe_all_clear_in_range(span<const unsigned long> words, std::size_t start, std::size_t stop) noexcept {
    return unsafe_count_range(words, start, stop) == 0;
  }

  /**
   * @brief Lowest index at or after `start` where `size` consecutive CLEAR
   * bits begin (all within `[0, nbits)`), if any -- the "find a
   * contiguous free run" scan `bit_ffc_area_at()` provides, e.g. for a
   * multi-page-contiguous allocation out of a page-frame bitmap. A
   * `size == 0` request trivially matches at `start` itself (if
   * `start <= nbits`).
   */
  [[nodiscard]] static reloco::optional<std::size_t>
  lowest_clear_run_from(span<const unsigned long> words, std::size_t nbits, std::size_t start,
                        std::size_t size) noexcept {
    if (size == 0) {
      return start <= nbits ? reloco::optional<std::size_t>(start) : reloco::nullopt;
    }
    if (start + size > nbits) {
      return reloco::nullopt;
    }
    std::size_t candidate = start;
    while (candidate + size <= nbits) {
      // Any set bit strictly inside [candidate, candidate + size) blocks
      // this candidate; jump straight past it rather than re-testing bit
      // by bit (mirrors bit_ffc_area_at_()'s own word-skipping shape).
      auto obstruction = lowest_set_from(words, candidate + size, candidate);
      if (!obstruction.has_value()) {
        return candidate;
      }
      candidate = *obstruction + 1;
    }
    return reloco::nullopt;
  }

  /** @brief `lowest_clear_run_from()` starting at bit 0. */
  [[nodiscard]] static reloco::optional<std::size_t> lowest_clear_run(span<const unsigned long> words,
                                                                      std::size_t nbits, std::size_t size) noexcept {
    return lowest_clear_run_from(words, nbits, 0, size);
  }

  /**
   * @brief Lowest index at or after `start` where `size` consecutive SET
   * bits begin, if any -- the inverse of `lowest_clear_run_from()`
   * (`bit_ffs_area_at()`).
   */
  [[nodiscard]] static reloco::optional<std::size_t>
  lowest_set_run_from(span<const unsigned long> words, std::size_t nbits, std::size_t start,
                      std::size_t size) noexcept {
    if (size == 0) {
      return start <= nbits ? reloco::optional<std::size_t>(start) : reloco::nullopt;
    }
    if (start + size > nbits) {
      return reloco::nullopt;
    }
    std::size_t candidate = start;
    while (candidate + size <= nbits) {
      auto obstruction = lowest_clear_from(words, candidate + size, candidate);
      if (!obstruction.has_value()) {
        return candidate;
      }
      candidate = *obstruction + 1;
    }
    return reloco::nullopt;
  }

  /** @brief `lowest_set_run_from()` starting at bit 0. */
  [[nodiscard]] static reloco::optional<std::size_t> lowest_set_run(span<const unsigned long> words, std::size_t nbits,
                                                                    std::size_t size) noexcept {
    return lowest_set_run_from(words, nbits, 0, size);
  }

  /**
   * @brief Non-atomic "find the lowest run of `size` consecutive clear
   * bits at or after `start` and set all of them" -- the contiguous-run
   * counterpart to `find_and_set_from()`, for callers already holding
   * whatever lock protects `words`.
   */
  [[nodiscard]] static reloco::optional<std::size_t> find_and_set_run_from(span<unsigned long> words,
                                                                           std::size_t nbits, std::size_t start,
                                                                           std::size_t size) noexcept {
    auto pos = lowest_clear_run_from(words, nbits, start, size);
    if (pos.has_value() && size > 0) {
      unsafe_set_range(words, pos.value(), pos.value() + size - 1);
    }
    return pos;
  }

  /** @brief `find_and_set_run_from()` starting at bit 0. */
  [[nodiscard]] static reloco::optional<std::size_t> find_and_set_run(span<unsigned long> words, std::size_t nbits,
                                                                      std::size_t size) noexcept {
    return find_and_set_run_from(words, nbits, 0, size);
  }

  // ---------------------------------------------------------------------------
  // Set Algebra (Rust `bitflags`-flavored, word-for-word across two
  // same-sized bitmaps)
  // ---------------------------------------------------------------------------
  //
  // Every function below treats `a`/`b` (or `dst`/`src`) as two bitmaps of
  // the *same* logical size and operates word-for-word; callers are
  // responsible for only ever comparing/combining bitmaps that share the
  // same `nbits` (mismatched word-span sizes trap via `RELOCO_ASSERT`).
  // The mutating members never need to allocate or construct a new bitmap,
  // so they work uniformly across `fixed_bitmap<N>`, `dynamic_bitmap`, and
  // even non-owning `bitmap_view`.

  /** @brief `true` if every bit set in `b` is also set in `a` (`a` is a superset of `b`). Traps if sizes differ. */
  [[nodiscard]] static bool is_superset_of(span<const unsigned long> a, span<const unsigned long> b) noexcept {
    RELOCO_ASSERT(a.size() == b.size(), "bitmap_utils: is_superset_of() size mismatch");
    for (std::size_t i = 0; i < a.size(); ++i) {
      if ((b.unsafe_at(i) & ~a.unsafe_at(i)) != 0) {
        return false;
      }
    }
    return true;
  }

  /** @brief `true` if `a` and `b` have at least one set bit in common. Traps if sizes differ. */
  [[nodiscard]] static bool intersects(span<const unsigned long> a, span<const unsigned long> b) noexcept {
    RELOCO_ASSERT(a.size() == b.size(), "bitmap_utils: intersects() size mismatch");
    for (std::size_t i = 0; i < a.size(); ++i) {
      if ((a.unsafe_at(i) & b.unsafe_at(i)) != 0) {
        return true;
      }
    }
    return false;
  }

  /** @brief `true` if `a` and `b` are bitwise identical. Traps if sizes differ. */
  [[nodiscard]] static bool equals(span<const unsigned long> a, span<const unsigned long> b) noexcept {
    RELOCO_ASSERT(a.size() == b.size(), "bitmap_utils: equals() size mismatch");
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a.unsafe_at(i) != b.unsafe_at(i)) {
        return false;
      }
    }
    return true;
  }

  /** @brief `dst |= src`, word-for-word (set union, in place). Traps if sizes differ. */
  static void union_with(span<unsigned long> dst, span<const unsigned long> src) noexcept {
    RELOCO_ASSERT(dst.size() == src.size(), "bitmap_utils: union_with() size mismatch");
    for (std::size_t i = 0; i < dst.size(); ++i) {
      dst.unsafe_at(i) |= src.unsafe_at(i);
    }
  }

  /** @brief `dst &= src`, word-for-word (set intersection, in place). Traps if sizes differ. */
  static void intersect_with(span<unsigned long> dst, span<const unsigned long> src) noexcept {
    RELOCO_ASSERT(dst.size() == src.size(), "bitmap_utils: intersect_with() size mismatch");
    for (std::size_t i = 0; i < dst.size(); ++i) {
      dst.unsafe_at(i) &= src.unsafe_at(i);
    }
  }

  /**
   * @brief `dst &= ~src`, word-for-word (set difference, in place: clears
   * every bit that `src` has set, leaving the rest of `dst` untouched).
   * Traps if sizes differ.
   */
  static void subtract(span<unsigned long> dst, span<const unsigned long> src) noexcept {
    RELOCO_ASSERT(dst.size() == src.size(), "bitmap_utils: subtract() size mismatch");
    for (std::size_t i = 0; i < dst.size(); ++i) {
      dst.unsafe_at(i) &= ~src.unsafe_at(i);
    }
  }

  /** @brief `dst ^= src`, word-for-word (symmetric difference, in place). Traps if sizes differ. */
  static void symmetric_difference_with(span<unsigned long> dst, span<const unsigned long> src) noexcept {
    RELOCO_ASSERT(dst.size() == src.size(), "bitmap_utils: symmetric_difference_with() size mismatch");
    for (std::size_t i = 0; i < dst.size(); ++i) {
      dst.unsafe_at(i) ^= src.unsafe_at(i);
    }
  }

  /** @brief Flips every bit in `[0, nbits)` in place (set complement), re-masking any tail padding back to zero. */
  static void invert(span<unsigned long> words, std::size_t nbits) noexcept {
    for (std::size_t i = 0; i < words.size(); ++i) {
      words.unsafe_at(i) = ~words.unsafe_at(i);
    }
    mask_tail_padding(words, nbits);
  }

  // ---------------------------------------------------------------------------
  // Atomic Operations (lock-free, GCC/Clang `__atomic_*` builtins)
  // ---------------------------------------------------------------------------
  //
  // Each atomic_* function below is a single, genuinely atomic read-modify-
  // write (or load) of the ONE word containing the requested bit,
  // implemented directly with GCC/Clang's `__atomic_*` builtins -- see
  // `arch/cpu_mask.hpp`'s own docs for the full rationale (this is the
  // same convention, just over a caller-supplied span instead of
  // `cpu_mask`'s own fixed storage). This gives per-bit atomicity, not
  // whole-bitmap atomicity: a concurrent reader/writer can observe one
  // word already updated and another not yet.

  /** @brief Atomically returns whether bit `index` is set. Traps if `index >= nbits`. */
  [[nodiscard]] static bool atomic_test(span<const unsigned long> words, std::size_t nbits, std::size_t index,
                                        std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(index < nbits, "bitmap_utils: atomic_test() index out of range");
    return unsafe_atomic_test(words, index, order);
  }

  /** @brief Fallible variant of `atomic_test()`. */
  [[nodiscard]] static result<bool> atomic_try_test(span<const unsigned long> words, std::size_t nbits,
                                                    std::size_t index,
                                                    std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (index >= nbits) {
      return unexpected(error::out_of_range);
    }
    return unsafe_atomic_test(words, index, order);
  }

  /** @brief `atomic_test()` without range-checking; UB if `index` doesn't fit in `words`. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE static bool
  unsafe_atomic_test(span<const unsigned long> words, std::size_t index,
                     std::memory_order order = std::memory_order_seq_cst) noexcept {
    unsigned long w = __atomic_load_n(&words.unsafe_at(index / bits_per_word), to_atomic_order(order));
    return (w & (1ul << (index % bits_per_word))) != 0;
  }

  /** @brief Atomically sets bit `index`. Traps if `index >= nbits`. */
  static void atomic_set(span<unsigned long> words, std::size_t nbits, std::size_t index,
                         std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(index < nbits, "bitmap_utils: atomic_set() index out of range");
    unsafe_atomic_set(words, index, order);
  }

  /** @brief Fallible variant of `atomic_set()`. */
  static result<void> atomic_try_set(span<unsigned long> words, std::size_t nbits, std::size_t index,
                                     std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (index >= nbits) {
      return unexpected(error::out_of_range);
    }
    unsafe_atomic_set(words, index, order);
    return {};
  }

  /** @brief `atomic_set()` without range-checking; UB if `index` doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static void
  unsafe_atomic_set(span<unsigned long> words, std::size_t index,
                    std::memory_order order = std::memory_order_seq_cst) noexcept {
    __atomic_fetch_or(&words.unsafe_at(index / bits_per_word), 1ul << (index % bits_per_word), to_atomic_order(order));
  }

  /** @brief Atomically clears bit `index`. Traps if `index >= nbits`. */
  static void atomic_clear(span<unsigned long> words, std::size_t nbits, std::size_t index,
                           std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(index < nbits, "bitmap_utils: atomic_clear() index out of range");
    unsafe_atomic_clear(words, index, order);
  }

  /** @brief Fallible variant of `atomic_clear()`. */
  static result<void> atomic_try_clear(span<unsigned long> words, std::size_t nbits, std::size_t index,
                                       std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (index >= nbits) {
      return unexpected(error::out_of_range);
    }
    unsafe_atomic_clear(words, index, order);
    return {};
  }

  /** @brief `atomic_clear()` without range-checking; UB if `index` doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static void
  unsafe_atomic_clear(span<unsigned long> words, std::size_t index,
                      std::memory_order order = std::memory_order_seq_cst) noexcept {
    __atomic_fetch_and(&words.unsafe_at(index / bits_per_word), ~(1ul << (index % bits_per_word)),
                       to_atomic_order(order));
  }

  /** @brief Atomically flips bit `index`. Traps if `index >= nbits`. */
  static void atomic_toggle(span<unsigned long> words, std::size_t nbits, std::size_t index,
                            std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(index < nbits, "bitmap_utils: atomic_toggle() index out of range");
    unsafe_atomic_toggle(words, index, order);
  }

  /** @brief Fallible variant of `atomic_toggle()`. */
  static result<void> atomic_try_toggle(span<unsigned long> words, std::size_t nbits, std::size_t index,
                                        std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (index >= nbits) {
      return unexpected(error::out_of_range);
    }
    unsafe_atomic_toggle(words, index, order);
    return {};
  }

  /** @brief `atomic_toggle()` without range-checking; UB if `index` doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static void
  unsafe_atomic_toggle(span<unsigned long> words, std::size_t index,
                       std::memory_order order = std::memory_order_seq_cst) noexcept {
    __atomic_fetch_xor(&words.unsafe_at(index / bits_per_word), 1ul << (index % bits_per_word), to_atomic_order(order));
  }

  /** @brief Atomically sets bit `index`, returning its PREVIOUS value. UB if `index` doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static bool
  unsafe_atomic_test_and_set(span<unsigned long> words, std::size_t index,
                             std::memory_order order = std::memory_order_seq_cst) noexcept {
    unsigned long bit = 1ul << (index % bits_per_word);
    unsigned long prev = __atomic_fetch_or(&words.unsafe_at(index / bits_per_word), bit, to_atomic_order(order));
    return (prev & bit) != 0;
  }

  /** @brief Atomically clears bit `index`, returning its PREVIOUS value. UB if `index` doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static bool
  unsafe_atomic_test_and_clear(span<unsigned long> words, std::size_t index,
                               std::memory_order order = std::memory_order_seq_cst) noexcept {
    unsigned long bit = 1ul << (index % bits_per_word);
    unsigned long prev = __atomic_fetch_and(&words.unsafe_at(index / bits_per_word), ~bit, to_atomic_order(order));
    return (prev & bit) != 0;
  }

  /** @brief Atomically flips bit `index`, returning its PREVIOUS value. UB if `index` doesn't fit in `words`. */
  RELOCO_UNSAFE_BUFFER_USAGE static bool
  unsafe_atomic_test_and_toggle(span<unsigned long> words, std::size_t index,
                                std::memory_order order = std::memory_order_seq_cst) noexcept {
    unsigned long bit = 1ul << (index % bits_per_word);
    unsigned long prev = __atomic_fetch_xor(&words.unsafe_at(index / bits_per_word), bit, to_atomic_order(order));
    return (prev & bit) != 0;
  }

  /** @brief Atomically loads word `index`. Traps if `index >= words.size()`. */
  [[nodiscard]] static unsigned long atomic_word(span<const unsigned long> words, std::size_t index,
                                                 std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(index < words.size(), "bitmap_utils: atomic_word() index out of range");
    return __atomic_load_n(&words.unsafe_at(index), to_atomic_order(order));
  }

  /**
   * @brief Atomically finds the lowest set bit at or after `start`, if
   * any. Mirrors Linux's `find_next_bit()`. Each backing word is loaded
   * with a single atomic `__atomic_load_n()`, and `__builtin_ctzl()`
   * locates the lowest set bit within it. As with every other atomic_*
   * operation here, this is only atomic per-word: a concurrent writer
   * can set a bit in an already-scanned word after this call observed it
   * as zero, so the result is a snapshot, not a linearization point
   * across the whole bitmap.
   */
  [[nodiscard]] static reloco::optional<std::size_t>
  atomic_lowest_set_from(span<const unsigned long> words, std::size_t nbits, std::size_t start,
                         std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (start >= nbits || words.empty()) {
      return reloco::nullopt;
    }
    const std::size_t word_count = words.size();
    std::size_t word_idx = start / bits_per_word;
    unsigned long remaining = atomic_word(words, word_idx, order) >> (start % bits_per_word);
    if (remaining != 0) {
      std::size_t bit = start + static_cast<std::size_t>(__builtin_ctzl(remaining));
      return bit < nbits ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
    }
    for (std::size_t i = word_idx + 1; i < word_count; ++i) {
      unsigned long w = atomic_word(words, i, order);
      if (w != 0) {
        std::size_t bit = i * bits_per_word + static_cast<std::size_t>(__builtin_ctzl(w));
        return bit < nbits ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
      }
    }
    return reloco::nullopt;
  }

  /** @brief Atomically finds the lowest set bit, if any. */
  [[nodiscard]] static reloco::optional<std::size_t>
  atomic_lowest_set(span<const unsigned long> words, std::size_t nbits,
                    std::memory_order order = std::memory_order_seq_cst) noexcept {
    return atomic_lowest_set_from(words, nbits, 0, order);
  }

  /**
   * @brief Atomically finds the lowest CLEAR bit at or after `start` and
   * sets it, returning that index -- a lock-free "allocate one slot"
   * primitive (mirrors Rust's `AtomicUsize`-based bitset allocators and
   * `arch/asid_allocator.hpp`'s bitmap scan-and-claim pattern, generalized
   * to any caller-supplied bitmap).
   *
   * Retries internally via compare-and-swap on the owning word if another
   * thread concurrently claims/frees bits in the same word; never blocks
   * or traps. Returns `nullopt` if every bit at or after `start` is
   * already set.
   */
  [[nodiscard]] static reloco::optional<std::size_t>
  atomic_find_and_set_from(span<unsigned long> words, std::size_t nbits, std::size_t start,
                           std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (start >= nbits) {
      return reloco::nullopt;
    }
    const std::size_t word_count = words.size();
    std::size_t start_word = start / bits_per_word;
    for (std::size_t word_idx = start_word; word_idx < word_count; ++word_idx) {
      std::size_t word_base = word_idx * bits_per_word;
      std::size_t low_bound = word_idx == start_word ? start % bits_per_word : 0;
      unsigned long expected = __atomic_load_n(&words.unsafe_at(word_idx), to_atomic_order(order));
      for (;;) {
        unsigned long available = ~expected >> low_bound;
        if (available == 0) {
          break; // this word is exhausted (from low_bound onward); move to the next word
        }
        std::size_t bit_in_word = low_bound + static_cast<std::size_t>(__builtin_ctzl(available));
        std::size_t index = word_base + bit_in_word;
        if (index >= nbits) {
          break;
        }
        unsigned long desired = expected | (1ul << bit_in_word);
        if (__atomic_compare_exchange_n(&words.unsafe_at(word_idx), &expected, desired, /*weak=*/true,
                                        to_atomic_order(order), to_atomic_order(order))) {
          return index;
        }
        // `expected` was refreshed with the current value by the failed CAS; retry.
      }
    }
    return reloco::nullopt;
  }

  /** @brief Atomically finds and sets the lowest clear bit, if any. */
  [[nodiscard]] static reloco::optional<std::size_t>
  atomic_find_and_set(span<unsigned long> words, std::size_t nbits,
                      std::memory_order order = std::memory_order_seq_cst) noexcept {
    return atomic_find_and_set_from(words, nbits, 0, order);
  }

  /**
   * @brief Takes a plain, non-atomic word-by-word snapshot of `src` into
   * `dst` (same size), loading each source word with a single atomic
   * `__atomic_load_n()`. Like every other atomic_* operation here, this
   * is only atomic per-word, not whole-bitmap-atomic.
   */
  static void atomic_snapshot(span<const unsigned long> src, span<unsigned long> dst,
                              std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(src.size() == dst.size(), "bitmap_utils: atomic_snapshot() size mismatch");
    for (std::size_t i = 0; i < src.size(); ++i) {
      dst.unsafe_at(i) = atomic_word(src, i, order);
    }
  }

private:
  /**
   * @brief Mask covering bits `[start_offset, stop_offset]` (both
   * inclusive, both already local bit offsets within a single word --
   * i.e. each `< bits_per_word`) -- the single-word building block
   * `unsafe_set_range()`/`unsafe_clear_range()`/`unsafe_count_range()`
   * compose across multiple words, mirroring `<sys/bitstring.h>`'s own
   * `_bit_make_mask()`.
   */
  [[nodiscard]] static constexpr unsigned long range_mask_(std::size_t start_offset,
                                                           std::size_t stop_offset) noexcept {
    const unsigned long low = ~0ul << start_offset;
    const unsigned long high = (stop_offset + 1 == bits_per_word) ? ~0ul : ((1ul << (stop_offset + 1)) - 1);
    return low & high;
  }

  // GCC/Clang's `__atomic_*` builtins take a plain `int` memory-order
  // constant (`__ATOMIC_RELAXED`, ...); both compilers' `std::memory_order`
  // enumerators are defined with exactly these same underlying values
  // (see `arch/cpu_mask.hpp`'s identical helper), so this cast is safe
  // given this library only targets GCC/Clang.
  [[nodiscard]] static constexpr int to_atomic_order(std::memory_order order) noexcept {
    return static_cast<int>(order);
  }
};

} // namespace structo

RELOCO_END_UNSAFE_BUFFER_USAGE
