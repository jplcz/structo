// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_table_traits.hpp
 * @brief Ready-made `structo::arch::page_table_levels<...>` configurations
 * for the RISC-V privileged architecture's three canonical paged virtual
 * memory formats: Sv39, Sv48, and Sv57 (RV64 only -- Sv32, RV32's 2-level
 * format, is not provided here since this library otherwise targets
 * 64-bit address spaces throughout).
 *
 * ## Shared shape
 *
 * All three formats share the same per-level shape: a 4KB granule, 9
 * index bits per level (512-entry tables, one 4KB page each), and the
 * root level's index field immediately followed by the next lower
 * format's levels, each one scaled down by exactly one level from the
 * wider format -- Sv48 is Sv39 with one extra level prepended, and Sv57
 * is Sv48 with one more. Picking the format is purely a `satp.MODE`
 * configuration choice; the level count and VA width are otherwise
 * unrelated to any other architectural parameter.
 *
 * ## Every level allows an early leaf (unlike ARM64/x86)
 *
 * The RISC-V spec permits a leaf PTE (megapage/gigapage/terapage/
 * petapage) at **any** level, including the root -- there is no level
 * that is architecturally restricted to always pointing at a next-level
 * table the way ARM64's level 0 or x86-64's PML4/PML5 are. Every level
 * in every configuration below therefore uses `AllowsLeaf=true`.
 *
 * Entry bit-layout is left to a `page_table_entry_traits<Tag>`
 * specialization, not provided here.
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/phys_page.hpp>

namespace structo::arch::riscv {

/** @brief Sv39: 3-level, 39-bit VA, 512GB of address space. */
using sv39 = page_table_levels<structo::page_4k, 39, page_table_level<9, 30, true>,
                                page_table_level<9, 21, true>, page_table_level<9, 12, true>>;

/** @brief Sv48: 4-level, 48-bit VA, 256TB of address space. */
using sv48 =
    page_table_levels<structo::page_4k, 48, page_table_level<9, 39, true>, page_table_level<9, 30, true>,
                       page_table_level<9, 21, true>, page_table_level<9, 12, true>>;

/** @brief Sv57: 5-level, 57-bit VA, 128PB of address space. */
using sv57 = page_table_levels<structo::page_4k, 57, page_table_level<9, 48, true>,
                                page_table_level<9, 39, true>, page_table_level<9, 30, true>,
                                page_table_level<9, 21, true>, page_table_level<9, 12, true>>;

} // namespace structo::arch::riscv
