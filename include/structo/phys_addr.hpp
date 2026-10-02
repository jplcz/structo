// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>
#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <type_traits>

namespace structo {

using namespace reloco;

// ============================================================================
// Physical Address Space Tags
// ============================================================================

/** @brief Default physical address space for unified flat-memory systems. */
struct default_phys_space {};

/** @brief Physical address as seen by the Host CPU (Hypervisor/VMM). */
struct host_phys_space {};

/** @brief Physical address as seen by a Guest VM (requires EPT/Stage-2 translation). */
struct guest_phys_space {};

/** @brief Physical address on an external bus (e.g., PCIe DMA / IOMMU domain). */
struct dma_bus_space {};

// --- ARM TrustZone (Legacy / Standard) ---

/** @brief Secure Physical Address Space (S-PAS) */
struct secure_phys_space {};

/** @brief Non-Secure Physical Address Space (NS-PAS) */
struct nonsecure_phys_space {};

// --- ARM Realm Management Extension (RME) ---

/** @brief Root Physical Address Space (R-PAS). Exclusive to EL3 Monitor. */
struct root_phys_space {};

/** @brief Realm Physical Address Space (RL-PAS). Isolated Confidential VMs. */
struct realm_phys_space {};

// ============================================================================
// Tagged Physical Address
// ============================================================================

/**
 * @brief Strong type for a tagged physical address.
 *
 * @tparam T The type this physical address points to (default: void).
 * @tparam SpaceTag The address space domain (prevents mixing GPA/HPA/DMA).
 * @tparam PhysInt The integer type used to store the address.
 */
template <typename T = void, typename SpaceTag = default_phys_space, typename PhysInt = std::uint64_t>
struct phys_addr {
  using value_type = T;
  using space_tag = SpaceTag;
  using address_type = PhysInt;

  address_type value{~PhysInt(0)};

  constexpr phys_addr() noexcept = default;
  constexpr phys_addr(std::nullptr_t) noexcept {}
  constexpr explicit phys_addr(address_type val) noexcept : value(val) {}

  [[nodiscard]] constexpr bool is_null() const noexcept { return value == ~PhysInt(0); }
  constexpr explicit operator bool() const noexcept { return value != ~PhysInt(0); }

  // Cast the pointed-to type (e.g., void -> acpi_header), staying in the same space
  template <typename U> [[nodiscard]] constexpr phys_addr<U, SpaceTag, PhysInt> cast_type() const noexcept {
    return phys_addr<U, SpaceTag, PhysInt>{value};
  }

  //    Explicit Escape Hatch: Cast between address spaces.
  //    (e.g., when the IOMMU is configured for identity mapping, DMA == HPA).
  //    Requires Clang unsafe block as operation is trivial to misuse
  template <typename NewSpaceTag>
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE constexpr phys_addr<T, NewSpaceTag, PhysInt> cast_space() const noexcept {
    return phys_addr<T, NewSpaceTag, PhysInt>{value};
  }

  [[nodiscard]] friend constexpr bool operator==(const phys_addr &lhs, const phys_addr &rhs) noexcept {
    return lhs.value == rhs.value;
  }
  [[nodiscard]] friend constexpr bool operator!=(const phys_addr &lhs, const phys_addr &rhs) noexcept {
    return lhs.value != rhs.value;
  }
};

// ============================================================================
// Mapping Traits (Translation Policies)
// ============================================================================

/**
 * @brief Fixed Offset Mapper (Direct Map).
 * We enforce that this mapper only translates addresses belonging to the
 * ExpectedSpace (e.g., only mapping Host Physical RAM, not Guest RAM).
 */
template <std::uintptr_t VirtBase, std::uintptr_t PhysSize, typename ExpectedSpace = default_phys_space,
          std::uint64_t PhysBase = 0>
struct dmap_mapper {
  using space_tag = ExpectedSpace;

  template <typename T, typename PhysInt>
  [[nodiscard]] static result<T *> to_virt(phys_addr<T, ExpectedSpace, PhysInt> p) noexcept {
    if (p.value >= PhysSize)
      return unexpected(error::out_of_range);
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE;
    std::uintptr_t vaddr = static_cast<std::uintptr_t>(p.value - PhysBase) + VirtBase;
    return reinterpret_cast<T *>(vaddr);
    RELOCO_END_UNSAFE_BUFFER_USAGE
  }

  template <typename T, typename PhysInt = std::uint64_t>
  [[nodiscard]] static result<phys_addr<T, ExpectedSpace, PhysInt>> to_phys(const T *v) noexcept {
    const uintptr_t virt_v = reinterpret_cast<std::uintptr_t>(v);

    if (virt_v < VirtBase || virt_v >= (VirtBase + PhysSize))
      return unexpected(error::out_of_range);

    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    PhysInt paddr = static_cast<PhysInt>(reinterpret_cast<std::uintptr_t>(v) - VirtBase) + PhysBase;
    return phys_addr<T, ExpectedSpace, PhysInt>{paddr};
    RELOCO_END_UNSAFE_BUFFER_USAGE
  }

  template <typename T, typename PhysInt>
  [[nodiscard]] static bool validate_phys(phys_addr<T, ExpectedSpace, PhysInt> p) noexcept {
    return p.value < PhysSize;
  }
};

// ============================================================================
// Smart Pointer for Auto-Translating Access
// ============================================================================

/**
 * @brief Pointer-like direct-map view of a physical address.
 * @tparam T Pointed-to type.
 * @tparam Mapper Direct-map policy.
 * @tparam PhysInt Physical-address integer type.
 */
template <typename T, typename Mapper, typename PhysInt = std::uint64_t> class dmap_ptr {
public:
  using space_tag = typename Mapper::space_tag;
  using phys_type = phys_addr<T, space_tag, PhysInt>;

  constexpr dmap_ptr() noexcept = default;
  constexpr dmap_ptr(std::nullptr_t) noexcept {}

  [[nodiscard]] constexpr phys_type phys() const noexcept { return paddr_; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return paddr_.is_null(); }
  constexpr explicit operator bool() const noexcept { return !paddr_.is_null(); }

  [[nodiscard]] result<T *> try_get() const noexcept {
    if (is_null()) {
      return unexpected(error::invalid_argument);
    }
    return Mapper::to_virt(paddr_);
  }

  [[nodiscard]] static result<dmap_ptr> from_paddr(phys_type phys) noexcept {
    if (phys.is_null()) {
      return dmap_ptr{};
    }
    if (!Mapper::validate_phys(phys))
      return unexpected(error::security_violation);
    return dmap_ptr(phys);
  }

  [[nodiscard]] static result<dmap_ptr> from_virt(const T *virt) noexcept {
    if (virt == nullptr) {
      return dmap_ptr{};
    }
    const auto res = Mapper::to_phys(virt);
    if (!res.has_value())
      return unexpected(res.error());
    return dmap_ptr(res.value());
  }

private:
  constexpr explicit dmap_ptr(phys_type v) : paddr_(v) {}

  phys_type paddr_{nullptr}; // Automatically uses the ~0 initialization
};
static_assert(sizeof(dmap_ptr<void, dmap_mapper<0x0, 0x1>, uint64_t>) == sizeof(std::uint64_t),
              "dmap_ptr must have zero overhead");

} // namespace structo