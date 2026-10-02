// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_table_traits.hpp
 * @brief Ready-made `structo::arch::page_table_levels<...>` configurations
 * for x86/x86-64 paging: 32-bit non-PAE, 32-bit PAE, 64-bit long-mode
 * 4-level, and 64-bit long-mode 5-level (LA57).
 *
 * ## The four formats
 *
 * - **`x86::i386`** -- classic 32-bit, non-PAE paging: 2 levels, 10
 *   index bits each (1024-entry tables), 32-bit VA. The root level (PDE)
 *   may terminate early with a 4MB "PSE" page.
 * - **`x86::pae`** -- 32-bit PAE paging: 3 levels, but the root (PDPTE)
 *   has only 2 index bits (4 entries) since PAE still has a 32-bit input
 *   address; it also may **not** terminate early (32-bit PAE has no
 *   1GB-page support). The middle level (PDE) may terminate early with a
 *   2MB page, same as every other format here.
 * - **`x86::long_mode_4level`** -- the long-mode default: 4 levels,
 *   9 index bits each, 48-bit canonical VA. The root (PML4) may not
 *   terminate early; the next level (PDPTE) may, with a 1GB page, if the
 *   CPU supports the Page1GB feature; the level below that (PDE) may,
 *   with a 2MB page.
 * - **`x86::long_mode_5level`** -- LA57 extends long mode with a fifth
 *   level (PML5) above PML4, widening the canonical VA to 57 bits.
 *   Neither PML5 nor PML4 may terminate early.
 *
 * All four share the same 4KB leaf page size; none of them disagree
 * with each other below the level that first allows an early leaf.
 *
 * Entry bit-layout is left to a `page_table_entry_traits<Tag>`
 * specialization, not provided here.
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/phys_page.hpp>

namespace structo::arch::x86 {

/**
 * @brief Classic 32-bit, non-PAE paging: 2-level, 10-bit index fields,
 * 32-bit VA. The root (PDE) may block-map a 4MB "PSE" page.
 */
using i386 = page_table_levels<structo::page_4k, 32, page_table_level<10, 22, true>, page_table_level<10, 12, true>>;

/**
 * @brief 32-bit PAE paging: 3-level, 32-bit VA. The root (PDPTE) may
 * not block-map (no 1GB pages in 32-bit PAE); the PDE level may
 * block-map a 2MB page.
 */
using pae =
    page_table_levels<structo::page_4k, 32, page_table_level<2, 30, false>, // PDPTE: no early leaf in 32-bit PAE
                      page_table_level<9, 21, true>,                        // PDE: may block-map a 2MB page
                      page_table_level<9, 12, true>                         // PTE: always a 4KB page
                      >;

/**
 * @brief 64-bit long-mode, 4-level paging: 48-bit canonical VA. PML4
 * may not block-map; PDPTE may, with a 1GB page (requires the Page1GB
 * CPU feature); PDE may, with a 2MB page.
 */
using long_mode_4level = page_table_levels<structo::page_4k, 48, page_table_level<9, 39, false>, // PML4: no early leaf
                                           page_table_level<9, 30, true>, // PDPTE: may block-map a 1GB page (Page1GB)
                                           page_table_level<9, 21, true>, // PDE: may block-map a 2MB page
                                           page_table_level<9, 12, true>  // PTE: always a 4KB page
                                           >;

/**
 * @brief 64-bit long-mode with LA57, 5-level paging: 57-bit canonical
 * VA. PML5 and PML4 may not block-map; PDPTE/PDE behave as in
 * `long_mode_4level`.
 */
using long_mode_5level = page_table_levels<structo::page_4k, 57, page_table_level<9, 48, false>, // PML5: no early leaf
                                           page_table_level<9, 39, false>,                       // PML4: no early leaf
                                           page_table_level<9, 30, true>, // PDPTE: may block-map a 1GB page (Page1GB)
                                           page_table_level<9, 21, true>, // PDE: may block-map a 2MB page
                                           page_table_level<9, 12, true>  // PTE: always a 4KB page
                                           >;

} // namespace structo::arch::x86
