// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file structo_config.hpp
 * @brief Single build-time customization entry point for every optional
 * structo feature macro -- mirrors `reloco/reloco_config.hpp`'s role and
 * conventions in the reloco library this one is built on.
 *
 * Include this header first, before anything else, from any structo
 * header that wants to honor a customization point defined here (see
 * `arch/execution_domain.hpp` for the one that exists so far); doing so
 * makes it the earliest point at which configuration can be injected,
 * regardless of which structo header an application includes first.
 *
 * User overrides must not be made by editing this file. Instead, supply
 * `detail/porting/structo_user_config.hpp` at the same fixed path in your
 * own include tree (there is no CMake-driven copy step for it yet, unlike
 * reloco's `JPLCZ_RELOCO_PORTING_HEADERS` -- add the directory containing
 * it to your own include path). When that header is present, it is
 * included here, before any of the `#ifndef`-guarded defaults below are
 * applied, so every `#define` it contains takes precedence.
 *
 * Every macro below may alternatively be set directly with a compiler
 * `-D` flag instead of (or in addition to) `structo_user_config.hpp`;
 * both approaches are equivalent since this header only ever applies a
 * default when the macro is not already defined.
 *
 * `structo_user_config.hpp` must not `#include` any structo header
 * (directly or transitively), same rationale as reloco's own
 * `reloco_user_config.hpp`: it is reached from here, at the very top of
 * whichever structo header chain included `structo_config.hpp` first, so
 * a structo header pulled in from here would re-enter that chain while
 * it is still on the include stack, with `#pragma once` silently
 * skipping it.
 */

#if defined(__has_include)
#if __has_include("detail/porting/structo_user_config.hpp")
#include "detail/porting/structo_user_config.hpp"
#endif
#endif

// ============================================================================
// Available customization points
// ============================================================================
//
// STRUCTO_DOMAIN_SECURE / STRUCTO_DOMAIN_NONSECURE / STRUCTO_DOMAIN_MONITOR /
// STRUCTO_DOMAIN_HYPERVISOR / STRUCTO_DOMAIN_SECURE_HYPERVISOR
//     Define at most one (any value) to tell structo which privilege
//     world/mode this translation unit -- and everything it pulls in
//     transitively from arch/execution_domain.hpp -- is being compiled
//     to run as:
//       - STRUCTO_DOMAIN_SECURE: ARM TrustZone Secure world, PL1/EL1
//         (an ordinary kernel built to run as the Secure OS).
//       - STRUCTO_DOMAIN_NONSECURE: ARM TrustZone Non-secure world,
//         PL1/EL1 (an ordinary kernel, or a hypervisor guest).
//       - STRUCTO_DOMAIN_MONITOR: ARM EL3/Monitor mode (Secure Monitor
//         firmware, e.g. a TF-A-like BL31).
//       - STRUCTO_DOMAIN_HYPERVISOR: ARM Non-secure Hyp mode/EL2.
//       - STRUCTO_DOMAIN_SECURE_HYPERVISOR: ARM Secure EL2 (`FEAT_SEL2`,
//         AArch64 only -- see arch/arm64/address_translate.hpp).
//     Resolves to the structo::arch::execution_domain enumerator
//     structo::arch::current_execution_domain is set to (see
//     arch/execution_domain.hpp). Defining more than one of these is a
//     hard #error, caught right here rather than surfacing as a
//     confusing redefinition error somewhere downstream.
//
//     Left undefined (the default), structo::arch::current_execution_domain
//     is structo::arch::execution_domain::unspecified -- today, this is
//     purely informational: no structo header currently gates any
//     Space-tagged operation (arch/tlb_flush.hpp's tlb_flusher<Arch>,
//     arch/address_translate.hpp's address_translator<Arch>, ...) on it.
//     It exists so a caller can record this fact once, in one place, for
//     its own use and for any future structo customization point that
//     wants to key off of it -- e.g. restricting which `Space` tags
//     (arch/tlb_flush.hpp's hypervisor_tlb_space/guest_tlb_space/
//     nonsecure_tlb_space/secure_tlb_space, which are only architecturally
//     valid to target from specific privilege modes) are even
//     instantiable for a given build.
#if defined(STRUCTO_DOMAIN_SECURE) + defined(STRUCTO_DOMAIN_NONSECURE) + defined(STRUCTO_DOMAIN_MONITOR) +           \
        defined(STRUCTO_DOMAIN_HYPERVISOR) + defined(STRUCTO_DOMAIN_SECURE_HYPERVISOR) >                             \
    1
#error "structo_config.hpp: define at most one of STRUCTO_DOMAIN_SECURE/STRUCTO_DOMAIN_NONSECURE/" \
       "STRUCTO_DOMAIN_MONITOR/STRUCTO_DOMAIN_HYPERVISOR/STRUCTO_DOMAIN_SECURE_HYPERVISOR"
#endif
