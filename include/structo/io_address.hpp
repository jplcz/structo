// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file io_address.hpp
 * @brief `structo::io_address<T, SpaceTag, IoInt>`: a typed, tagged address
 * into some device-register address space -- x86-style port I/O, a
 * memory-mapped register window, an ARM `Device-nGnRnE` region, ... --
 * plus `reg_traits<Size>` and the `io_math` namespace for register-size-
 * aware offset arithmetic.
 *
 * This is a sibling of `phys_addr`/`phys_addr.hpp`, not a specialization
 * of it: `phys_addr` tags *where in physical memory* a byte lives
 * (host/guest/DMA/secure/...); `io_address` tags *which device register
 * space* an address lives in, which is a materially different axis --
 * two `io_address`es can be numerically equal and still refer to
 * completely different hardware if their `SpaceTag`s differ (an x86 port
 * number is not a byte offset into some BAR's MMIO window, and a
 * Secure-world MMIO alias is not the same register as its Non-secure
 * alias even when both decode to the same literal address).
 *
 * `io_address` deliberately does *no* I/O itself -- no `inb`/`outb`, no
 * volatile MMIO load/store, nothing. It is pure address tagging and
 * compile-time-checked arithmetic, exactly like `phys_addr`. A later,
 * separate type (a type-erased `io_space_ref`, layered on top of this
 * one) is expected to own the actual access side: single/multi-size
 * reads and writes, `rep insb`/`rep outsb`-style string I/O, and whatever
 * else a concrete `SpaceTag`'s backing hardware needs -- none of that
 * belongs in a zero-overhead address type.
 *
 * ## A universal framework, not an x86 header
 *
 * Nothing here is specific to x86 port I/O or to ARM. `SpaceTag` is an
 * entirely open, caller-extensible set -- this header only ships a
 * handful of illustrative example tags (`port_io_space` for x86-style
 * `IN`/`OUT` port space, `device_io_space` for a plain, domain-agnostic
 * MMIO window, and `secure_io_space`/`nonsecure_io_space`/
 * `realm_io_space`/`hypervisor_io_space` for the usual TrustZone/RME/
 * virtualization security domains a platform's MMIO decode might be
 * split across). Embedders targeting some other architecture's register
 * space (RISC-V PLIC/CLINT windows, a PCI config-space window, a
 * software-defined accelerator's doorbell space, ...) define their own
 * empty tag struct and get the exact same compile-time space separation
 * for free.
 *
 * ## Why `cast_space()` exists here (unlike `target_ptr`)
 *
 * `target_ptr.hpp` deliberately has no `cast_space()` at all, because two
 * *virtual* address spaces essentially never coincide bit-for-bit.
 * `io_address` follows `phys_addr`'s lead instead and keeps the (unsafe,
 * `RELOCO_UNSAFE_BUFFER_USAGE`-gated) escape hatch: unlike virtual address
 * spaces, device register spaces frequently *do* alias on purpose --
 * a Secure and Non-secure MMIO window are often the exact same physical
 * decode at a fixed offset from each other, a hypervisor may trap-and-
 * emulate a guest-visible device at the identical address it itself uses
 * to program the real hardware, and a debug/back-door window often
 * mirrors a normal one verbatim. `cast_space()` lets an embedder express
 * "I know this specific platform aliases these two spaces" explicitly,
 * the same way `phys_addr::cast_space()` does for an identity-mapped
 * IOMMU.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/int_ops.hpp>
#include <type_traits>

