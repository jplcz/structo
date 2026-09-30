#pragma once
#include "phys_addr.hpp"
#include "phys_page.hpp"

namespace structo {

using namespace reloco;

// ============================================================================
// Strongly Typed Page Frame Number (PFN)
// ============================================================================

/**
 * @brief Typed physical page-frame number.
 * @tparam SpaceTag Physical address-space tag.
 * @tparam PageTraits Page-size traits.
 * @tparam PhysInt Underlying integer representation.
 */
template <typename SpaceTag = default_phys_space, typename PageTraits = page_4k, typename PhysInt = std::uint64_t>
struct phys_pfn {
  using space_tag = SpaceTag;
  using page_traits = PageTraits;
  using address_type = PhysInt;

  PhysInt value{~PhysInt(0)};

  constexpr phys_pfn() noexcept = default;
  constexpr phys_pfn(std::nullptr_t) noexcept {}
  constexpr explicit phys_pfn(PhysInt val) noexcept : value(val) {}

  [[nodiscard]] constexpr bool is_null() const noexcept { return value == ~PhysInt(0); }
  constexpr explicit operator bool() const noexcept { return !is_null(); }

  /** @brief Converts a strongly-typed physical address to this PFN type. */
  template <typename T>
  [[nodiscard]] static constexpr phys_pfn from_addr(phys_addr<T, SpaceTag, PhysInt> addr) noexcept {
    if (addr.is_null()) {
      return phys_pfn{};
    }
    // Shifts the underlying address value down by the traits' page_shift[cite: 4, 5]
    return phys_pfn{addr.value >> PageTraits::page_shift};
  }

  /** @brief Converts this PFN back to a physical address. */
  template <typename T = void> [[nodiscard]] constexpr phys_addr<T, SpaceTag, PhysInt> to_addr() const noexcept {
    if (is_null()) {
      return phys_addr<T, SpaceTag, PhysInt>{nullptr}; // Uses the nullptr constructor
    }
    // Shifts the PFN up by the traits' page_shift to form the physical address[cite: 4, 5]
    return phys_addr<T, SpaceTag, PhysInt>{value << PageTraits::page_shift};
  }

  [[nodiscard]] friend constexpr bool operator==(const phys_pfn &lhs, const phys_pfn &rhs) noexcept {
    return lhs.value == rhs.value;
  }
  [[nodiscard]] friend constexpr bool operator!=(const phys_pfn &lhs, const phys_pfn &rhs) noexcept {
    return lhs.value != rhs.value;
  }
};

// ============================================================================
// Physical Address Page Math Utilities
// ============================================================================

namespace page_math {

/** @brief Extracts the page offset from a physical address. */
template <typename PageTraits, typename T, typename SpaceTag, typename PhysInt>
[[nodiscard]] constexpr PhysInt offset(phys_addr<T, SpaceTag, PhysInt> paddr) noexcept {
  if (paddr.is_null())
    return 0;
  // Masks the address using the page_traits alignment_mask[cite: 4, 5]
  return paddr.value & PageTraits::alignment_mask;
}

/** @brief Rounds a physical address down to the nearest page boundary. */
template <typename PageTraits, typename T, typename SpaceTag, typename PhysInt>
[[nodiscard]] constexpr phys_addr<T, SpaceTag, PhysInt> align_down(phys_addr<T, SpaceTag, PhysInt> paddr) noexcept {
  if (paddr.is_null())
    return paddr;
  // Clears the lower bits using the inverted alignment_mask[cite: 4, 5]
  return phys_addr<T, SpaceTag, PhysInt>{paddr.value & ~PhysInt(PageTraits::alignment_mask)};
}

/** @brief Rounds a physical address up to the nearest page boundary. */
template <typename PageTraits, typename T, typename SpaceTag, typename PhysInt>
[[nodiscard]] constexpr phys_addr<T, SpaceTag, PhysInt> align_up(phys_addr<T, SpaceTag, PhysInt> paddr) noexcept {
  if (paddr.is_null())
    return paddr;
  // Adds the mask and clears the lower bits to align upwards[cite: 4, 5]
  PhysInt val = (paddr.value + PageTraits::alignment_mask) & ~PhysInt(PageTraits::alignment_mask);
  return phys_addr<T, SpaceTag, PhysInt>{val};
}

} // namespace page_math

} // namespace structo
