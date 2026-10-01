// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file phys_page.hpp
 * @brief `structo::page_view<PageTraits, OsTraits>`: a typed view of an
 * operating-system page (pointer, PFN, or compressed handle, whatever
 * `OsTraits::os_page_type` is), plus `page_traits<Size, Shift>` (compile-
 * time page-size constants) and the `os_traits_base<Derived, OsPage>`
 * CRTP helper that implements buddy-allocator-style neighbor/retreat/
 * advance arithmetic on top of a handful of OS-supplied primitives.
 *
 * `Derived` (passed as the first `os_traits_base` template argument, CRTP-
 * style) only needs to provide `to_pfn(p)`, `from_pfn(pfn) ->
 * result<os_page_type>`, and `is_same_zone(a, b)`; `os_traits_base` then
 * implements `try_advance()`/`try_retreat()`/`try_get_buddy()` generically
 * on top of those three. Example `Derived` (a compressed 32-bit page
 * handle backed by a flat metadata array, as used by `buddy_allocator`):
 * @code
 * struct my_os_traits : structo::os_traits_base<my_os_traits, uint32_t> {
 *   using os_page_type = uint32_t;
 *
 *   static os_page_type null_page() noexcept { return ~uint32_t(0); }
 *   static bool is_null(os_page_type p) noexcept { return p == null_page(); }
 *
 *   static uint64_t to_pfn(os_page_type p) noexcept { return p; }
 *
 *   static reloco::result<os_page_type> from_pfn(uint64_t pfn) noexcept {
 *     if (pfn >= MY_TOTAL_PAGES) {
 *       return reloco::unexpected(reloco::error::out_of_range);
 *     }
 *     return static_cast<uint32_t>(pfn);
 *   }
 *
 *   static bool is_same_zone(os_page_type a, os_page_type b) noexcept {
 *     return my_meta[a].zone_id == my_meta[b].zone_id;
 *   }
 * };
 * @endcode
 */

