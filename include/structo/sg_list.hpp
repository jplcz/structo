// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "phys_addr.hpp"
#include <reloco/error.hpp>
#include <reloco/vector.hpp>

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

  [[nodiscard]] Container &base() noexcept { return c_; }
  [[nodiscard]] const Container &base() const noexcept { return c_; }

private:
  Container c_;
};

} // namespace structo

/** @brief Propagates relocatability from an SG list's container element type. */
template <typename T> struct reloco::is_trivially_relocatable<structo::sg_list<T>> : is_trivially_relocatable<T> {};
