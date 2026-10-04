// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file bitmap_view.hpp
 * @brief `structo::bitmap_view`: a non-owning bitmap wrapper over a
 * caller-supplied `reloco::span<unsigned long>` + explicit `nbits`,
 * exposing the exact same `bitmap_ops<Derived>` instance-method surface
 * as `fixed_bitmap<N>`/`dynamic_bitmap` (`set`/`test`/`find_and_set`/
 * atomic variants/etc.) without copying or allocating anything -- the
 * generic counterpart to `cpu_mask::words()`'s own "decay to a view"
 * escape hatch, except usable as the primary handle rather than a
 * fallback.
 *
 * This is the type a caller reaches for when the backing words already
 * live somewhere else it doesn't own and can't extend the lifetime of --
 * e.g. a bitmap embedded inside a fixed-size on-the-wire/shared-memory
 * structure (a page-table-adjacent bookkeeping region mapped read-write
 * but owned by firmware/another privilege level), or a slice of a larger
 * `dynamic_bitmap`/`fixed_bitmap<N>` a subsystem is only supposed to see
 * part of.
 *
 * `bitmap_view` is **non-copyable and non-movable**, unlike a typical
 * cheap-to-copy C++ "view" type (e.g. `reloco::span` itself): copying or
 * moving a `bitmap_view` would silently produce a second handle (or
 * relocate the only handle) to memory the view itself has no stake in
 * keeping alive or exclusive, inviting exactly the dangling-view and
 * ambiguous-concurrent-aliasing bugs a view type exists to avoid framing
 * as "just data" in the first place -- the same reasoning
 * `arch::ipi_dispatcher.hpp`'s `ipi_message` gives for deleting all four
 * special members: a type that is *only* ever meaningful at the
 * particular address/scope it was constructed at should not be
 * copyable-or-movable-into-looking-like-a-value. Construct a fresh
 * `bitmap_view` at each scope that needs one instead of trying to hand
 * an existing one around.
 *
 * @code
 * void scan_segment(reloco::span<unsigned long> words, std::size_t nbits) {
 *   structo::bitmap_view view(words, nbits);
 *   auto free_slot = view.find_and_set();
 *   // ...
 * }
 * @endcode
 */

#include "bitmap_ops.hpp"
#include "bitmap_utils.hpp"

#include <cstddef>
#include <reloco/span.hpp>

namespace structo {

using namespace reloco;

/**
 * @brief Non-owning bitmap view over a caller-supplied
 * `span<unsigned long>` + explicit `nbits`. Non-copyable, non-movable;
 * see the @file-level docs for why.
 */
class bitmap_view : public bitmap_ops<bitmap_view> {
public:
  /**
   * @brief Wraps `words` as a bitmap of `nbits` logical bits.
   * @param words The backing storage; must stay alive and exclusively
   * accessed per whatever locking discipline the caller's use of this
   * view requires for at least as long as this `bitmap_view` is used.
   * @param nbits Logical bit count; must be `<= words.size() * bitmap_utils::bits_per_word`.
   */
  constexpr bitmap_view(span<unsigned long> words, std::size_t nbits) noexcept : words_(words), nbits_(nbits) {}

  // Non-copyable, non-movable: see the @file-level docs.
  bitmap_view(const bitmap_view &) = delete;
  bitmap_view &operator=(const bitmap_view &) = delete;
  bitmap_view(bitmap_view &&) = delete;
  bitmap_view &operator=(bitmap_view &&) = delete;

  // ---- `bitmap_ops<Derived>` contract ----

  [[nodiscard]] span<unsigned long> words() & noexcept { return words_; }
  [[nodiscard]] span<const unsigned long> words() const & noexcept { return words_; }
  [[nodiscard]] std::size_t nbits() const noexcept { return nbits_; }

private:
  span<unsigned long> words_;
  std::size_t nbits_;
};

} // namespace structo
