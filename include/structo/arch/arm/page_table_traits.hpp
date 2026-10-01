// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_table_traits.hpp
 * @brief Ready-made `structo::arch::page_table_levels<...>` configurations
 * for 32-bit ARMv7-A's LPAE ("Large Physical Address Extension") long-
 * descriptor translation table format, 4KB granule (the only granule
 * LPAE supports).
 *
 * ## Why "lpae" and not plain "arm"
 *
 * ARMv7-A has two entirely different, mutually exclusive page-table
 * encodings: the classic "short-descriptor" format (2-level, 32-bit
 * descriptors, no `page_table_entry_traits` here yet) and LPAE's "long-
 * descriptor" format (64-bit descriptors, same index-field shape as
 * ARM64's VMSAv8-64 since LPAE was VMSAv8's direct predecessor). Only
 * the latter is provided here; the namespace is named accordingly so a
 * future short-descriptor configuration can live alongside it as
 * `structo::arch::arm::short_descriptor` without any ambiguity.
 *
 * ## Why "level 1" and "level 2"
 *
 * Like VMSAv8-64, LPAE lets `TTBCR.{T0SZ,T1SZ}` shrink the walk's input
 * address so it starts below the nominal root level when the full
 * 32-bit VA space isn't needed:
 *
 * - **Level 1 root, 3 levels, full 32-bit input address**
 *   (`arm::lpae::level1`): the standard configuration for a full-size
 *   32-bit address space. Level 1 has only 2 index bits (4 entries)
 *   here, not 9 -- a 32-bit input address leaves only 2 bits above
 *   level 2's 21-bit shift, unlike VMSAv8-64's wider root.
 * - **Level 2 root, 2 levels, 30-bit input address**
 *   (`arm::lpae::level2`): 1GB of VA space, for address spaces known in
 *   advance to be small (e.g. some Hyp-mode or stage-2 configurations).
 *   This happens to have the identical shape to `arm64::level2` since
 *   both share LPAE's level-2/level-3 field widths.
 *
 * Entry bit-layout is, as elsewhere in this family of headers, left to
 * a `page_table_entry_traits<Tag>` specialization -- not provided here.
 *
 * @see structo/arch/arm64/page_table_traits.hpp for the VMSAv8-64
 *      (AArch64) equivalent this format's level-2 shape coincides with.
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/phys_page.hpp>

namespace structo::arch::arm::lpae {

/**
 * @brief LPAE, level-1-rooted, 3-level configuration (full 32-bit input
 * address).
 */
using level1 = page_table_levels<structo::page_4k, 32,
                                  page_table_level<2, 30, true>, // L1: may block-map a 1GB region; only 4 entries (32-bit VA)
                                  page_table_level<9, 21, true>, // L2: may block-map a 2MB region
                                  page_table_level<9, 12, true>  // L3: always a 4KB page
                                  >;

/**
 * @brief LPAE, level-2-rooted, 2-level configuration (30-bit input
 * address, 1GB VA space).
 */
using level2 = page_table_levels<structo::page_4k, 30,
                                  page_table_level<9, 21, true>, // L2: may block-map a 2MB region
                                  page_table_level<9, 12, true>  // L3: always a 4KB page
                                  >;

} // namespace structo::arch::arm::lpae
