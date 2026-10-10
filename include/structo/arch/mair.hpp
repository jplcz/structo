// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mair.hpp
 * @brief Constexpr builders for the memory-attribute indirection registers: AArch64 `MAIR_ELx` and the
 * AArch32 LPAE `MAIR0`/`MAIR1` pair (same 8 x 8-bit layout), plus `mair_layout`, the policy that tells
 * `vmsa_recursive_format` which index holds which `cache_mode`.
 *
 * @code
 * using namespace structo::arch;
 *
 * // Eight 8-bit attribute slots; the page descriptors select one by AttrIndx[2:0].
 * constexpr mair_value mair = mair_value{}
 *     .set<0>(mair_attr::device_nGnRnE())                        // strongly ordered MMIO
 *     .set<1>(mair_attr::device_nGnRE())                         // normal MMIO (early write ack allowed)
 *     .set<2>(mair_attr::normal(mair_cache::non_cacheable))      // uncached / write-combining RAM
 *     .set<3>(mair_attr::normal(mair_cache::write_through, mair_alloc::read))
 *     .set<4>(mair_attr::normal(mair_cache::write_back, mair_alloc::read_write)); // regular RAM
 *
 * // Tell the remapper formats which index means what: value, then the slot used for write_back,
 * // write_through, uncached, write_combining, device, device_ordered.
 * using layout = mair_layout<mair.raw, 4, 3, 2, 2, 1, 0>;
 *
 * // AArch64: msr mair_el1, mair.raw.   AArch32 LPAE: MAIR0 = mair.lo(), MAIR1 = mair.hi().
 * @endcode
 */

#include <structo/arch/protection.hpp>

#include <cstdint>

namespace structo::arch {

/** Cacheability of a Normal-memory attribute (one nibble: outer or inner). */
enum class mair_cache : std::uint8_t { non_cacheable, write_through, write_back };

/** Read/write allocation hints for cacheable Normal memory. */
enum class mair_alloc : std::uint8_t { none = 0, write = 1, read = 2, read_write = 3 };

/** One 8-bit MAIR attribute (Arm ARM D8.6, "MAIR_ELx"). */
struct mair_attr {
  std::uint8_t raw{0};

  [[nodiscard]] friend constexpr bool operator==(mair_attr a, mair_attr b) noexcept { return a.raw == b.raw; }
  [[nodiscard]] friend constexpr bool operator!=(mair_attr a, mair_attr b) noexcept { return a.raw != b.raw; }

  /** Device-nGnRnE: no gathering, no reordering, no early write acknowledge. */
  [[nodiscard]] static constexpr mair_attr device_nGnRnE() noexcept { return mair_attr{0x00}; }
  /** Device-nGnRE: no gathering, no reordering, early write acknowledge allowed. */
  [[nodiscard]] static constexpr mair_attr device_nGnRE() noexcept { return mair_attr{0x04}; }
  /** Device-nGRE: no gathering, reordering and early acknowledge allowed. */
  [[nodiscard]] static constexpr mair_attr device_nGRE() noexcept { return mair_attr{0x08}; }
  /** Device-GRE: gathering, reordering and early acknowledge allowed. */
  [[nodiscard]] static constexpr mair_attr device_GRE() noexcept { return mair_attr{0x0C}; }

  /** Normal memory with the same cacheability for inner and outer domains. */
  [[nodiscard]] static constexpr mair_attr normal(mair_cache c, mair_alloc a = mair_alloc::none) noexcept {
    return normal(c, a, c, a);
  }

  /** Normal memory with separate outer and inner cacheability. Hints are ignored for `non_cacheable`. */
  [[nodiscard]] static constexpr mair_attr normal(mair_cache outer, mair_alloc outer_alloc, mair_cache inner,
                                                  mair_alloc inner_alloc) noexcept {
    return mair_attr{static_cast<std::uint8_t>((nibble(outer, outer_alloc) << 4) | nibble(inner, inner_alloc))};
  }

  /** Normal Tagged write-back, read/write-allocate (FEAT_MTE). */
  [[nodiscard]] static constexpr mair_attr normal_tagged() noexcept { return mair_attr{0xF0}; }

