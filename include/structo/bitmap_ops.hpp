// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file bitmap_ops.hpp
 * @brief `structo::bitmap_ops<Derived>`: the CRTP base every concrete
 * bitmap type in this library (`fixed_bitmap<N>`, `dynamic_bitmap`,
 * `bitmap_view`) publicly derives from to get the full instance-method
 * bitmap API -- tri-tier (checked/`try_`/`unsafe_`) per-bit accessors,
 * whole-bitmap scans/queries, and atomic variants -- for free, each call
 * simply forwarding to the matching `structo::bitmap_utils` static
 * function (see `bitmap_utils.hpp`) over `Derived`'s own backing words.
 *
 * This mirrors the CRTP convention already used elsewhere in this
 * codebase (`reloco::iterator_adaptor<Derived, Item>` for the `Iterator`
 * adapter surface, `structo::phys_page::os_traits_base<Derived, ...>`
 * for buddy-allocator neighbor bookkeeping): one place implements and
 * tests every operation exactly once, and each concrete bitmap type only
 * has to supply the handful of primitives `bitmap_ops` needs to get
 * there.
 *
 * ## `Derived` contract
 *
 * `Derived` must publicly derive from `bitmap_ops<Derived>` and provide:
 * - `span<unsigned long> words() & noexcept;` (mutable view over the
 *   backing words, for mutating operations)
 * - `span<const unsigned long> words() const & noexcept;` (read-only
 *   view, for query operations)
 * - `std::size_t nbits() const noexcept;` (the logical bit count --
 *   `words().size() * bitmap_utils::bits_per_word` may be larger, if
 *   the last word has unused tail padding)
 *
 * @code
 * class my_bitmap : public structo::bitmap_ops<my_bitmap> {
 * public:
 *   // ... backing storage + constructors ...
 *   span<unsigned long> words() & noexcept { return span<unsigned long>(words_, word_count_); }
 *   span<const unsigned long> words() const & noexcept { return span<const unsigned long>(words_, word_count_); }
 *   std::size_t nbits() const noexcept { return nbits_; }
 * private:
 *   unsigned long *words_;
 *   std::size_t word_count_;
 *   std::size_t nbits_;
 * };
 *
 * my_bitmap b = ...;
 * b.set(5);
 * bool was_set = b.test(5);
 * auto free_slot = b.lowest_clear();
 * @endcode
 */

#include "bitmap_utils.hpp"

#include <atomic>
#include <cstddef>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>

