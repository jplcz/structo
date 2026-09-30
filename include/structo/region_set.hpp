// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstddef>
#include <cstdint>
#include <reloco/inline_vector.hpp>
#include "phys_addr.hpp"

namespace structo {

using namespace reloco;

/**
 * @brief Contiguous physical memory interval.
 * @tparam PhysInt Physical-address integer type.
 */
template <typename PhysInt = uint64_t> struct memory_region {
  PhysInt base;
  PhysInt size;

  constexpr PhysInt end() const noexcept { return base + size; }
};

/**
 * @brief A fixed-capacity, sorted array of memory regions.
 * Automatically merges adjacent/overlapping regions on insertion,
 * and handles splitting regions when subtracting reserved memory.
 */
template <size_t Capacity, typename PhysInt = uint64_t>
class region_set : public inline_vector<memory_region<PhysInt>, Capacity> {
public:
  constexpr region_set() noexcept = default;

  region_set(inline_vector<memory_region<PhysInt>, Capacity> &&other) noexcept
      : inline_vector<memory_region<PhysInt>, Capacity>(std::move(other)) {}

  region_set &operator=(inline_vector<memory_region<PhysInt>, Capacity> &&other) noexcept {
    inline_vector<memory_region<PhysInt>, Capacity>::operator=(std::move(other));
    return *this;
  }

  /**
   * @brief Adds a memory region, merging it with existing adjacent or overlapping regions.
   */
  result<void> try_add(PhysInt base, PhysInt size) noexcept {
    if (size == 0)
      return {};

    PhysInt end = base + size;
    size_t insert_idx = 0;

    // Find the right place to insert (sorted by base)
    while (insert_idx < this->size() && (*this)[insert_idx].end() < base) {
      insert_idx++;
    }

    // Check for overlap/adjacency with current and subsequent regions
    size_t merge_end_idx = insert_idx;
    while (merge_end_idx < this->size() && (*this)[merge_end_idx].base <= end) {
      if ((*this)[merge_end_idx].base < base) {
        base = (*this)[merge_end_idx].base;
      }
      if ((*this)[merge_end_idx].end() > end) {
        end = (*this)[merge_end_idx].end();
      }
      merge_end_idx++;
    }

    size_t elements_to_remove = merge_end_idx - insert_idx;

    if (elements_to_remove == 0) {
      // Pure insertion: ensure capacity and shift right
      auto push_res = this->try_push_back({0, 0});
      if (!push_res)
        return unexpected(push_res.error());

      for (size_t i = this->size() - 1; i > insert_idx; --i) {
        (*this)[i] = (*this)[i - 1];
      }
      (*this)[insert_idx] = {base, end - base};
    } else {
      // Update the first element to the new merged region bounds
      (*this)[insert_idx] = {base, end - base};

      // If we absorbed multiple regions, shift the rest left to close the gap
      if (elements_to_remove > 1) {
        size_t shift_amount = elements_to_remove - 1;
        for (size_t i = insert_idx + 1; i < this->size() - shift_amount; ++i) {
          (*this)[i] = (*this)[i + shift_amount];
        }
        for (size_t i = 0; i < shift_amount; ++i) {
          this->pop_back();
        }
      }
    }

    return {};
  }

  /**
   * @brief Subtracts a region from the set. Used to punch holes for reserved memory.
   */
  RELOCO_CONSTEXPR20 result<void> try_subtract(PhysInt base, PhysInt size) noexcept {
    if (size == 0)
      return {};
    PhysInt sub_end = base + size;

    for (size_t i = 0; i < this->size();) {
      PhysInt r_base = (*this)[i].base;
      PhysInt r_end = (*this)[i].end();

      // Case 1: No overlap
      if (sub_end <= r_base || base >= r_end) {
        i++;
        continue;
      }

      // Case 2: Complete overlap (remove region entirely)
      if (base <= r_base && sub_end >= r_end) {
        for (size_t j = i; j < this->size() - 1; ++j) {
          (*this)[j] = (*this)[j + 1];
        }
        this->pop_back();
        continue; // Do not increment i
      }

      // Case 3: Overlap at the beginning (shrink from the left)
      if (base <= r_base && sub_end < r_end) {
        (*this)[i].base = sub_end;
        (*this)[i].size = r_end - sub_end;
        i++;
        continue;
      }

      // Case 4: Overlap at the end (shrink from the right)
      if (base > r_base && sub_end >= r_end) {
        (*this)[i].size = base - r_base;
        i++;
        continue;
      }

      // Case 5: Split (the subtracted region falls perfectly in the middle)
      if (base > r_base && sub_end < r_end) {
        auto push_res = this->try_push_back({0, 0});
        if (!push_res)
          return unexpected(push_res.error());

        // Shift everything right to make room for the split piece
        for (size_t j = this->size() - 1; j > i + 1; --j) {
          (*this)[j] = (*this)[j - 1];
        }

        // First half
        (*this)[i].base = r_base;
        (*this)[i].size = base - r_base;

        // Second half
        (*this)[i + 1].base = sub_end;
        (*this)[i + 1].size = r_end - sub_end;

        i += 2;
        continue;
      }
    }
    return {};
  }

  /**
   * @brief Helper to find the largest contiguous block of RAM.
   */
  RELOCO_CONSTEXPR20 memory_region<PhysInt> largest_region() const noexcept {
    memory_region<PhysInt> largest{0, 0};
    for (size_t i = 0; i < this->size(); ++i) {
      if ((*this)[i].size > largest.size) {
        largest = (*this)[i];
      }
    }
    return largest;
  }
};

} // namespace structo