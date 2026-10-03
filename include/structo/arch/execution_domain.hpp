// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file execution_domain.hpp
 * @brief `structo::arch::execution_domain` -- the privilege world/mode a
 * translation unit is being compiled to run as, per the
 * `STRUCTO_DOMAIN_*` family of customization points in
 * `structo_config.hpp`.
 *
 * This is scaffolding for **future** use, not a gate applied anywhere
 * yet: `arch/tlb_flush.hpp`'s `tlb_flusher<Arch>` and
 * `arch/address_translate.hpp`'s `address_translator<Arch>` do not
 * currently restrict which `Space` tag a caller may instantiate based on
 * `current_execution_domain` below, even though several tags
 * (`hypervisor_tlb_space`, `guest_tlb_space`, `nonsecure_tlb_space`,
 * `secure_tlb_space`) are only architecturally valid to target from
 * specific privilege modes, as their own docs describe. A caller who
 * wants that enforced today still needs its own `static_assert`/runtime
 * check; this header only gives every translation unit a single, shared,
 * build-time-configured place to ask "which world am I?" so such a check
 * -- here or added to the two headers above later -- has one canonical
 * answer to consult instead of each caller inventing its own.
 */

#include <structo/structo_config.hpp>

namespace structo::arch {

/**
 * @brief Which privilege world/mode this translation unit is being
 * compiled to run as. See `structo_config.hpp`'s `STRUCTO_DOMAIN_*`
 * family for how a caller sets this.
 */
enum class execution_domain {
  /** @brief No `STRUCTO_DOMAIN_*` macro was defined -- the default, and the only value this library itself ever assumes. */
  unspecified,
  /** @brief ARM TrustZone Secure world, PL1/EL1. */
  secure,
  /** @brief ARM TrustZone Non-secure world, PL1/EL1. */
  nonsecure,
  /** @brief ARM EL3/Monitor mode. */
  monitor,
  /** @brief ARM Non-secure Hyp mode/EL2. */
  hypervisor,
  /** @brief ARM Secure EL2 (`FEAT_SEL2`, AArch64 only). */
  secure_hypervisor,
};

#if defined(STRUCTO_DOMAIN_SECURE)
inline constexpr execution_domain current_execution_domain = execution_domain::secure;
#elif defined(STRUCTO_DOMAIN_NONSECURE)
inline constexpr execution_domain current_execution_domain = execution_domain::nonsecure;
#elif defined(STRUCTO_DOMAIN_MONITOR)
inline constexpr execution_domain current_execution_domain = execution_domain::monitor;
#elif defined(STRUCTO_DOMAIN_HYPERVISOR)
inline constexpr execution_domain current_execution_domain = execution_domain::hypervisor;
#elif defined(STRUCTO_DOMAIN_SECURE_HYPERVISOR)
inline constexpr execution_domain current_execution_domain = execution_domain::secure_hypervisor;
#else
inline constexpr execution_domain current_execution_domain = execution_domain::unspecified;
#endif

} // namespace structo::arch