namespace structo {

using namespace reloco;

/**
 * @brief CRTP base providing the full `bitmap_utils`-backed instance API
 * on top of `Derived::words()`/`Derived::nbits()`. See the @file-level
 * docs for the `Derived` contract. Never instantiated on its own.
 */
template <typename Derived> class bitmap_ops {
public:
  /** @brief The logical number of bits this bitmap represents. */
  [[nodiscard]] std::size_t size() const noexcept { return derived().nbits(); }

  // ---------------------------------------------------------------------------
  // Per-Bit Tri-Tier Accessors
  // ---------------------------------------------------------------------------

  /** @brief Sets bit `index`. Traps (`RELOCO_ASSERT`) if `index >= size()`. */
  void set(std::size_t index) noexcept { bitmap_utils::set(derived().words(), derived().nbits(), index); }

  /** @brief Fallible variant of `set()`. */
  result<void> try_set(std::size_t index) noexcept {
    return bitmap_utils::try_set(derived().words(), derived().nbits(), index);
  }

  /** @brief Sets bit `index` without range-checking; UB if `index` doesn't fit in the backing words. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_set(std::size_t index) noexcept {
    bitmap_utils::unsafe_set(derived().words(), index);
  }

  /** @brief Clears bit `index`. Traps (`RELOCO_ASSERT`) if `index >= size()`. */
  void clear(std::size_t index) noexcept { bitmap_utils::clear(derived().words(), derived().nbits(), index); }

  /** @brief Fallible variant of `clear()`. */
  result<void> try_clear(std::size_t index) noexcept {
    return bitmap_utils::try_clear(derived().words(), derived().nbits(), index);
  }

  /** @brief Clears bit `index` without range-checking; UB if `index` doesn't fit in the backing words. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_clear(std::size_t index) noexcept {
    bitmap_utils::unsafe_clear(derived().words(), index);
  }

  /** @brief Returns whether bit `index` is set. Traps (`RELOCO_ASSERT`) if `index >= size()`. */
  [[nodiscard]] bool test(std::size_t index) const noexcept {
    return bitmap_utils::test(derived().words(), derived().nbits(), index);
  }

  /** @brief Fallible variant of `test()`. */
  [[nodiscard]] result<bool> try_test(std::size_t index) const noexcept {
    return bitmap_utils::try_test(derived().words(), derived().nbits(), index);
  }

  /** @brief Returns whether bit `index` is set, without range-checking; UB if `index` doesn't fit in the backing words. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE bool unsafe_test(std::size_t index) const noexcept {
    return bitmap_utils::unsafe_test(derived().words(), index);
  }

  /** @brief Flips bit `index`. Traps (`RELOCO_ASSERT`) if `index >= size()`. */
  void toggle(std::size_t index) noexcept { bitmap_utils::toggle(derived().words(), derived().nbits(), index); }

  /** @brief Fallible variant of `toggle()`. */
  result<void> try_toggle(std::size_t index) noexcept {
    return bitmap_utils::try_toggle(derived().words(), derived().nbits(), index);
  }

  /** @brief Flips bit `index` without range-checking; UB if `index` doesn't fit in the backing words. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_toggle(std::size_t index) noexcept {
    bitmap_utils::unsafe_toggle(derived().words(), index);
  }

  // ---------------------------------------------------------------------------
  // Whole-Bitmap Queries
  // ---------------------------------------------------------------------------

  /** @brief Clears every word. */
  void clear_all() noexcept { bitmap_utils::clear_all(derived().words()); }

  /** @brief Sets every bit in `[0, size())`. */
  void fill() noexcept { bitmap_utils::fill(derived().words(), derived().nbits()); }

  /** @brief Number of set bits. */
  [[nodiscard]] std::size_t count() const noexcept { return bitmap_utils::count(derived().words()); }

  /** @brief `true` if at least one bit is set. */
  [[nodiscard]] bool any() const noexcept { return bitmap_utils::any(derived().words()); }

  /** @brief `true` if no bit is set. */
  [[nodiscard]] bool none() const noexcept { return bitmap_utils::none(derived().words()); }

  /** @brief `true` if every bit in `[0, size())` is set. */
  [[nodiscard]] bool all() const noexcept { return bitmap_utils::all(derived().words(), derived().nbits()); }

  /** @brief Lowest set bit index at or after `start`, if any. */
  [[nodiscard]] reloco::optional<std::size_t> lowest_set_from(std::size_t start) const noexcept {
    return bitmap_utils::lowest_set_from(derived().words(), derived().nbits(), start);
  }

  /** @brief Lowest set bit index, if any. */
  [[nodiscard]] reloco::optional<std::size_t> lowest_set() const noexcept {
    return bitmap_utils::lowest_set(derived().words(), derived().nbits());
  }

  /** @brief Lowest CLEAR bit index at or after `start`, if any -- the "find a free slot" scan. */
  [[nodiscard]] reloco::optional<std::size_t> lowest_clear_from(std::size_t start) const noexcept {
    return bitmap_utils::lowest_clear_from(derived().words(), derived().nbits(), start);
  }

  /** @brief Lowest clear bit index, if any. */
  [[nodiscard]] reloco::optional<std::size_t> lowest_clear() const noexcept {
    return bitmap_utils::lowest_clear(derived().words(), derived().nbits());
  }

  /** @brief Highest set bit index, if any. */
  [[nodiscard]] reloco::optional<std::size_t> highest_set() const noexcept {
    return bitmap_utils::highest_set(derived().words(), derived().nbits());
  }

  /**
   * @brief Non-atomic "find the lowest clear bit at or after `start` and
   * set it" -- a scan-and-claim pool allocator primitive for callers
   * already holding whatever lock protects this bitmap.
   */
  [[nodiscard]] reloco::optional<std::size_t> find_and_set_from(std::size_t start) noexcept {
    return bitmap_utils::find_and_set_from(derived().words(), derived().nbits(), start);
  }

  /** @brief `find_and_set_from()` starting at bit 0. */
  [[nodiscard]] reloco::optional<std::size_t> find_and_set() noexcept {
    return bitmap_utils::find_and_set(derived().words(), derived().nbits());
  }

  // ---------------------------------------------------------------------------
  // Range Operations (FreeBSD `bitstring.h`-style)
  // ---------------------------------------------------------------------------

  /** @brief Sets every bit in `[start, stop]`. Traps if `start > stop` or `stop >= size()`. */
  void set_range(std::size_t start, std::size_t stop) noexcept {
    bitmap_utils::set_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief Fallible variant of `set_range()`. */
  result<void> try_set_range(std::size_t start, std::size_t stop) noexcept {
    return bitmap_utils::try_set_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief `set_range()` without range-checking; UB if `start > stop` or the range doesn't fit. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_set_range(std::size_t start, std::size_t stop) noexcept {
    bitmap_utils::unsafe_set_range(derived().words(), start, stop);
  }

  /** @brief Clears every bit in `[start, stop]`. Traps if `start > stop` or `stop >= size()`. */
  void clear_range(std::size_t start, std::size_t stop) noexcept {
    bitmap_utils::clear_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief Fallible variant of `clear_range()`. */
  result<void> try_clear_range(std::size_t start, std::size_t stop) noexcept {
    return bitmap_utils::try_clear_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief `clear_range()` without range-checking; UB if `start > stop` or the range doesn't fit. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_clear_range(std::size_t start, std::size_t stop) noexcept {
    bitmap_utils::unsafe_clear_range(derived().words(), start, stop);
  }

  /** @brief Number of set bits in `[start, stop]`. Traps if `start > stop` or `stop >= size()`. */
  [[nodiscard]] std::size_t count_range(std::size_t start, std::size_t stop) const noexcept {
    return bitmap_utils::count_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief Fallible variant of `count_range()`. */
  [[nodiscard]] result<std::size_t> try_count_range(std::size_t start, std::size_t stop) const noexcept {
    return bitmap_utils::try_count_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief `count_range()` without range-checking; UB if `start > stop` or the range doesn't fit. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE std::size_t unsafe_count_range(std::size_t start,
                                                                         std::size_t stop) const noexcept {
    return bitmap_utils::unsafe_count_range(derived().words(), start, stop);
  }

  /** @brief `true` if every bit in `[start, stop]` is set. Traps if `start > stop` or `stop >= size()`. */
  [[nodiscard]] bool all_set_in_range(std::size_t start, std::size_t stop) const noexcept {
    return bitmap_utils::all_set_in_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief Fallible variant of `all_set_in_range()`. */
  [[nodiscard]] result<bool> try_all_set_in_range(std::size_t start, std::size_t stop) const noexcept {
    return bitmap_utils::try_all_set_in_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief `all_set_in_range()` without range-checking; UB if `start > stop` or the range doesn't fit. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE bool unsafe_all_set_in_range(std::size_t start,
                                                                       std::size_t stop) const noexcept {
    return bitmap_utils::unsafe_all_set_in_range(derived().words(), start, stop);
  }

  /** @brief `true` if every bit in `[start, stop]` is clear. Traps if `start > stop` or `stop >= size()`. */
  [[nodiscard]] bool all_clear_in_range(std::size_t start, std::size_t stop) const noexcept {
    return bitmap_utils::all_clear_in_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief Fallible variant of `all_clear_in_range()`. */
  [[nodiscard]] result<bool> try_all_clear_in_range(std::size_t start, std::size_t stop) const noexcept {
    return bitmap_utils::try_all_clear_in_range(derived().words(), derived().nbits(), start, stop);
  }

  /** @brief `all_clear_in_range()` without range-checking; UB if `start > stop` or the range doesn't fit. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE bool unsafe_all_clear_in_range(std::size_t start,
                                                                         std::size_t stop) const noexcept {
    return bitmap_utils::unsafe_all_clear_in_range(derived().words(), start, stop);
  }

  /** @brief Lowest index at or after `start` where `size` consecutive CLEAR bits begin, if any. */
  [[nodiscard]] reloco::optional<std::size_t> lowest_clear_run_from(std::size_t start, std::size_t size) const noexcept {
    return bitmap_utils::lowest_clear_run_from(derived().words(), derived().nbits(), start, size);
  }

  /** @brief `lowest_clear_run_from()` starting at bit 0. */
  [[nodiscard]] reloco::optional<std::size_t> lowest_clear_run(std::size_t size) const noexcept {
    return bitmap_utils::lowest_clear_run(derived().words(), derived().nbits(), size);
  }

  /** @brief Lowest index at or after `start` where `size` consecutive SET bits begin, if any. */
  [[nodiscard]] reloco::optional<std::size_t> lowest_set_run_from(std::size_t start, std::size_t size) const noexcept {
    return bitmap_utils::lowest_set_run_from(derived().words(), derived().nbits(), start, size);
  }

  /** @brief `lowest_set_run_from()` starting at bit 0. */
  [[nodiscard]] reloco::optional<std::size_t> lowest_set_run(std::size_t size) const noexcept {
    return bitmap_utils::lowest_set_run(derived().words(), derived().nbits(), size);
  }

  /**
   * @brief Finds the lowest run of `size` consecutive clear bits at or
   * after `start` and sets all of them -- the contiguous-run counterpart
   * to `find_and_set_from()`.
   */
  [[nodiscard]] reloco::optional<std::size_t> find_and_set_run_from(std::size_t start, std::size_t size) noexcept {
    return bitmap_utils::find_and_set_run_from(derived().words(), derived().nbits(), start, size);
  }

  /** @brief `find_and_set_run_from()` starting at bit 0. */
  [[nodiscard]] reloco::optional<std::size_t> find_and_set_run(std::size_t size) noexcept {
    return bitmap_utils::find_and_set_run(derived().words(), derived().nbits(), size);
  }

  // ---------------------------------------------------------------------------
  // Atomic Operations (lock-free, GCC/Clang `__atomic_*` builtins)
  // ---------------------------------------------------------------------------

  /** @brief Atomically returns whether bit `index` is set. Traps if `index >= size()`. */
  [[nodiscard]] bool atomic_test(std::size_t index, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return bitmap_utils::atomic_test(derived().words(), derived().nbits(), index, order);
  }

  /** @brief Fallible variant of `atomic_test()`. */
  [[nodiscard]] result<bool> atomic_try_test(std::size_t index,
                                             std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return bitmap_utils::atomic_try_test(derived().words(), derived().nbits(), index, order);
  }

  /** @brief `atomic_test()` without range-checking; UB if `index` doesn't fit in the backing words. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE bool
  unsafe_atomic_test(std::size_t index, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return bitmap_utils::unsafe_atomic_test(derived().words(), index, order);
  }

  /** @brief Atomically sets bit `index`. Traps if `index >= size()`. */
  void atomic_set(std::size_t index, std::memory_order order = std::memory_order_seq_cst) noexcept {
    bitmap_utils::atomic_set(derived().words(), derived().nbits(), index, order);
  }

  /** @brief Fallible variant of `atomic_set()`. */
  result<void> atomic_try_set(std::size_t index, std::memory_order order = std::memory_order_seq_cst) noexcept {
    return bitmap_utils::atomic_try_set(derived().words(), derived().nbits(), index, order);
  }

  /** @brief `atomic_set()` without range-checking; UB if `index` doesn't fit in the backing words. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_atomic_set(std::size_t index,
                                                    std::memory_order order = std::memory_order_seq_cst) noexcept {
    bitmap_utils::unsafe_atomic_set(derived().words(), index, order);
  }

  /** @brief Atomically clears bit `index`. Traps if `index >= size()`. */
  void atomic_clear(std::size_t index, std::memory_order order = std::memory_order_seq_cst) noexcept {
    bitmap_utils::atomic_clear(derived().words(), derived().nbits(), index, order);
  }

  /** @brief Fallible variant of `atomic_clear()`. */
  result<void> atomic_try_clear(std::size_t index, std::memory_order order = std::memory_order_seq_cst) noexcept {
    return bitmap_utils::atomic_try_clear(derived().words(), derived().nbits(), index, order);
  }

  /** @brief `atomic_clear()` without range-checking; UB if `index` doesn't fit in the backing words. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_atomic_clear(std::size_t index,
                                                      std::memory_order order = std::memory_order_seq_cst) noexcept {
    bitmap_utils::unsafe_atomic_clear(derived().words(), index, order);
  }

  /** @brief Atomically flips bit `index`. Traps if `index >= size()`. */
  void atomic_toggle(std::size_t index, std::memory_order order = std::memory_order_seq_cst) noexcept {
    bitmap_utils::atomic_toggle(derived().words(), derived().nbits(), index, order);
  }

  /** @brief Fallible variant of `atomic_toggle()`. */
  result<void> atomic_try_toggle(std::size_t index, std::memory_order order = std::memory_order_seq_cst) noexcept {
    return bitmap_utils::atomic_try_toggle(derived().words(), derived().nbits(), index, order);
  }

  /** @brief `atomic_toggle()` without range-checking; UB if `index` doesn't fit in the backing words. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_atomic_toggle(std::size_t index,
                                                       std::memory_order order = std::memory_order_seq_cst) noexcept {
    bitmap_utils::unsafe_atomic_toggle(derived().words(), index, order);
  }

  /** @brief Atomically sets bit `index`, returning its PREVIOUS value. UB if `index` doesn't fit in the backing words. */
  RELOCO_UNSAFE_BUFFER_USAGE bool
  unsafe_atomic_test_and_set(std::size_t index, std::memory_order order = std::memory_order_seq_cst) noexcept {
    return bitmap_utils::unsafe_atomic_test_and_set(derived().words(), index, order);
  }

  /** @brief Atomically clears bit `index`, returning its PREVIOUS value. UB if `index` doesn't fit in the backing words. */
  RELOCO_UNSAFE_BUFFER_USAGE bool
  unsafe_atomic_test_and_clear(std::size_t index, std::memory_order order = std::memory_order_seq_cst) noexcept {
    return bitmap_utils::unsafe_atomic_test_and_clear(derived().words(), index, order);
  }

  /** @brief Atomically flips bit `index`, returning its PREVIOUS value. UB if `index` doesn't fit in the backing words. */
  RELOCO_UNSAFE_BUFFER_USAGE bool
  unsafe_atomic_test_and_toggle(std::size_t index, std::memory_order order = std::memory_order_seq_cst) noexcept {
    return bitmap_utils::unsafe_atomic_test_and_toggle(derived().words(), index, order);
  }

  /**
   * @brief Atomically finds the lowest set bit at or after `start`, if
   * any. See `bitmap_utils::atomic_lowest_set_from()`.
   */
  [[nodiscard]] reloco::optional<std::size_t>
  atomic_lowest_set_from(std::size_t start, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return bitmap_utils::atomic_lowest_set_from(derived().words(), derived().nbits(), start, order);
  }

  /** @brief Atomically finds the lowest set bit, if any. */
  [[nodiscard]] reloco::optional<std::size_t>
  atomic_lowest_set(std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return bitmap_utils::atomic_lowest_set(derived().words(), derived().nbits(), order);
  }

  /**
   * @brief Atomically finds the lowest CLEAR bit at or after `start` and
   * sets it, returning that index. See `bitmap_utils::atomic_find_and_set_from()`.
   */
  [[nodiscard]] reloco::optional<std::size_t>
  atomic_find_and_set_from(std::size_t start, std::memory_order order = std::memory_order_seq_cst) noexcept {
    return bitmap_utils::atomic_find_and_set_from(derived().words(), derived().nbits(), start, order);
  }

  /** @brief Atomically finds and sets the lowest clear bit, if any. */
  [[nodiscard]] reloco::optional<std::size_t>
  atomic_find_and_set(std::memory_order order = std::memory_order_seq_cst) noexcept {
    return bitmap_utils::atomic_find_and_set(derived().words(), derived().nbits(), order);
  }

private:
  [[nodiscard]] Derived &derived() noexcept { return static_cast<Derived &>(*this); }
  [[nodiscard]] const Derived &derived() const noexcept { return static_cast<const Derived &>(*this); }
};

} // namespace structo
