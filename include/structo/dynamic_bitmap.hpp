// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file dynamic_bitmap.hpp
 * @brief `structo::dynamic_bitmap`: a runtime-sized bitmap managing its
 * own heap-allocated `unsigned long[]` buffer directly through a
 * `reloco::allocator_ref`, rather than composing on top of
 * `reloco::vector<unsigned long>` -- there is no growth, no element
 * construction/destruction to track (`unsigned long` is trivial), and no
 * capacity-vs-size distinction a bitmap ever needs, so a bitmap-shaped
 * problem does not need a general-purpose growable-container solution.
 * This mirrors `reloco::heap_spsc_ring_buffer<T>`'s own "store an
 * `allocator_ref`, raw `allocate()`/`deallocate()` in a private
 * `try_init()`/destructor pair" shape more closely than it mirrors
 * `arch::asid_allocator`'s `vector`-backed one.
 *
 * Like `reloco::vector`/`arch::asid_allocator`, construction is fallible
 * (`try_allocate`/`try_create`, per `reloco`'s fallible-construction
 * convention: see `reloco/docs/fallible-construction.md`) and the type
 * is move-only: moving transfers the owned buffer (and nulls out the
 * moved-from source so its destructor is a no-op); copying is `delete`d
 * since cloning a bitmap of unknown provenance is rarely what a caller
 * actually wants silently -- use `try_clone()` to request it explicitly.
 *
 * `nbits` is a runtime constructor argument (not a template parameter),
 * for callers that don't know the bit count until runtime (e.g. a page
 * count computed from a boot-time-probed RAM size) -- the same rationale
 * `arch::asid_allocator` gives for its own runtime-sized ASID bitmap.
 *
 * @code
 * auto maker = structo::dynamic_bitmap::try_create(num_pages);
 * if (!maker) { panic("out of memory sizing the page bitmap"); }
 * auto pages = std::move(maker.value());
 *
 * auto slot = pages.find_and_set(); // claim the lowest free page
 * if (slot) {
 *   use_page(*slot);
 *   pages.clear(*slot);
 * }
 * @endcode
 */

#include "bitmap_ops.hpp"
#include "bitmap_utils.hpp"

#include <cstddef>
#include <reloco/allocator.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>
#include <utility>

namespace structo {

using namespace reloco;

/**
 * @brief Runtime-sized bitmap managing its own heap-allocated
 * `unsigned long[]` buffer. Move-only; see the @file-level docs.
 */
class dynamic_bitmap : public bitmap_ops<dynamic_bitmap> {
public:
  /** @brief Constructs an empty (`nbits() == 0`), storage-less bitmap bound to `alloc`. Never fails. */
  constexpr explicit dynamic_bitmap(allocator_ref alloc = default_allocator()) noexcept : alloc_(alloc) {}

  dynamic_bitmap(dynamic_bitmap &&other) noexcept
      : alloc_(other.alloc_), words_(other.words_), word_count_(other.word_count_), nbits_(other.nbits_) {
    other.words_ = nullptr;
    other.word_count_ = 0;
    other.nbits_ = 0;
  }

  dynamic_bitmap &operator=(dynamic_bitmap &&other) noexcept {
    if (this != &other) {
      release();
      alloc_ = other.alloc_;
      words_ = other.words_;
      word_count_ = other.word_count_;
      nbits_ = other.nbits_;
      other.words_ = nullptr;
      other.word_count_ = 0;
      other.nbits_ = 0;
    }
    return *this;
  }

  // Use `try_clone()`/`try_clone(allocator_ref)` for an explicit, fallible copy.
  dynamic_bitmap(const dynamic_bitmap &) = delete;
  dynamic_bitmap &operator=(const dynamic_bitmap &) = delete;

  ~dynamic_bitmap() noexcept { release(); }

  /**
   * @brief Fallible allocation factory.
   * @param alloc Allocator backing the bitmap's storage.
   * @param nbits Logical bit count.
   * @return The bitmap (all bits clear), or `error::allocation_failed` if `alloc` could not provide the backing
   * storage. `nbits == 0` always succeeds without allocating.
   */
  [[nodiscard]] static result<dynamic_bitmap> try_allocate(allocator_ref alloc, std::size_t nbits) noexcept {
    dynamic_bitmap bm(alloc);
    const std::size_t wc = bitmap_utils::word_count_for(nbits);
    if (wc == 0) {
      return bm;
    }

    auto res = alloc.allocate(wc * sizeof(unsigned long), alignof(unsigned long));
    if (!res) {
      return unexpected(res.error());
    }

    bm.words_ = static_cast<unsigned long *>(res->ptr);
    bm.word_count_ = wc;
    bm.nbits_ = nbits;
    bm.clear_all();
    return bm;
  }

  /** @brief `try_allocate()` using `reloco::default_allocator()`. */
  [[nodiscard]] static result<dynamic_bitmap> try_create(std::size_t nbits) noexcept {
    return try_allocate(default_allocator(), nbits);
  }

  /** @brief Fallible explicit copy, using `alloc` for the clone's storage. */
  [[nodiscard]] result<dynamic_bitmap> try_clone(allocator_ref alloc) const noexcept {
    auto res = try_allocate(alloc, nbits_);
    if (!res) {
      return res;
    }
    bitmap_utils::atomic_snapshot(words(), res->words(), std::memory_order_relaxed);
    return res;
  }

  /** @brief `try_clone()` using this bitmap's own allocator. */
  [[nodiscard]] result<dynamic_bitmap> try_clone() const noexcept { return try_clone(alloc_); }

  // ---- `bitmap_ops<Derived>` contract ----

  [[nodiscard]] span<unsigned long> words() & noexcept { return span<unsigned long>(words_, word_count_); }
  [[nodiscard]] span<const unsigned long> words() const & noexcept {
    return span<const unsigned long>(words_, word_count_);
  }
  [[nodiscard]] std::size_t nbits() const noexcept { return nbits_; }

private:
  void release() noexcept {
    if (words_ != nullptr) {
      alloc_.deallocate(words_, word_count_ * sizeof(unsigned long));
      words_ = nullptr;
      word_count_ = 0;
      nbits_ = 0;
    }
  }

  allocator_ref alloc_;
  unsigned long *words_ = nullptr;
  std::size_t word_count_ = 0;
  std::size_t nbits_ = 0;
};

} // namespace structo