namespace structo {

using namespace reloco;

// ============================================================================
// I/O Address Space Tags
// ============================================================================

/** @brief Default I/O address space for platforms with a single, flat register space. */
struct default_io_space {};

/** @brief x86-style port-mapped I/O space, accessed via `IN`/`OUT`, not load/store. */
struct port_io_space {};

/** @brief A plain, domain-agnostic memory-mapped register window. */
struct device_io_space {};

/** @brief Secure-world (e.g. ARM TrustZone S-EL1/S-EL0) MMIO alias. */
struct secure_io_space {};

/** @brief Non-secure-world MMIO alias. */
struct nonsecure_io_space {};

/** @brief An ARM CCA Realm's MMIO alias (RL-PAS-backed, emulated or assigned devices). */
struct realm_io_space {};

/** @brief MMIO owned/trapped-and-emulated by a hypervisor (EL2), not passed through to a guest. */
struct hypervisor_io_space {};

namespace detail {

// void has no object representation to stride by; every other T steps by
// sizeof(T), matching target_ptr.hpp's identical element_size convention.
template <typename T> struct io_element_size {
  static constexpr std::size_t value = sizeof(T);
};
template <> struct io_element_size<void> {
  static constexpr std::size_t value = 1;
};

} // namespace detail

// ============================================================================
// Tagged I/O Address
// ============================================================================

/**
 * @brief Strong type for a tagged device-register address.
 *
 * @tparam T The type of the register/value this address refers to
 * (default: `void`, for raw/untyped register-space arithmetic).
 * @tparam SpaceTag The I/O address-space domain (`port_io_space`,
 * `device_io_space`, `secure_io_space`, `nonsecure_io_space`,
 * `realm_io_space`, `hypervisor_io_space`, or a caller-defined tag),
 * preventing e.g. a port number from being mixed up with an MMIO offset
 * at compile time.
 * @tparam IoInt The integer type used to store the address (default:
 * `std::uint64_t`; use e.g. `std::uint16_t` for `port_io_space` on x86).
 */
template <typename T = void, typename SpaceTag = default_io_space, typename IoInt = std::uint64_t> struct io_address {
  using value_type = T;
  using space_tag = SpaceTag;
  using address_type = IoInt;
  using difference_type = std::make_signed_t<IoInt>;

  /** @brief Byte stride of one `T` element (`1` for `T = void`). */
  static constexpr std::size_t element_size = detail::io_element_size<T>::value;

  address_type value{~IoInt(0)};

  constexpr io_address() noexcept = default;
  constexpr io_address(std::nullptr_t) noexcept {}
  constexpr explicit io_address(address_type val) noexcept : value(val) {}

  [[nodiscard]] constexpr bool is_null() const noexcept { return value == ~IoInt(0); }
  constexpr explicit operator bool() const noexcept { return value != ~IoInt(0); }

  /** @brief Reinterprets the pointed-to type, staying in the same `SpaceTag`. */
  template <typename U> [[nodiscard]] constexpr io_address<U, SpaceTag, IoInt> cast_type() const noexcept {
    return io_address<U, SpaceTag, IoInt>{value};
  }

  /**
   * @brief Explicit escape hatch: reinterprets this address as belonging
   * to a different `SpaceTag` (e.g. when a platform's Secure and
   * Non-secure MMIO aliases are known to be the exact same byte offset
   * apart, or a hypervisor-trapped guest device address coincides with
   * the host's own). Requires `RELOCO_BEGIN_UNSAFE_BUFFER_USAGE` because
   * it is trivial to misuse across two spaces that do *not* actually
   * alias.
   */
  template <typename NewSpaceTag>
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE constexpr io_address<T, NewSpaceTag, IoInt> cast_space() const noexcept {
    return io_address<T, NewSpaceTag, IoInt>{value};
  }

  // --------------------------------------------------------------------
  // Comparisons. Only ever defined between two io_address instantiated
  // with the *same* T/SpaceTag/IoInt -- mixing e.g. port_io_space and
  // device_io_space addresses is a compile error, not a logic bug. All
  // six are provided (unlike phys_addr's ==/!= only) since ordering is
  // routinely meaningful here: checking an address falls within some
  // BAR/window range, sorting a device's registers, etc.
  // --------------------------------------------------------------------

  [[nodiscard]] friend constexpr bool operator==(const io_address &a, const io_address &b) noexcept {
    return a.value == b.value;
  }
  [[nodiscard]] friend constexpr bool operator!=(const io_address &a, const io_address &b) noexcept {
    return a.value != b.value;
  }
  [[nodiscard]] friend constexpr bool operator<(const io_address &a, const io_address &b) noexcept {
    return a.value < b.value;
  }
  [[nodiscard]] friend constexpr bool operator<=(const io_address &a, const io_address &b) noexcept {
    return a.value <= b.value;
  }
  [[nodiscard]] friend constexpr bool operator>(const io_address &a, const io_address &b) noexcept {
    return a.value > b.value;
  }
  [[nodiscard]] friend constexpr bool operator>=(const io_address &a, const io_address &b) noexcept {
    return a.value >= b.value;
  }

  // --------------------------------------------------------------------
  // Checked element-wise offset arithmetic. Unlike target_ptr, there is
  // deliberately no wrapping/saturating flavor: a device/port address
  // silently wrapping around is essentially always a bug (there is no
  // hardware notion of a "saturated register"), so checked_add/sub,
  // failing loudly with error::integer_overflow, is the only offset
  // primitive offered on the type itself. See the io_math namespace
  // below for register-size-aware (rather than plain sizeof(T)-strided)
  // offset helpers.
  // --------------------------------------------------------------------

  /**
   * @brief Advances by `n` elements (`n * element_size` bytes), failing
   * with `error::integer_overflow` instead of wrapping if either the
   * byte offset or the resulting address would overflow. `n` may be
   * negative, matching `checked_sub`'s own sign convention.
   */
  [[nodiscard]] constexpr result<io_address> checked_add(difference_type n) const noexcept {
    auto byte_offset = reloco::checked_mul<difference_type>(n, static_cast<difference_type>(element_size));
    if (!byte_offset)
      return unexpected(byte_offset.error());
    if (byte_offset.value() >= 0) {
      auto sum = reloco::checked_add<address_type>(value, static_cast<address_type>(byte_offset.value()));
      if (!sum)
        return unexpected(sum.error());
      return io_address{sum.value()};
    }
    auto magnitude = reloco::checked_neg(byte_offset.value());
    if (!magnitude)
      return unexpected(magnitude.error());
    auto diff = reloco::checked_sub<address_type>(value, static_cast<address_type>(magnitude.value()));
    if (!diff)
      return unexpected(diff.error());
    return io_address{diff.value()};
  }

  /** @copydoc checked_add */
  [[nodiscard]] constexpr result<io_address> checked_sub(difference_type n) const noexcept {
    auto neg_n = reloco::checked_neg(n);
    if (!neg_n)
      return unexpected(neg_n.error());
    return checked_add(neg_n.value());
  }

  /**
   * @brief Element-wise distance from `origin` to `*this`, failing with
   * `error::invalid_argument` if the byte distance is not an exact
   * multiple of `element_size`, or `error::integer_overflow` if it
   * doesn't fit `difference_type`.
   */
  [[nodiscard]] constexpr result<difference_type> checked_offset_from(const io_address &origin) const noexcept {
    const bool negative = value < origin.value;
    const address_type byte_diff = negative ? (origin.value - value) : (value - origin.value);
    if (byte_diff % element_size != 0)
      return unexpected(error::invalid_argument);
    auto scaled = reloco::checked_cast<difference_type>(byte_diff / element_size);
    if (!scaled)
      return unexpected(scaled.error());
    if (!negative)
      return scaled.value();
    return reloco::checked_neg(scaled.value());
  }
};

static_assert(sizeof(io_address<int, device_io_space>) == sizeof(std::uint64_t), "io_address must be zero-overhead");

// ============================================================================
// Register Width Traits
// ============================================================================

/**
 * @brief Compile-time register-width traits (byte count + alignment
 * mask), the `io_address`/`io_math` equivalent of `phys_page.hpp`'s
 * `page_traits<Size, Shift>`.
 * @tparam Size Register width in bytes; must be a power of two.
 */
template <std::size_t Size> struct reg_traits {
  static_assert(Size > 0 && (Size & (Size - 1)) == 0, "register size must be a power of two");
  static constexpr std::size_t reg_size = Size;
  static constexpr std::uint64_t alignment_mask = Size - 1;
};

using reg8 = reg_traits<1>;
using reg16 = reg_traits<2>;
using reg32 = reg_traits<4>;
using reg64 = reg_traits<8>;

// ============================================================================
// Register-Size-Aware Offset Math
// ============================================================================

/**
 * @brief Register-width-aware offset arithmetic over raw `io_address`es,
 * the `io_address` counterpart to `pfn_translator.hpp`'s `page_math`
 * namespace. Where `io_address::checked_add`/`checked_sub` stride by
 * `sizeof(T)` (meaningful once an address is cast to a concrete register
 * struct), these free functions instead stride by an explicit
 * `RegTraits::reg_size` -- the shape needed when working with a raw,
 * untyped register window carved into uniform `N`-byte slots (a GICD/
 * GICR-style distributor frame, a PCI config-space window, ...).
 */
namespace io_math {

/** @brief Extracts the in-register byte offset (`addr % RegTraits::reg_size`). */
template <typename RegTraits, typename T, typename SpaceTag, typename IoInt>
[[nodiscard]] constexpr IoInt offset(io_address<T, SpaceTag, IoInt> addr) noexcept {
  if (addr.is_null())
    return 0;
  return static_cast<IoInt>(addr.value & RegTraits::alignment_mask);
}

/** @brief Whether `addr` falls exactly on a `RegTraits::reg_size` boundary. */
template <typename RegTraits, typename T, typename SpaceTag, typename IoInt>
[[nodiscard]] constexpr bool is_aligned(io_address<T, SpaceTag, IoInt> addr) noexcept {
  return !addr.is_null() && offset<RegTraits>(addr) == 0;
}

/** @brief Rounds `addr` down to the nearest `RegTraits::reg_size` boundary. */
template <typename RegTraits, typename T, typename SpaceTag, typename IoInt>
[[nodiscard]] constexpr io_address<T, SpaceTag, IoInt> align_down(io_address<T, SpaceTag, IoInt> addr) noexcept {
  if (addr.is_null())
    return addr;
  return io_address<T, SpaceTag, IoInt>{static_cast<IoInt>(addr.value & ~IoInt(RegTraits::alignment_mask))};
}

/** @brief Rounds `addr` up to the nearest `RegTraits::reg_size` boundary. */
template <typename RegTraits, typename T, typename SpaceTag, typename IoInt>
[[nodiscard]] constexpr io_address<T, SpaceTag, IoInt> align_up(io_address<T, SpaceTag, IoInt> addr) noexcept {
  if (addr.is_null())
    return addr;
  IoInt val = static_cast<IoInt>((addr.value + RegTraits::alignment_mask) & ~IoInt(RegTraits::alignment_mask));
  return io_address<T, SpaceTag, IoInt>{val};
}

/**
 * @brief Computes the address of the `index`-th `RegTraits::reg_size`
 * slot past `base` (`base + index * RegTraits::reg_size`), failing with
 * `error::integer_overflow` instead of wrapping on overflow. `index` may
 * be negative to address a slot *before* `base`.
 */
template <typename RegTraits, typename T, typename SpaceTag, typename IoInt>
[[nodiscard]] constexpr result<io_address<T, SpaceTag, IoInt>> at_reg(io_address<T, SpaceTag, IoInt> base,
                                                                      std::make_signed_t<IoInt> index) noexcept {
  using signed_type = std::make_signed_t<IoInt>;
  auto byte_offset = reloco::checked_mul<signed_type>(index, static_cast<signed_type>(RegTraits::reg_size));
  if (!byte_offset)
    return unexpected(byte_offset.error());
  if (byte_offset.value() >= 0) {
    auto sum = reloco::checked_add<IoInt>(base.value, static_cast<IoInt>(byte_offset.value()));
    if (!sum)
      return unexpected(sum.error());
    return io_address<T, SpaceTag, IoInt>{sum.value()};
  }
  auto magnitude = reloco::checked_neg(byte_offset.value());
  if (!magnitude)
    return unexpected(magnitude.error());
  auto diff = reloco::checked_sub<IoInt>(base.value, static_cast<IoInt>(magnitude.value()));
  if (!diff)
    return unexpected(diff.error());
  return io_address<T, SpaceTag, IoInt>{diff.value()};
}

/**
 * @brief Inverse of `at_reg`: the (signed) number of `RegTraits::reg_size`
 * slots from `base` to `addr`, failing with `error::invalid_argument` if
 * the byte distance is not an exact multiple of `RegTraits::reg_size`, or
 * `error::integer_overflow` if it doesn't fit the signed index type.
 */
template <typename RegTraits, typename T, typename SpaceTag, typename IoInt>
[[nodiscard]] constexpr result<std::make_signed_t<IoInt>> reg_index(io_address<T, SpaceTag, IoInt> addr,
                                                                    io_address<T, SpaceTag, IoInt> base) noexcept {
  using signed_type = std::make_signed_t<IoInt>;
  const bool negative = addr.value < base.value;
  const IoInt byte_diff = negative ? (base.value - addr.value) : (addr.value - base.value);
  if (byte_diff % RegTraits::reg_size != 0)
    return unexpected(error::invalid_argument);
  auto scaled = reloco::checked_cast<signed_type>(byte_diff / RegTraits::reg_size);
  if (!scaled)
    return unexpected(scaled.error());
  if (!negative)
    return scaled.value();
  return reloco::checked_neg(scaled.value());
}

} // namespace io_math

} // namespace structo