  [[nodiscard]] constexpr bool is_device() const noexcept { return (raw & 0xF0) == 0; }

private:
  [[nodiscard]] static constexpr unsigned nibble(mair_cache c, mair_alloc a) noexcept {
    const unsigned hint = static_cast<unsigned>(a);
    switch (c) {
    case mair_cache::non_cacheable:
      return 0b0100;
    case mair_cache::write_through:
      return 0b1000 | hint; // non-transient
    case mair_cache::write_back:
      break;
    }
    return 0b1100 | hint; // non-transient
  }
};

/** A full MAIR value: 8 attribute slots, slot `n` in bits [8n+7:8n]. */
struct mair_value {
  std::uint64_t raw{0};

  /** Returns a copy with slot `Index` (0..7) replaced by `attr`. */
  template <unsigned Index> [[nodiscard]] constexpr mair_value set(mair_attr attr) const noexcept {
    static_assert(Index < 8, "MAIR has 8 attribute slots");
    const std::uint64_t mask = std::uint64_t{0xFF} << (Index * 8);
    return mair_value{(raw & ~mask) | (static_cast<std::uint64_t>(attr.raw) << (Index * 8))};
  }

  /** Slot `index` (0..7); out-of-range indexes read as Device-nGnRnE. */
  [[nodiscard]] constexpr mair_attr get(unsigned index) const noexcept {
    return index < 8 ? mair_attr{static_cast<std::uint8_t>(raw >> (index * 8))} : mair_attr{};
  }

  /** First slot holding `attr`, or -1. */
  [[nodiscard]] constexpr int find(mair_attr attr) const noexcept {
    for (unsigned i = 0; i < 8; ++i) {
      if (get(i) == attr) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  /** AArch32 LPAE `MAIR0` (slots 0-3). */
  [[nodiscard]] constexpr std::uint32_t lo() const noexcept { return static_cast<std::uint32_t>(raw); }
  /** AArch32 LPAE `MAIR1` (slots 4-7). */
  [[nodiscard]] constexpr std::uint32_t hi() const noexcept { return static_cast<std::uint32_t>(raw >> 32); }
};

/** Layout used by default: 0 Device-nGnRnE, 1 Device-nGnRE, 2 Normal NC, 3 Normal WT, 4 Normal WB. */
inline constexpr mair_value default_mair_value = mair_value{}
                                                     .set<0>(mair_attr::device_nGnRnE())
                                                     .set<1>(mair_attr::device_nGnRE())
                                                     .set<2>(mair_attr::normal(mair_cache::non_cacheable))
                                                     .set<3>(mair_attr::normal(mair_cache::write_through,
                                                                               mair_alloc::read_write))
                                                     .set<4>(mair_attr::normal(mair_cache::write_back,
                                                                               mair_alloc::read_write));

/** Memory-type policy for `vmsa_recursive_format`: `Value` plus the slot used for each `cache_mode`. */
template <std::uint64_t Value, unsigned WriteBack, unsigned WriteThrough, unsigned Uncached,
          unsigned WriteCombining, unsigned Device, unsigned DeviceOrdered>
struct mair_layout {
  static_assert(WriteBack < 8 && WriteThrough < 8 && Uncached < 8 && WriteCombining < 8 && Device < 8 &&
                    DeviceOrdered < 8,
                "MAIR has 8 attribute slots");

  /** Value to program into `MAIR_ELx` (or `lo()`/`hi()` into `MAIR0`/`MAIR1`). */
  static constexpr std::uint64_t value = Value;

  [[nodiscard]] static constexpr std::uint64_t index_of(cache_mode c) noexcept {
    switch (c) {
    case cache_mode::device_ordered:
      return DeviceOrdered;
    case cache_mode::device:
      return Device;
    case cache_mode::uncached:
      return Uncached;
    case cache_mode::write_combining:
      return WriteCombining;
    case cache_mode::write_through:
      return WriteThrough;
    case cache_mode::write_back:
      break;
    }
    return WriteBack;
  }

  /** Inverse of `index_of`; when several modes share a slot the first of: ordered, device, uncached, WC, WT wins. */
  [[nodiscard]] static constexpr cache_mode cache_of(std::uint64_t index) noexcept {
    if (index == DeviceOrdered) {
      return cache_mode::device_ordered;
    }
    if (index == Device) {
      return cache_mode::device;
    }
    if (index == Uncached) {
      return cache_mode::uncached;
    }
    if (index == WriteCombining) {
      return cache_mode::write_combining;
    }
    if (index == WriteThrough) {
      return cache_mode::write_through;
    }
    return cache_mode::write_back;
  }
};

/** The default layout (`default_mair_value`). */
using default_mair = mair_layout<default_mair_value.raw, 4, 3, 2, 2, 1, 0>;

} // namespace structo::arch
