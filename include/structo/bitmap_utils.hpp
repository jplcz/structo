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
