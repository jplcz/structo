// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_table_traits.hpp
 * @brief Ready-made `structo::arch::page_table_levels<...>` configurations
 * for ARMv8-A/AArch64 (VMSAv8-64), 4KB translation granule, for the two
 * most common translation-table starting levels.
 *
 * ## Why "level 1" and "level 2"
 *
 * The AArch64 translation table walk does not always start at level 0:
 * the architecture lets `TCR_ELx.{T0SZ,T1SZ}` shrink the input address
 * size so the walk starts at whichever level's table already covers the
 * full address range, skipping the (otherwise wasted) levels above it.
 * For a 4KB granule specifically, the two configurations every general-
 * purpose kernel actually ships are:
 *
 * - **Level 1 root, 3 levels, 39-bit input address** (`arm64::level1`):
 *   512GB of VA space per translation table base register. This is the
 *   default 3-level layout most 64-bit kernels use for each of TTBR0/
 *   TTBR1 (e.g. Linux's `CONFIG_ARM64_VA_BITS=39`).
 * - **Level 2 root, 2 levels, 30-bit input address** (`arm64::level2`):
 *   1GB of VA space per translation table base register -- small enough
 *   that levels 0 and 1 would only ever have a single entry, so the
 *   architecture lets the walk skip straight to level 2. Used for
 *   address spaces that are known in advance to be small (e.g. some
 *   EL2/EL3 stage-1 mappings, or a reduced-size stage-2 IPA space).
 *
 * A level-0-rooted, full 4-level, 48-bit configuration is also legal
 * (512GB x 512 = 256TB of VA space) -- see `page_table_traits.hpp`'s own
 * file-level `@code` example, which already demonstrates exactly that
 * shape generically; it is not repeated here since it adds nothing
 * `arm64::level1` doesn't already show about how to assemble one.
 *
 * Every level below the root, down to (but not including) the leaf-
 * adjacent level, permits an early-terminating block mapping (1GB blocks
 * at level 1, 2MB blocks at level 2); level 0 never does (there is no
 * such thing as a 512GB block), which is why it is absent from both
 * configurations below -- neither ever includes a level-0 entry.
 *
 * Entry bit-layout (which bit means "present", how the output address is
 * packed, access permissions, ...) is deliberately not covered here --
 * that is `page_table_entry_traits<Tag>`'s job, same as in
 * `page_table_traits.hpp`.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch;
 * void decompose_l1(std::uint64_t va) {
 *   auto l1 = arm64::level1::index_of<0>(va); // bits [38:30]
 *   auto l2 = arm64::level1::index_of<1>(va); // bits [29:21]
 *   auto l3 = arm64::level1::index_of<2>(va); // bits [20:12]
 *   (void)l1; (void)l2; (void)l3;
 * }
 * @endcode
 */

#include <structo/arch/page_table_traits.hpp>
#include <structo/phys_page.hpp>

namespace structo::arch::arm64 {

/**
 * @brief 4KB granule, level-1-rooted, 3-level configuration (39-bit
 * input address, 512GB VA space per TTBRn).
 */
using level1 = page_table_levels<structo::page_4k, 39, page_table_level<9, 30, true>, // L1: may block-map a 1GB region
                                  page_table_level<9, 21, true>,                       // L2: may block-map a 2MB region
                                  page_table_level<9, 12, true>                        // L3: always a 4KB page
                                  >;

/**
 * @brief 4KB granule, level-2-rooted, 2-level configuration (30-bit
 * input address, 1GB VA space per TTBRn).
 */
using level2 = page_table_levels<structo::page_4k, 30, page_table_level<9, 21, true>, // L2: may block-map a 2MB region
                                  page_table_level<9, 12, true>                        // L3: always a 4KB page
                                  >;

} // namespace structo::arch::arm64