#include <reloco/error.hpp>
#include "phys_addr.hpp"
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace structo {

using namespace reloco;

// ============================================================================
// Page Traits
// ============================================================================

/**
 * @brief Compile-time page-size and page-shift traits.
 * @tparam Size Page size in bytes.
 * @tparam Shift Base-two page-size shift.
 */
template <size_t Size, size_t Shift> struct page_traits {
  static_assert((1ULL << Shift) == Size, "Shift must match Size");
  static constexpr size_t page_size = Size;
  static constexpr size_t page_shift = Shift;
  static constexpr uint64_t alignment_mask = Size - 1;
};

using page_4k = page_traits<4096, 12>;
using page_16k = page_traits<16 * 1024, 14>;
using page_64k = page_traits<64 * 1024, 16>;
using page_2m = page_traits<2 * 1024 * 1024, 21>;
using page_1g = page_traits<1024 * 1024 * 1024, 30>;

// ============================================================================
// CRTP Base for OS Traits
// ============================================================================

/**
 * @brief CRTP helper implementing common operating-system page operations.
 * @tparam Derived Concrete OS traits type.
 * @tparam OsPage OS page handle type.
 */
template <typename Derived, typename OsPage> struct os_traits_base {
  using os_page_type = OsPage; // Can be a pointer OR a compressed integer/handle!

  [[nodiscard]] static result<os_page_type> try_advance(os_page_type p, size_t count) noexcept {
    uint64_t pfn = Derived::to_pfn(p);

    if (~uint64_t(0) - count < pfn)
      return unexpected(error::out_of_range);

    auto target_res = Derived::from_pfn(pfn + count);
    if (!target_res)
      return unexpected(error::out_of_range);

    if (!Derived::is_same_zone(p, *target_res))
      return unexpected(error::security_violation);

    return *target_res;
  }

  [[nodiscard]] static result<os_page_type> try_retreat(os_page_type p, size_t count) noexcept {
    uint64_t pfn = Derived::to_pfn(p);
    if (pfn < count)
      return unexpected(error::out_of_range);

    auto target_res = Derived::from_pfn(pfn - count);
    if (!target_res)
      return unexpected(error::out_of_range);

    if (!Derived::is_same_zone(p, *target_res))
      return unexpected(error::security_violation);

    return *target_res;
  }

  [[nodiscard]] static result<os_page_type> try_get_buddy(os_page_type p, uint16_t order) noexcept {
    uint64_t buddy_pfn = Derived::to_pfn(p) ^ (1ULL << order);

    auto buddy_res = Derived::from_pfn(buddy_pfn);
    if (!buddy_res)
      return unexpected(error::out_of_range);

    if (!Derived::is_same_zone(p, *buddy_res))
      return unexpected(error::security_violation);

    return *buddy_res;
  }
};

// ============================================================================
// Page View
// ============================================================================

/**
 * @brief Typed view of an operating-system page.
 * @tparam PageTraits Page-size traits.
 * @tparam OsTraits Operating-system page traits.
 */
template <typename PageTraits, typename OsTraits> class page_view {
public:
  using page_traits_type = PageTraits;
  using os_traits_type = OsTraits;
  using os_page_type = typename OsTraits::os_page_type;

  constexpr page_view() noexcept : page_(OsTraits::null_page()) {}
  constexpr page_view(std::nullptr_t) noexcept : page_(OsTraits::null_page()) {}

  [[nodiscard]] static constexpr page_view from_os_page(os_page_type p) noexcept { return page_view{p}; }

  [[nodiscard]] constexpr bool is_null() const noexcept { return OsTraits::is_null(page_); }
  constexpr explicit operator bool() const noexcept { return !is_null(); }

  // Prevent unchecked arithmetic
  page_view operator+(size_t count) const = delete;
  page_view operator-(size_t count) const = delete;
  page_view &operator+=(size_t count) = delete;
  page_view &operator-=(size_t count) = delete;

  [[nodiscard]] result<page_view> try_add(size_t count) const noexcept {
    if (is_null())
      return unexpected(error::invalid_argument);
    auto res = OsTraits::try_advance(page_, count);
    if (!res)
      return unexpected(res.error());
    return page_view{*res};
  }

  [[nodiscard]] result<page_view> try_sub(size_t count) const noexcept {
    if (is_null())
      return unexpected(error::invalid_argument);
    auto res = OsTraits::try_retreat(page_, count);
    if (!res)
      return unexpected(res.error());
    return page_view{*res};
  }

  [[nodiscard]] uint16_t buddy_order() const noexcept { return OsTraits::buddy_order(page_); }
  void set_buddy_order(uint16_t order) noexcept { OsTraits::set_buddy_order(page_, order); }

  [[nodiscard]] bool is_buddy_free() const noexcept { return OsTraits::is_buddy_free(page_); }
  void set_buddy_free(bool is_free) noexcept { OsTraits::set_buddy_free(page_, is_free); }

  [[nodiscard]] result<page_view> try_get_buddy(uint16_t order) const noexcept {
    if (is_null())
      return unexpected(error::invalid_argument);
    auto res = OsTraits::try_get_buddy(page_, order);
    if (!res)
      return unexpected(res.error());
    return page_view{*res};
  }

  [[nodiscard]] uint64_t pfn() const noexcept { return is_null() ? 0 : OsTraits::to_pfn(page_); }

  template <typename SpaceTag = host_phys_space, typename PhysInt = uint64_t>
  [[nodiscard]] phys_addr<void, SpaceTag, PhysInt> phys() const noexcept {
    if (is_null())
      return phys_addr<void, SpaceTag, PhysInt>{nullptr};
    return phys_addr<void, SpaceTag, PhysInt>{OsTraits::to_pfn(page_) << PageTraits::page_shift};
  }

  [[nodiscard]] os_page_type get_os_page() const noexcept { return page_; }

  [[nodiscard]] friend constexpr bool operator==(const page_view &lhs, const page_view &rhs) noexcept {
    return lhs.page_ == rhs.page_;
  }
  [[nodiscard]] friend constexpr bool operator!=(const page_view &lhs, const page_view &rhs) noexcept {
    return lhs.page_ != rhs.page_;
  }

private:
  constexpr explicit page_view(os_page_type p) noexcept : page_(p) {}

  os_page_type page_;
};

} // namespace structo