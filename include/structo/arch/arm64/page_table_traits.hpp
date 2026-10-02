// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_table_traits.hpp
 * @brief Ready-made `structo::arch::page_table_levels<...>` configurations
 * for ARMv8-A/AArch64 (VMSAv8-64), covering all three translation
 * granules the architecture defines (4KB, 16KB, 64KB), for the most
 * common translation-table starting levels of each.
 *
 * ## Naming
 *
 * `arm64::level1`/`arm64::level2` (no suffix) are the 4KB-granule
 * configurations; `_16k`/`_64k` suffixes select the other two granules
 * (e.g. `arm64::level1_16k`, `arm64::level2_64k`). The unsuffixed names
 * are kept exactly as they were before 16KB/64KB support was added, so
 * existing code that assumes a 4KB granule keeps compiling unchanged.
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
 * Which levels permit an early-terminating block mapping is granule-
 * dependent, not just "every level but the leaf":
 *
 * - **4KB granule**: level 1 (1GB blocks) and level 2 (2MB blocks) both
 *   permit it.
 * - **16KB granule**: only level 2 (32MB blocks) permits it -- level 1
 *   does not, in the baseline architecture.
 * - **64KB granule**: only level 2 (512MB blocks) permits it -- level 1
 *   does not, in the baseline architecture.
 *
 * Level 0 never permits it for any granule (there is no such thing as a
 * sub-page-table-sized "block" that large relative to the VA space it
 * covers), which is why it is absent from every configuration below --
 * none of them ever includes a level-0 entry.
 *
 * The 16KB and 64KB granules also use wider per-level index fields than
 * 4KB (11 bits and 13 bits respectively, vs. 9 bits for 4KB), because
 * each level's table is exactly one page of the active granule, and a
 * bigger page holds more 8-byte entries. The 64KB/3-level configuration
 * (`level1_64k`) is the one exception: its root level only needs 6
 * index bits (64 entries) to reach 48-bit VA, so the architecture lets
 * that top-level table be a "folded"/truncated 64-entry table rather
 * than a full 8192-entry, 64KB one -- saving 63 x 8KB of otherwise-wasted
 * table memory for a config that would otherwise barely use its root
 * table at all.
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

/**
 * @brief 16KB granule, level-1-rooted, 3-level configuration (47-bit
 * input address, 128TB VA space per TTBRn).
 */
using level1_16k =
    page_table_levels<structo::page_16k, 47, page_table_level<11, 36, false>, // L1: no block mapping for this granule
                      page_table_level<11, 25, true>,                         // L2: may block-map a 32MB region
                      page_table_level<11, 14, true>                          // L3: always a 16KB page
                      >;

/**
 * @brief 16KB granule, level-2-rooted, 2-level configuration (36-bit
 * input address, 64GB VA space per TTBRn).
 */
using level2_16k =
    page_table_levels<structo::page_16k, 36, page_table_level<11, 25, true>, // L2: may block-map a 32MB region
                      page_table_level<11, 14, true>                         // L3: always a 16KB page
                      >;

/**
 * @brief 64KB granule, level-1-rooted, 3-level configuration (48-bit
 * input address, 256TB VA space per TTBRn). The root (level 1) table is
 * "folded": it only needs 6 index bits (64 entries), not the full 13
 * bits a non-root 64KB-granule table would have.
 */
using level1_64k = page_table_levels<structo::page_64k, 48,
                                     page_table_level<6, 42, false>, // L1 (folded): no block mapping for this granule
                                     page_table_level<13, 29, true>, // L2: may block-map a 512MB region
                                     page_table_level<13, 16, true>  // L3: always a 64KB page
                                     >;

/**
 * @brief 64KB granule, level-2-rooted, 2-level configuration (42-bit
 * input address, 4TB VA space per TTBRn).
 */
using level2_64k =
    page_table_levels<structo::page_64k, 42, page_table_level<13, 29, true>, // L2: may block-map a 512MB region
                      page_table_level<13, 16, true>                         // L3: always a 64KB page
                      >;

} // namespace structo::arch::arm64
