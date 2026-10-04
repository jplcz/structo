// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file domain_space_traits.hpp
 * @brief `structo::arch::domain_space_traits<Domain>` and
 * `structo::arch::space_domain_of<SpaceTag>` -- translating a
 * `structo::arch::execution_domain` into the matching empty tag struct
 * in each of this library's independent "which world/space does this
 * address/TLB entry/register belong to" tag families, and back again.
 *
 * Four modules each define their own family of empty tag structs for
 * exactly this purpose, chosen independently because each solves a
 * different problem (a TLB entry, a physical address, an MMIO register
 * window, a *virtual* address in someone else's address space) and so
 * has a different, non-overlapping set of `template <typename
 * SpaceTag>` consumers:
 *
 * - `arch/tlb_flush.hpp` / `arch/address_translate.hpp`:
 *   `process_tlb_space`, `guest_tlb_space`, `hypervisor_tlb_space`,
 *   `secure_tlb_space`, `nonsecure_tlb_space`, `root_tlb_space`,
 *   `realm_tlb_space`, `gpt_tlb_space`.
 * - `phys_addr.hpp`: `default_phys_space`, `host_phys_space`,
 *   `guest_phys_space`, `dma_bus_space`, `secure_phys_space`,
 *   `nonsecure_phys_space`, `root_phys_space`, `realm_phys_space`.
 * - `io_address.hpp`: `default_io_space`, `port_io_space`,
 *   `device_io_space`, `secure_io_space`, `nonsecure_io_space`,
 *   `realm_io_space`, `hypervisor_io_space`.
 * - `target_ptr.hpp`: `user_space`, `kernel_space`, `guest_vm_space`,
 *   `realm_space`, `secure_world_space`.
 *
 * Nothing ties these four families together on its own -- a caller
 * holding a `secure_tlb_space`-tagged flush and wanting the matching
 * `phys_addr` space tag (`secure_phys_space`) for the same world
 * previously had no choice but to know, and spell out, that
 * correspondence itself at every call site. This header is that single,
 * shared correspondence table, built on `execution_domain` (see
 * `arch/execution_domain.hpp`) as the common key every family's tags are
 * expressed in terms of.
 *
 * ## Not every `Domain`/family pairing has a tag to offer
 *
 * Only `execution_domain::secure` has a representative tag in all four
 * families (`secure_tlb_space`, `secure_phys_space`, `secure_io_space`,
 * `secure_world_space`) -- every other domain is missing at least one:
 * neither `monitor` (EL3) nor `hypervisor` (EL2) has a dedicated
 * `target_ptr.hpp` virtual-address-space tag of its own (EL3 code
 * generally has no distinct "virtual address space" concept worth
 * tagging; EL2's own stage-1 virtual addresses have no tag because
 * nothing in this library yet needs to name them specifically, unlike
 * EL2's *physical*-address view, `host_phys_space`, which `phys_
 * translator.hpp` policies already use); `monitor` has no `io_address.hpp`
 * tag either (no "Root-world-only MMIO alias" concept exists yet).
 * `nonsecure` likewise has no virtual-address-space tag -- `user_space`/
 * `kernel_space` are the ordinary, implicitly-Non-secure case in a
 * system with no TrustZone split at all, so neither can be picked as
 * *the* Non-secure-world tag without being arbitrary and wrong half the
 * time. `secure_hypervisor` (`FEAT_SEL2`) only has a TLB-space tag,
 * because it is architecturally the same `AT S1E2*`/`AT S12E1*`
 * encoding as plain `hypervisor_tlb_space` (see
 * `arch/arm64/address_translate.hpp`), but no Secure-world-specific
 * `phys_addr`/`io_address` tag exists yet to distinguish a Secure EL2's
 * own physical/MMIO view from a Non-secure EL2's.
 *
 * `execution_domain::unspecified` intentionally has no
 * `domain_space_traits` specialization at all (a hard compile error if
 * instantiated, not a silently-empty one) -- there is no tag to
 * translate it into in the first place.
 *
 * `guest_*_space`/`guest_vm_space`, `realm_*_space`, `dma_bus_space`,
 * `default_*_space`, `port_io_space`, `device_io_space`,
 * `untagged_tlb_space`, `user_space`, `kernel_space`, and
 * `gpt_tlb_space` are deliberately not reachable through either
 * direction of this header at all: none of them names a *privilege
 * world/mode a translation unit is compiled to run as* (the thing
 * `execution_domain` enumerates) -- they name a role (the guest, a DMA
 * initiator, an unspecified default) or a library-wide-out-of-scope
 * extension (`FEAT_RME`'s Realm/Root-Granule-Protection-Table concepts,
 * beyond `root_tlb_space`/`root_phys_space`'s plain Monitor-world
 * reuse) instead.
 */

#include <structo/arch/execution_domain.hpp>
#include <structo/arch/tlb_flush.hpp>
#include <structo/io_address.hpp>
#include <structo/phys_addr.hpp>
#include <structo/target_ptr.hpp>

namespace structo::arch {

/**
 * @brief Translates an `execution_domain` into the matching tag struct
 * in each of `tlb_flush.hpp`/`address_translate.hpp` (`tlb_space`),
 * `phys_addr.hpp` (`phys_space`), `io_address.hpp` (`io_space`), and
 * `target_ptr.hpp` (`virt_space`). See the @file docs for which members
 * exist for which `Domain`, and why the missing ones are missing rather
 * than filled in with an arbitrary guess.
 *
 * Left undefined for any `Domain` with no tag in any family
 * (`execution_domain::unspecified`) -- a hard compile error, same
 * not-silently-wrong precedent as `address_translate_traits<Arch>`.
 */
template <execution_domain Domain> struct domain_space_traits;

/** @brief ARM TrustZone Secure world: the one `Domain` with a tag in every family. */
template <> struct domain_space_traits<execution_domain::secure> {
  using tlb_space = secure_tlb_space;
  using phys_space = structo::secure_phys_space;
  using io_space = structo::secure_io_space;
  using virt_space = structo::secure_world_space;
};

/** @brief ARM TrustZone Non-secure world. No `virt_space` member -- see the @file docs. */
template <> struct domain_space_traits<execution_domain::nonsecure> {
  using tlb_space = nonsecure_tlb_space;
  using phys_space = structo::nonsecure_phys_space;
  using io_space = structo::nonsecure_io_space;
};

/** @brief ARM EL3/Monitor mode. No `io_space`/`virt_space` member -- see the @file docs. */
template <> struct domain_space_traits<execution_domain::monitor> {
  using tlb_space = root_tlb_space;
  using phys_space = structo::root_phys_space;
};

/** @brief ARM Non-secure Hyp mode/EL2. No `virt_space` member -- see the @file docs. */
template <> struct domain_space_traits<execution_domain::hypervisor> {
  using tlb_space = hypervisor_tlb_space;
  using phys_space = structo::host_phys_space;
  using io_space = structo::hypervisor_io_space;
};

/** @brief ARM Secure EL2 (`FEAT_SEL2`). Only a `tlb_space` member -- see the @file docs. */
template <> struct domain_space_traits<execution_domain::secure_hypervisor> {
  using tlb_space = hypervisor_tlb_space;
};

/**
 * @brief `domain_space_traits<current_execution_domain>` -- the
 * correspondence table for whichever `Domain` this translation unit was
 * built for (see `structo_config.hpp`'s `STRUCTO_DOMAIN_*` family).
 * Ill-formed (no such specialization) if built with no `STRUCTO_DOMAIN_*`
 * macro defined at all, same as instantiating `domain_space_traits<
 * execution_domain::unspecified>` directly would be.
 */
using current_domain_spaces = domain_space_traits<current_execution_domain>;

namespace detail {

// Reverse lookup backing `space_domain_of`: one explicit specialization
// per tag that has an unambiguous `execution_domain`, built directly
// from `domain_space_traits`'s own members above so the two directions
// cannot silently drift apart from each other.
template <typename SpaceTag> struct space_domain_of_impl {
  static constexpr execution_domain value = execution_domain::unspecified;
};

#define STRUCTO_DETAIL_DOMAIN_SPACE_OF(space_tag, domain)                                                              \
  template <> struct space_domain_of_impl<space_tag> {                                                                 \
    static constexpr execution_domain value = domain;                                                                  \
  }

STRUCTO_DETAIL_DOMAIN_SPACE_OF(secure_tlb_space, execution_domain::secure);
STRUCTO_DETAIL_DOMAIN_SPACE_OF(structo::secure_phys_space, execution_domain::secure);
STRUCTO_DETAIL_DOMAIN_SPACE_OF(structo::secure_io_space, execution_domain::secure);
STRUCTO_DETAIL_DOMAIN_SPACE_OF(structo::secure_world_space, execution_domain::secure);

STRUCTO_DETAIL_DOMAIN_SPACE_OF(nonsecure_tlb_space, execution_domain::nonsecure);
STRUCTO_DETAIL_DOMAIN_SPACE_OF(structo::nonsecure_phys_space, execution_domain::nonsecure);
STRUCTO_DETAIL_DOMAIN_SPACE_OF(structo::nonsecure_io_space, execution_domain::nonsecure);

STRUCTO_DETAIL_DOMAIN_SPACE_OF(root_tlb_space, execution_domain::monitor);
STRUCTO_DETAIL_DOMAIN_SPACE_OF(structo::root_phys_space, execution_domain::monitor);

// hypervisor_tlb_space is deliberately not registered here: it is shared
// verbatim between execution_domain::hypervisor and ::secure_hypervisor
// (see arch/arm64/address_translate.hpp), so it has no single unambiguous
// reverse mapping -- space_domain_of<hypervisor_tlb_space> therefore
// reports execution_domain::unspecified rather than silently picking one
// of the two. Query domain_space_traits<execution_domain::hypervisor>/
// <execution_domain::secure_hypervisor>::tlb_space directly instead.
STRUCTO_DETAIL_DOMAIN_SPACE_OF(structo::host_phys_space, execution_domain::hypervisor);
STRUCTO_DETAIL_DOMAIN_SPACE_OF(structo::hypervisor_io_space, execution_domain::hypervisor);

#undef STRUCTO_DETAIL_DOMAIN_SPACE_OF

} // namespace detail

/**
 * @brief Matches a tag struct from any of the four families back to the
 * `execution_domain` it represents, the reverse of `domain_space_traits`.
 * `execution_domain::unspecified` for any tag with no unambiguous domain
 * (including every tag the @file docs list as deliberately unreachable,
 * and `hypervisor_tlb_space` -- see `detail::space_domain_of_impl`'s own
 * comment), not a compile error: unlike `domain_space_traits`, a caller
 * matching an arbitrary, possibly-unrelated tag against a domain is the
 * expected use (see `same_domain_v` below), not a programming mistake.
 */
template <typename SpaceTag>
inline constexpr execution_domain space_domain_of = detail::space_domain_of_impl<SpaceTag>::value;

/**
 * @brief `true` if `TagA` and `TagB` -- each from any of the four tag
 * families `domain_space_traits`/`space_domain_of` cover, in any
 * combination, including `TagA == TagB` -- represent the same, known
 * `execution_domain`. Always `false` if either matches no domain
 * (`space_domain_of<Tag> == execution_domain::unspecified`), so two
 * unrelated domain-agnostic tags (e.g. two different `default_phys_space`
 * uses for genuinely different platforms) never compare equal by this
 * metric merely for being the same type.
 */
template <typename TagA, typename TagB>
inline constexpr bool same_domain_v =
    space_domain_of<TagA> == space_domain_of<TagB> && space_domain_of<TagA> != execution_domain::unspecified;

} // namespace structo::arch
