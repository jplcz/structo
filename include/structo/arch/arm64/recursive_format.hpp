// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file arm64/recursive_format.hpp
 * @brief `arm64::recursive_stage1_format<Granule, Tag, Regime, Policy, Mair>`: AArch64 stage-1 last-level format
 * for `recursive_remapper` (4 KiB: 512 entries, 16 KiB: 2048, 64 KiB: 8192). See `vmsa_recursive_format.hpp`
 * for the protection legalization rules.
 *
 * @code
 * // Default: 4 KiB granule, non-secure, EL1&0, no WXN.
 * using format = structo::arch::arm64::recursive_stage1_format<>;
 * // 64 KiB granule, Secure EL1&0 tables (NS bit follows security_state).
 * using f64 = structo::arch::arm64::recursive_stage1_format<
 *     structo::page_64k, structo::arch::arm64::stage1_secure_tag<structo::page_64k>>;
 * @endcode
 */

#include <structo/arch/arm64/pte_stage1.hpp>
#include <structo/arch/vmsa_recursive_format.hpp>

namespace structo::arch::arm64 {

template <typename Granule = structo::page_4k, typename Tag = stage1_ns_tag<Granule>,
          vmsa_regime Regime = vmsa_regime::el1_el0, typename Policy = default_mmu_policy,
          typename Mair = default_mair>
using recursive_stage1_format = vmsa_recursive_format<Granule, Tag, Regime, Policy, Mair>;

} // namespace structo::arch::arm64
