// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include <cstdint>
#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/int_ops.hpp>
#include <reloco/lifetime.hpp>
#include <type_traits>
#include <utility>

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

  /**
   * @brief Whether this address is null.
   *
   * Both the default-constructed sentinel (`~PhysInt(0)`, all-bits-set)
   * and a literal `0` count as null. Physical address `0` is reserved/
   * unmapped on virtually every real platform (the null page, the real-mode
   * IVT on x86, ...), so treating it as a distinct "valid" address was an
   * easy way to inject a bug: any `phys_addr` that ends up zero-initialized
   * by something other than this type's own default constructor -- e.g.
   * `memset`, a zeroed/`{}`-aggregate-initialized struct, or a POD field
   * shared with C-interop code -- would otherwise be silently accepted as
   * a legitimate address instead of being caught as "not set".
   */
  [[nodiscard]] constexpr bool is_null() const noexcept { return value == 0 || value == ~PhysInt(0); }
  constexpr explicit operator bool() const noexcept { return !is_null(); }

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

  // --------------------------------------------------------------------------
  // Checked Math (never wraps/UBs; see reloco/int_ops.hpp)
  // --------------------------------------------------------------------------

  /**
   * @brief Advances this address by @p offset bytes, failing with
   * `error::integer_overflow` instead of wrapping past the integer's range.
   */
  [[nodiscard]] constexpr result<phys_addr> try_add(address_type offset) const noexcept {
    auto added = checked_add(value, offset);
    if (!added.has_value())
      return unexpected(added.error());
    return phys_addr{added.value()};
  }

  /**
   * @brief Retreats this address by @p offset bytes, failing with
   * `error::integer_overflow` instead of underflowing past zero.
   */
  [[nodiscard]] constexpr result<phys_addr> try_sub(address_type offset) const noexcept {
    auto subtracted = checked_sub(value, offset);
    if (!subtracted.has_value())
      return unexpected(subtracted.error());
    return phys_addr{subtracted.value()};
  }

  /**
   * @brief Computes the byte distance from @p other to `*this` (i.e.
   * `this->value - other.value`), failing with `error::integer_overflow`
   * if @p other is further along than `*this` (the subtraction would
   * underflow). Both operands must share the same `SpaceTag`/`PhysInt`,
   * enforced by the parameter type.
   */
  [[nodiscard]] constexpr result<address_type> try_diff(const phys_addr &other) const noexcept {
    return checked_sub(value, other.value);
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

  /**
   * @brief RAII handle shaped identically to `slot_map_ptr<T, Mapper>::guard`
   * (`slot_map_ptr.hpp`), so code generic over "some scoped physical-address
   * mapping" -- e.g. `compat_sg.hpp`'s `chained_sg_codec`/`two_level_sg_codec`
   * -- can use either a `dmap_ptr` or a `slot_map_ptr` as its `Mapper`
   * callable's return value without caring which. Since the direct map is
   * permanent, there is nothing to actually release: `reset()`/the
   * destructor are no-ops, but every other member (`get()`/`operator->()`/
   * `operator*()`/`is_null()`/move semantics) behaves identically to
   * `slot_map_ptr`'s guard.
   */
  class [[nodiscard]] RELOCO_POINTER guard {
  public:
    constexpr guard() noexcept = default;
    ~guard() noexcept = default;

    guard(const guard &) = delete;
    guard &operator=(const guard &) = delete;

    guard(guard &&other) noexcept : m_ptr(std::exchange(other.m_ptr, nullptr)) {}
    guard &operator=(guard &&other) noexcept {
      if (this != &other) {
        m_ptr = std::exchange(other.m_ptr, nullptr);
      }
      return *this;
    }

    [[nodiscard]] constexpr bool is_null() const noexcept { return m_ptr == nullptr; }
    constexpr explicit operator bool() const noexcept { return m_ptr != nullptr; }

    [[nodiscard]] T *get() const noexcept RELOCO_LIFETIMEBOUND { return m_ptr; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return m_ptr; }
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return *m_ptr; }

    /** @brief No-op (the direct map is permanent); present only for API parity with `slot_map_ptr::guard`. */
    void reset() noexcept { m_ptr = nullptr; }

  private:
    friend class dmap_ptr;
    constexpr explicit guard(T *ptr) noexcept : m_ptr(ptr) {}

    T *m_ptr{nullptr};
  };

  /**
   * @brief `try_get()`, wrapped in the same `guard` RAII handle `slot_map_ptr`
   * returns from its own `try_map()`. Prefer this over `try_get()` when
   * writing `Mapper`-callable code (e.g. for `compat_sg.hpp`) that must
   * work unchanged whether the concrete pointer type is a `dmap_ptr` or a
   * `slot_map_ptr`.
   * @param (unnamed) Accepted and ignored for signature parity with
   * `slot_map_ptr::try_map(std::size_t)`; the direct map always covers
   * exactly one `T` at `phys()`, regardless of the requested byte span.
   */
  [[nodiscard]] result<guard> try_map(std::size_t = sizeof(T)) const noexcept {
    auto res = try_get();
    if (!res.has_value())
      return unexpected(res.error());
    return guard(res.value());
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