// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <reloco/array.hpp>
#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <reloco/intrusive_c_tailq.hpp>
#include <reloco/lifetime.hpp>

namespace structo {

using namespace reloco;

/**
 * @brief ARCHETYPE: The expected interface for the FreeList template parameter.
 *
 * Your custom list does NOT need to inherit from this. It just needs to
 * provide methods with matching signatures.
 *
 * @tparam OsPage The legacy pointer or compressed handle type.
 */
template <typename OsPage> struct free_list_archetype {
  // Must provide an iterator that supports !=, *, and prefix ++
  struct iterator {
    bool operator!=(const iterator &other) const noexcept;
    OsPage operator*() const noexcept;
    iterator &operator++() noexcept;
  };

  // State Management
  void clear() & noexcept;
  [[nodiscard]] bool empty() const & noexcept;

  // Intrusive Operations
  void push_front(OsPage p) & noexcept;
  void remove(OsPage p) & noexcept;
  OsPage pop_front() & noexcept;

  // Iteration
  [[nodiscard]] iterator begin() const & noexcept;
  [[nodiscard]] iterator end() const & noexcept;
};

/**
 * @brief Ready-to-use `FreeList` adapter bridging `buddy_allocator`'s
 * pointer-handle convention (`os_page_type = T *`, every
 * `free_list_archetype` operation taking/returning a bare pointer) onto
 * `reloco::c_tailq<T, Hook>` -- reloco's own `TAILQ`-style intrusive
 * list, and representative of how "typical" hand-rolled intrusive
 * lists are shaped: `push_front()`/`remove()` take the node *by
 * reference* (`T &`), not by pointer, and its iterator dereferences to
 * `T &` as well. Only `pop_front()` returns a nullable `T *`, there
 * being no other way to represent "the list is empty".
 *
 * This adapter exists purely to translate at that boundary: it
 * dereferences the incoming `T *` once before forwarding to
 * `reloco::c_tailq`, and reports positions back out as `T *` by asking
 * the underlying iterator for the node pointer it already holds.
 *
 * @tparam T The node type carrying the intrusive hook (e.g. a page
 * descriptor struct with a `TAILQ_ENTRY`-equivalent member).
 * @tparam Hook Pointer-to-member of `T`'s hook field, exactly as
 * `reloco::c_tailq<T, Hook>` itself takes it.
 *
 * @code
 * struct page_meta {
 *   // ... other OS bookkeeping fields ...
 *   struct {
 *     page_meta *next = nullptr;
 *     page_meta **prev = nullptr;
 *   } link;
 * };
 *
 * using free_list = structo::tailq_free_list<page_meta, &page_meta::link>;
 * using allocator = structo::buddy_allocator<free_list, my_page_view, 11>;
 * @endcode
 */
template <typename T, auto Hook> class tailq_free_list {
public:
  using os_page_type = T *;

private:
  using tailq_type = reloco::c_tailq<T, Hook>;

public:
  /** @brief Forward iterator yielding `T *` (never a `T &`), matching `free_list_archetype`. */
  class iterator {
  public:
    iterator() noexcept = default;
    explicit iterator(typename tailq_type::const_iterator it) noexcept : it_(it) {}

    [[nodiscard]] bool operator!=(const iterator &other) const noexcept { return it_ != other.it_; }
    [[nodiscard]] os_page_type operator*() const noexcept { return it_.node(); }
    iterator &operator++() noexcept {
      ++it_;
      return *this;
    }

  private:
    typename tailq_type::const_iterator it_{};
  };

  void clear() & noexcept { list_.clear(); }
  [[nodiscard]] bool empty() const & noexcept { return list_.empty(); }

  void push_front(os_page_type p) & noexcept {
    RELOCO_ASSERT(p != nullptr, "tailq_free_list: push_front(nullptr)");
    list_.push_front(*p);
  }

  void remove(os_page_type p) & noexcept {
    RELOCO_ASSERT(p != nullptr, "tailq_free_list: remove(nullptr)");
    list_.remove(*p);
  }

  [[nodiscard]] os_page_type pop_front() & noexcept { return list_.pop_front(); }

  [[nodiscard]] iterator begin() const & noexcept { return iterator(list_.begin()); }
  [[nodiscard]] iterator end() const & noexcept { return iterator(list_.end()); }

private:
  tailq_type list_;
};

/**
 * @brief A completely decoupled, traits-driven Buddy Allocator.
 *
 * @tparam FreeList An intrusive list container providing:
 *         clear(), empty(), push_front(os_page_type),
 *         remove(os_page_type), pop_front() -> os_page_type.
 * @tparam PageView Strongly-typed reloco::page_view.
 * @tparam MaxOrder The maximum power-of-two order for block sizes.
 */
template <typename FreeList, typename PageView, size_t MaxOrder = 11> class buddy_allocator {
public:
  using free_list_type = FreeList;
  using page_type = PageView;
  using os_page_type = typename page_type::os_page_type;

  static constexpr size_t MAX_ORDER = MaxOrder;

  constexpr buddy_allocator() noexcept = default;

  /**
   * @brief Initializes the allocator, carving the memory region into
   * maximally aligned power-of-two blocks.
   */
  [[nodiscard]] result<void> init(page_type start, size_t num_pages) noexcept {
    if (start.is_null() || num_pages == 0) {
      return unexpected(error::invalid_argument);
    }

    for (size_t i = 0; i <= MaxOrder; i++) {
      free_areas_[i].clear();
    }

    page_type current = start;
    size_t remaining = num_pages;

    while (remaining > 0) {
      size_t order = MaxOrder;

      // Find the largest properly-aligned order that fits
      while (order > 0) {
        size_t block_pages = 1ULL << order;
        bool is_aligned = (current.pfn() % block_pages) == 0;
        bool fits = (remaining >= block_pages);

        if (is_aligned && fits) {
          break;
        }
        order--;
      }

      if (order > 0) {
        auto zone_check = current.try_add((1ULL << order) - 1);
        if (!zone_check) {
          return unexpected(zone_check.error());
        }
      }

      current.set_buddy_order(static_cast<uint16_t>(order));
      current.set_buddy_free(true);
      free_areas_[order].push_front(current.get_os_page());

      remaining -= (1ULL << order);

      if (remaining > 0) {
        auto next_res = current.try_add(1ULL << order);
        if (!next_res)
          return unexpected(next_res.error());
        current = *next_res;
      }
    }

    return {};
  }

  /**
   * @brief Allocates a contiguous physical block of 2^order pages.
   */
  [[nodiscard]] result<page_type> allocate(size_t order) noexcept {
    if (order > MaxOrder) {
      return unexpected(error::invalid_argument);
    }

    size_t current_order = order;
    while (current_order <= MaxOrder && free_areas_[current_order].empty()) {
      current_order++;
    }

    if (current_order > MaxOrder) {
      return unexpected(error::allocation_failed);
    }

    // Pop the block and mark it as allocated
    page_type block = page_type::from_os_page(free_areas_[current_order].pop_front());
    block.set_buddy_free(false);

    // Split the block down to the requested size
    while (current_order > order) {
      current_order--;

      auto buddy_res = block.try_add(1ULL << current_order);
      if (!buddy_res)
        return unexpected(buddy_res.error()); // Unreachable if OS layout is sane

      page_type buddy = *buddy_res;
      buddy.set_buddy_order(static_cast<uint16_t>(current_order));
      buddy.set_buddy_free(true);
      free_areas_[current_order].push_front(buddy.get_os_page());
    }

    block.set_buddy_order(static_cast<uint16_t>(order));
    return block;
  }

  /**
   * @brief Frees a block, automatically coalescing with its buddies if possible.
   */
  void free(page_type p, size_t order) noexcept {
    // In a kernel, freeing invalid memory is a fatal bug. Panic immediately.
    RELOCO_ASSERT(!p.is_null(), "buddy_allocator: Attempted to free a null page");
    RELOCO_ASSERT(order <= MaxOrder, "buddy_allocator: Invalid order passed to free");

    while (order < MaxOrder) {
      // Safely calculate mathematical buddy (bounds & zone verified by PageView)
      auto buddy_res = p.try_get_buddy(static_cast<uint16_t>(order));
      if (!buddy_res) {
        break; // Buddy spans across a zone boundary or out of memory bounds
      }

      page_type buddy = *buddy_res;

      // Ensure buddy is entirely free and of the exact matching size
      if (!buddy.is_buddy_free() || buddy.buddy_order() != order) {
        break;
      }

      // Merge them
      free_areas_[order].remove(buddy.get_os_page());
      buddy.set_buddy_free(false);

      if (buddy.pfn() < p.pfn()) {
        p = buddy;
      }
      order++;
    }

    p.set_buddy_order(static_cast<uint16_t>(order));
    p.set_buddy_free(true);
    free_areas_[order].push_front(p.get_os_page());
  }

  /**
   * @brief Exact page allocation via Binary Decomposition.
   * Eliminates internal fragmentation by returning the unused tail back to the free list.
   */
  [[nodiscard]] result<page_type> allocate_n(size_t num_pages) noexcept {
    if (num_pages == 0)
      return unexpected(error::invalid_argument);

    auto order_res = pages_to_order(num_pages);
    if (!order_res)
      return unexpected(order_res.error());
    size_t order = *order_res;

    // Find the smallest available block >= requested size
    size_t current_order = order;
    while (current_order <= MaxOrder && free_areas_[current_order].empty()) {
      current_order++;
    }
    if (current_order > MaxOrder)
      return unexpected(error::allocation_failed);

    page_type block = page_type::from_os_page(free_areas_[current_order].pop_front());
    block.set_buddy_free(false);

    // Standard split down to the bounding 'order'
    while (current_order > order) {
      current_order--;
      auto buddy_res = block.try_add(1ULL << current_order);
      if (!buddy_res)
        return unexpected(buddy_res.error());

      page_type buddy = *buddy_res;
      buddy.set_buddy_order(static_cast<uint16_t>(current_order));
      buddy.set_buddy_free(true);
      free_areas_[current_order].push_front(buddy.get_os_page());
    }

    // Exact Page Splitting: Trim the tail!
    size_t remaining_needed = num_pages;
    page_type current_chunk = block;
    size_t chunk_order = order;

    while (remaining_needed > 0 && chunk_order > 0) {
      chunk_order--;
      size_t half_size = 1ULL << chunk_order;

      auto buddy_res = current_chunk.try_add(half_size);
      if (!buddy_res)
        return unexpected(buddy_res.error());

      page_type buddy = *buddy_res;

      if (remaining_needed <= half_size) {
        // We only need the first half. Free the second half back to the system.
        buddy.set_buddy_order(static_cast<uint16_t>(chunk_order));
        buddy.set_buddy_free(true);
        free_areas_[chunk_order].push_front(buddy.get_os_page());
      } else {
        // The first half is fully consumed. Mark it, and shift focus to the second half.
        current_chunk.set_buddy_order(static_cast<uint16_t>(chunk_order));
        current_chunk.set_buddy_free(false);

        remaining_needed -= half_size;
        current_chunk = buddy;
      }
    }

    if (remaining_needed > 0) {
      current_chunk.set_buddy_order(static_cast<uint16_t>(chunk_order));
      current_chunk.set_buddy_free(false);
    }

    return block;
  }

  /**
   * @brief Exact page freeing.
   * Shreds an arbitrary contiguous range back into the largest possible
   * aligned power-of-two chunks, returning them to the buddy system.
   */
  void free_n(page_type p, size_t num_pages) noexcept {
    RELOCO_ASSERT(!p.is_null(), "buddy_allocator: Attempted to free_n a null page");
    RELOCO_ASSERT(num_pages > 0, "buddy_allocator: Attempted to free_n 0 pages");

    page_type current = p;
    size_t remaining = num_pages;

    while (remaining > 0) {
      size_t order = 0;

      // Find the maximum order that fits in the remaining space AND
      // strictly maintains natural power-of-two physical alignment.
      while (order < MaxOrder) {
        size_t next_order_pages = 1ULL << (order + 1);

        // Must fit in the remaining requested pages
        if (next_order_pages > remaining)
          break;

        // The physical PFN MUST be a multiple of the next order's size
        if ((current.pfn() & (next_order_pages - 1)) != 0)
          break;

        order++;
      }

      free(current, order);

      remaining -= (1ULL << order);

      if (remaining > 0) {
        auto next_res = current.try_add(1ULL << order);
        RELOCO_ASSERT(next_res.has_value(), "buddy_allocator: free_n crossed illegal zone boundary");
        current = *next_res;
      }
    }
  }

  /**
   * @brief "Un-frees" one specific page the allocator currently
   * considers free, splitting its containing free block as needed and
   * marking only that exact page allocated -- every sibling half
   * produced by the split is returned to the free lists untouched.
   *
   * Unlike `allocate()`/`allocate_n()`/`allocate_constrained()`, which
   * all let the allocator pick *which* physical pages to hand back,
   * `reserve()` lets the caller demand one specific, already-known
   * physical page. This is the common need when a page is discovered
   * to already be in use strictly *after* `init()` has already carved
   * the whole region into free blocks assuming it was available --
   * e.g. a firmware/bootloader-reserved region only enumerated from
   * ACPI/UEFI tables once the buddy allocator for the whole zone has
   * already been built from a coarser memory map.
   *
   * @return `error::invalid_state` if @p p is not currently free --
   * either because it is already allocated/reserved, or because it
   * does not lie within any block this allocator currently tracks at
   * all (e.g. outside the managed region, or never passed to `init()`).
   */
  [[nodiscard]] result<void> reserve(page_type p) noexcept {
    RELOCO_ASSERT(!p.is_null(), "buddy_allocator: Attempted to reserve a null page");

    size_t order = MaxOrder;
    while (true) {
      size_t block_pages = 1ULL << order;

      for (auto os_page : free_areas_[order]) {
        page_type block = page_type::from_os_page(os_page);
        uint64_t block_pfn = block.pfn();

        if (p.pfn() < block_pfn || p.pfn() >= block_pfn + block_pages) {
          continue; // `p` is not inside this free block
        }

        // Found the free block containing `p`. Pull it off the free
        // list whole, then split down one level at a time: whichever
        // half does *not* contain `p` goes straight back to the free
        // list at its own (smaller) order, and the half that *does*
        // contain `p` is narrowed into on the next iteration.
        free_areas_[order].remove(block.get_os_page());
        block.set_buddy_free(false);

        page_type current = block;
        size_t current_order = order;
        while (current_order > 0) {
          current_order--;
          size_t half_pages = 1ULL << current_order;

          auto buddy_res = current.try_add(half_pages);
          RELOCO_ASSERT(buddy_res.has_value(), "buddy_allocator: reserve split crossed illegal zone boundary");
          page_type buddy = *buddy_res;

          if (p.pfn() < buddy.pfn()) {
            // `p` is in the lower half; the upper half is untouched.
            buddy.set_buddy_order(static_cast<uint16_t>(current_order));
            buddy.set_buddy_free(true);
            free_areas_[current_order].push_front(buddy.get_os_page());
          } else {
            // `p` is in the upper half; the lower half is untouched.
            current.set_buddy_order(static_cast<uint16_t>(current_order));
            current.set_buddy_free(true);
            free_areas_[current_order].push_front(current.get_os_page());
            current = buddy;
          }
        }

        current.set_buddy_order(0);
        current.set_buddy_free(false);
        return {};
      }

      if (order == 0)
        break;
      order--;
    }

    return unexpected(error::invalid_state);
  }

  /** @brief Physical constraints applied when selecting a page range. */
  struct physical_constraint {
    uint64_t low_pfn{0};         // Minimum acceptable PFN
    uint64_t high_pfn{~0ULL};    // Maximum acceptable PFN (inclusive)
    uint64_t alignment_pages{1}; // Must be a power of two
    uint64_t boundary_pages{0};  // 0 = no boundary restrictions. Otherwise power of two.
  };

  /**
   * @brief Allocates physical pages fulfilling strict hardware constraints (low, high, alignment, boundary).
   * Operates similarly to FreeBSD's vm_page_alloc_contig().
   */
  [[nodiscard]] result<page_type> allocate_constrained(size_t num_pages, const physical_constraint &c) noexcept {
    if (num_pages == 0 || c.alignment_pages == 0)
      return unexpected(error::invalid_argument);

    // We only need to check blocks large enough to potentially hold our constraints
    auto min_order_res = pages_to_order(num_pages);
    if (!min_order_res)
      return unexpected(min_order_res.error());

    for (size_t order = *min_order_res; order <= MaxOrder; order++) {
      // Iterate through the free list at this order
      for (auto os_page : free_areas_[order]) {
        page_type block = page_type::from_os_page(os_page);

        // Calculate the ideal aligned starting PFN within this block
        uint64_t block_pfn = block.pfn();
        uint64_t start_pfn = block_pfn;

        if (start_pfn < c.low_pfn) {
          start_pfn = c.low_pfn;
        }

        // Align up to requested alignment
        start_pfn = (start_pfn + c.alignment_pages - 1) & ~(c.alignment_pages - 1);

        // Check Boundary Crossing
        if (c.boundary_pages > 0) {
          uint64_t end_pfn = start_pfn + num_pages - 1;
          if ((start_pfn / c.boundary_pages) != (end_pfn / c.boundary_pages)) {
            // It crosses a boundary. Push start_pfn to the next boundary.
            start_pfn = (start_pfn + c.boundary_pages) & ~(c.boundary_pages - 1);
            // Re-apply alignment if the boundary push misaligned it
            start_pfn = (start_pfn + c.alignment_pages - 1) & ~(c.alignment_pages - 1);
          }
        }

        uint64_t end_pfn = start_pfn + num_pages - 1;
        uint64_t block_end_pfn = block_pfn + (1ULL << order) - 1;

        // Verify it still fits inside this buddy block and satisfies high_pfn
        if (end_pfn <= block_end_pfn && end_pfn <= c.high_pfn) {

          // WE FOUND A MATCH!
          // Remove this massive block from the free list
          free_areas_[order].remove(block.get_os_page());
          block.set_buddy_free(false);

          // Carve out the front padding and return it to the buddy system
          size_t front_padding = start_pfn - block_pfn;
          if (front_padding > 0) {
            // We use our exact page freeing logic to decompose the front padding!
            free_n(block, front_padding);
          }

          // Navigate to our actual starting page
          auto alloc_page = block.try_add(front_padding).value();

          // Carve out the back padding and return it to the buddy system
          size_t back_padding = (1ULL << order) - front_padding - num_pages;
          if (back_padding > 0) {
            auto back_page = alloc_page.try_add(num_pages).value();
            free_n(back_page, back_padding);
          }

          // Mark our specific exact allocation as consumed (using chunk_order 0 for exact slices)
          alloc_page.set_buddy_order(0);
          alloc_page.set_buddy_free(false);

          return alloc_page;
        }
      }
    }

    // No block in any order satisfies the hardware constraints
    return unexpected(error::allocation_failed);
  }

  /** @brief A page range returned by a successful buddy allocation. */
  struct range_allocation {
    page_type page;
    size_t count;
  };

  /**
   * @brief Opportunistic greedy allocation for vmalloc-like memory population.
   * Tries to allocate exactly `max_pages`. If memory is too fragmented,
   * falls back to returning the largest contiguous chunk available <= `max_pages`.
   */
  [[nodiscard]] result<range_allocation> allocate_up_to(size_t max_pages) noexcept {
    if (max_pages == 0)
      return unexpected(error::invalid_argument);

    // Try the optimal path: exact allocation
    auto exact_res = allocate_n(max_pages);
    if (exact_res) {
      return range_allocation{exact_res.value(), max_pages};
    }

    // If it failed for a reason other than fragmentation (e.g., bounds violation)
    if (exact_res.error() != error::allocation_failed) {
      return unexpected(exact_res.error());
    }

    // The Fallback Path: We are fragmented.
    // Find the highest power-of-two order strictly less than or equal to max_pages.
    size_t order = 0;
    while ((1ULL << (order + 1)) <= max_pages) {
      order++;
    }
    if (order > MaxOrder)
      order = MaxOrder;

    // Scan downwards for the largest surviving block
    while (true) {
      if (!free_areas_[order].empty()) {
        page_type block = page_type::from_os_page(free_areas_[order].pop_front());
        block.set_buddy_free(false);
        block.set_buddy_order(static_cast<uint16_t>(order));

        // We found a smaller, but perfectly contiguous power-of-two block!
        return range_allocation{block, 1ULL << order};
      }
      if (order == 0)
        break;
      order--;
    }

    // Truly out of memory
    return unexpected(error::allocation_failed);
  }

private:
  [[nodiscard]] static result<size_t> pages_to_order(size_t num_pages) noexcept {
    if (num_pages == 0)
      return unexpected(error::invalid_argument);

    size_t order = 0;
    while ((1ULL << order) < num_pages) {
      order++;
      if (order > MaxOrder)
        return unexpected(error::allocation_failed);
    }
    return order;
  }

  reloco::array<free_list_type, MaxOrder + 1> free_areas_{};
};

} // namespace structo