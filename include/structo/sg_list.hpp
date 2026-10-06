// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "phys_addr.hpp"
#include <cstddef>
#include <reloco/concepts.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/type_id.hpp>
#include <reloco/vector.hpp>
#include <type_traits>

namespace structo {

using namespace reloco;

/**
 * @brief A single scatter-gather descriptor.
 */
template <typename SpaceTag = dma_bus_space, typename PhysInt = std::uint64_t> struct sg_entry {
  using space_tag = SpaceTag;
  using phys_int = PhysInt;

  phys_addr<void, SpaceTag, PhysInt> addr;
  PhysInt length{0};
};

/**
 * @brief A Scatter-Gather List adapter that wraps any vector-like container.
 *
 * Provides automatic physical address coalescing on insertion.
 * Compatible with reloco::vector, reloco::inplace_vector, or std::vector.
 *
 * @tparam Container A vector containing `sg_entry` elements.
 */
template <typename Container = vector<sg_entry<>>> class sg_list {
public:
  using container_type = Container;
  using entry_type = typename Container::value_type;
  using space_tag = typename entry_type::space_tag;
  using phys_int = typename entry_type::phys_int;

  using iterator = typename Container::iterator;
  using const_iterator = typename Container::const_iterator;

  constexpr sg_list() = default;
  constexpr explicit sg_list(Container &&c) noexcept : c_(std::move(c)) {}

  /**
   * @brief Constructs an empty list using an explicit allocator. Only
   * participates in overload resolution when `Container` itself accepts
   * one (i.e. `reloco::vector`/`reloco::inline_vector`, not `std::vector`).
   */
  template <typename C = Container, typename = std::enable_if_t<std::is_constructible_v<C, allocator_ref>>>
  constexpr explicit sg_list(allocator_ref alloc) noexcept : c_(alloc) {}

  /**
   * @brief Builds, optionally reserving capacity, using an explicit
   * allocator. Only available when `Container::try_allocate` exists.
   */
  template <typename C = Container, typename = std::enable_if_t<has_try_allocate_v<C, size_t>>>
  [[nodiscard]] static result<sg_list> try_allocate(allocator_ref alloc, size_t initial_cap = 0) noexcept {
    auto container_res = Container::try_allocate(alloc, initial_cap);
    if (!container_res)
      return unexpected(container_res.error());
    return sg_list(std::move(*container_res));
  }

  /**
   * @brief Builds, optionally reserving capacity, using
   * `default_allocator()`. Only available when `Container::try_create`
   * exists.
   */
  template <typename C = Container, typename = std::enable_if_t<has_try_create_v<C, size_t>>>
  [[nodiscard]] static result<sg_list> try_create(size_t initial_cap = 0) noexcept {
    return try_allocate(default_allocator(), initial_cap);
  }

  /**
   * @brief Performs a deep copy using a specific allocator. Only available
   * when `Container::try_clone(allocator_ref)` exists.
   */
  template <typename C = Container, typename = std::enable_if_t<has_try_clone_allocator_aware_v<C>>>
  [[nodiscard]] result<sg_list> try_clone(allocator_ref alloc) const noexcept {
    auto cloned = c_.try_clone(alloc);
    if (!cloned)
      return unexpected(cloned.error());
    return sg_list(std::move(*cloned));
  }

  /**
   * @brief Performs a deep copy, delegating to `Container::try_clone()`
   * for the allocator choice (the container's own bound allocator for
   * `reloco::vector`, `default_allocator()` for `reloco::inline_vector`).
   * Only available when `Container::try_clone()` exists.
   */
  template <typename C = Container, typename = std::enable_if_t<has_try_clone_self_contained_v<C>>>
  [[nodiscard]] result<sg_list> try_clone() const noexcept {
    auto cloned = c_.try_clone();
    if (!cloned)
      return unexpected(cloned.error());
    return sg_list(std::move(*cloned));
  }

  /**
   * @brief Returns the bound allocator. Only available when `Container`
   * itself exposes one (i.e. `reloco::vector`/`reloco::inline_vector`, not
   * `std::vector`).
   */
  template <typename C = Container>
  [[nodiscard]] auto get_allocator() const noexcept -> decltype(std::declval<const C &>().get_allocator()) {
    return c_.get_allocator();
  }

  [[nodiscard]] bool empty() const noexcept { return c_.empty(); }
  [[nodiscard]] size_t size() const noexcept { return c_.size(); }

  // Conditionally expose capacity if the underlying container has it
  template <typename C = Container, typename = decltype(std::declval<C>().capacity())>
  [[nodiscard]] size_t capacity() const noexcept {
    return c_.capacity();
  }

  [[nodiscard]] iterator begin() & noexcept { return c_.begin(); }
  [[nodiscard]] iterator end() & noexcept { return c_.end(); }
  [[nodiscard]] const_iterator begin() const & noexcept { return c_.begin(); }
  [[nodiscard]] const_iterator end() const & noexcept { return c_.end(); }

  void clear() & noexcept { c_.clear(); }

  /**
   * @brief Appends a memory region. Coalesces if physically contiguous.
   */
  result<void> try_push_back(phys_addr<void, space_tag, phys_int> paddr, phys_int length) & noexcept {
    if (length == 0)
      return {};
    if (paddr.is_null())
      return unexpected(error::invalid_argument);

    // Attempt to coalesce with the existing tail
    if (!c_.empty()) {
      entry_type &tail = c_.back();
      if (tail.addr.value + tail.length == paddr.value) {
        // Prevent integer overflow on length
        if (~phys_int(0) - tail.length < length) {
          return unexpected(error::invalid_argument);
        }
        tail.length += length;
        return {};
      }
    }

    // Assuming the underlying vector provides a standard push_back
    return c_.try_push_back({paddr, length});
  }

  /** @brief Borrows the underlying container directly (e.g. for capacity queries the adapter does not expose). */
  [[nodiscard]] Container &base() & noexcept RELOCO_LIFETIMEBOUND { return c_; }
  /** @copydoc base() & */
  [[nodiscard]] const Container &base() const & noexcept RELOCO_LIFETIMEBOUND { return c_; }

private:
  Container c_;
};

/**
 * @brief Single-pass pull cursor splitting a type-erased `sg_list`'s
 * entries into caller-sized chunks on demand, for use cases such as
 * remapping memory where the maximum chunk size (e.g. how much can be
 * mapped as one segment at the destination) is a runtime decision that
 * can vary from call to call, not a single fixed limit chosen up front.
 *
 * Only `PhysInt` (the raw address/length integer width) remains a
 * template parameter; the originating `sg_list`'s `Container` and
 * `SpaceTag` are both erased at construction. This lets a cursor be
 * handed to a low-level memory mapper that accepts chunks for several
 * different address spaces (e.g. DMA-bus vs. CPU-physical) without that
 * mapper needing to be templated on the list's concrete `Container`
 * type; the mapper instead calls `holds<SpaceTag>()` to validate, at
 * runtime, which address space the cursor actually carries before
 * trusting `next_up_to<SpaceTag>()`'s output -- mirroring
 * `reloco::any`'s `is<T>()`/`try_get<T>()` type-erasure idiom.
 *
 * Exposes no `begin()`/`end()` and no Rust-style `.iter()`: driven purely
 * by repeated `next_up_to<SpaceTag>(max_length)` calls, each returning
 * the next physically-contiguous chunk (never crossing an original
 * entry's boundary, since a chunk's address range must remain
 * contiguous) of at most `max_length` bytes -- possibly shorter, both
 * because the remainder of the current entry may be smaller than
 * `max_length` and because a chunk is never split across two different
 * original entries.
 *
 * Holds only a type-erased pointer back into its `sg_list`, a function
 * pointer bound to that list's concrete `Container` type, the erased
 * entry type's `reloco::type_id`, and a small amount of plain cursor
 * state (current entry index + consumed-so-far offset within it) -- all
 * trivially copyable, so a caller needing more than one independent pass
 * over the same list can simply copy the cursor before advancing it,
 * rather than re-deriving one from the list.
 *
 * @tparam PhysInt The raw address/length integer width; must match the
 * originating `sg_list`'s own `phys_int`.
 */
template <typename PhysInt = std::uint64_t> class sg_list_cursor {
public:
  using phys_int = PhysInt;

  /**
   * @brief Binds to `list`, erasing its `Container` and `SpaceTag`;
   * `list` must outlive this cursor.
   */
  template <typename Container>
  constexpr explicit sg_list_cursor(
      const sg_list<Container> &list RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : list_(&list), advance_(&advance<Container>),
        entry_type_(type_id::of<typename sg_list<Container>::entry_type>()) {
    static_assert(std::is_same_v<typename sg_list<Container>::phys_int, PhysInt>,
                  "sg_list_cursor<PhysInt> requires a matching sg_list::phys_int");
  }

  /** @brief Whether this cursor was built from an `sg_list` whose entries use `SpaceTag`. */
  template <typename SpaceTag> [[nodiscard]] bool holds() const noexcept {
    return entry_type_ == type_id::of<sg_entry<SpaceTag, PhysInt>>();
  }

  /** @brief The erased `reloco::type_id` of the `sg_entry` specialization this cursor was built from. */
  [[nodiscard]] type_id entry_type_id() const noexcept { return entry_type_; }

  /**
   * @brief Returns the next chunk, at most `max_length` bytes and never
   * crossing an original entry boundary.
   * @tparam SpaceTag The address space the caller expects this cursor to
   * carry; validated against the erased entry type (see `holds()`).
   * @param max_length Upper bound on the returned chunk's length; the
   * actual chunk may be shorter (see the class docs).
   * @return The next chunk, or `error::invalid_argument` if `max_length
   * == 0` or `SpaceTag` does not match the erased entry type, or
   * `error::out_of_bounds` once every entry has been fully consumed.
   */
  template <typename SpaceTag>
  [[nodiscard]] result<sg_entry<SpaceTag, PhysInt>> next_up_to(PhysInt max_length) & noexcept {
    if (max_length == 0 || !holds<SpaceTag>()) {
      return unexpected(error::invalid_argument);
    }

    result<raw_chunk> chunk = advance_(list_, entry_idx_, offset_, max_length);
    if (!chunk) {
      return unexpected(chunk.error());
    }
    return sg_entry<SpaceTag, PhysInt>{phys_addr<void, SpaceTag, PhysInt>{chunk->addr_value}, chunk->length};
  }

private:
  /** @brief A chunk's raw address/length, before being re-wrapped into a typed `sg_entry`. */
  struct raw_chunk {
    PhysInt addr_value;
    PhysInt length;
  };

  using advance_fn = result<raw_chunk> (*)(const void *, std::size_t &, PhysInt &, PhysInt);

  /** @brief Type-erased per-`Container` chunking logic, bound to `advance_` at construction. */
  template <typename Container>
  static result<raw_chunk> advance(const void *ctx, std::size_t &entry_idx, PhysInt &offset,
                                   PhysInt max_length) noexcept {
    const auto &entries = static_cast<const sg_list<Container> *>(ctx)->base();
    while (entry_idx < entries.size() && offset >= entries[entry_idx].length) {
      ++entry_idx;
      offset = 0;
    }
    if (entry_idx >= entries.size()) {
      return unexpected(error::out_of_bounds);
    }

    const auto &current = entries[entry_idx];
    PhysInt remaining = current.length - offset;
    PhysInt chunk_length = remaining < max_length ? remaining : max_length;
    raw_chunk chunk{current.addr.value + offset, chunk_length};
    offset += chunk_length;
    return chunk;
  }

  const void *list_;
  advance_fn advance_;
  type_id entry_type_;
  std::size_t entry_idx_{0};
  PhysInt offset_{0};
};

/** @brief Deduces `sg_list_cursor<PhysInt>` from an `sg_list<Container>`'s own `phys_int`. */
template <typename Container>
sg_list_cursor(const sg_list<Container> &) -> sg_list_cursor<typename sg_list<Container>::phys_int>;

} // namespace structo

/** @brief Propagates relocatability from an SG list's container element type. */
template <typename T> struct reloco::is_trivially_relocatable<structo::sg_list<T>> : is_trivially_relocatable<T> {};
