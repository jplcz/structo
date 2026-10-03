// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file address_translate.hpp
 * @brief Per-architecture, Space-tagged *hardware address translation*
 * customization point -- `structo::arch::address_translate_traits<Arch>`
 * (filled in by each `arch/<arch>/address_translate.hpp`) and
 * `structo::arch::address_translator<Arch>` (the public, caller-facing
 * dispatcher built on top of it).
 *
 * This asks the MMU itself "what would happen if I accessed this
 * virtual address for {read, write} right now, in this @ref
 * structo::arch::process_tlb_space "Space"?" -- e.g. ARM's `AT`
 * (AArch64)/CP15 `ATS1*`/`ATS12NSO*` (AArch32) System instructions --
 * rather than walking page tables in software. The CPU performs the
 * full privilege/permission check exactly as a real access would, and
 * reports either the resulting output address plus its attributes, or
 * why the access would fault -- all without actually touching memory or
 * raising a fault, and (critically) without the caller having to
 * reimplement the page-table-walk logic (short/long descriptor format,
 * `FEAT_LPA2`, stage-2 combination, ...) itself.
 *
 * Reuses the same `Space` tags as `arch/tlb_flush.hpp`
 * (`process_tlb_space`, `hypervisor_tlb_space`, `guest_tlb_space`, ...)
 * -- translating an address and flushing its TLB entry target exactly
 * the same hardware translation regime, so one shared set of tags
 * serves both customization points. See `tlb_flush.hpp`'s own docs for
 * what each tag means; this header only adds the translation-specific
 * pieces.
 *
 * ## Result: `translated_address`, or a `reloco::error`
 *
 * `address_translator<Arch>::translate<Space>(vaddr, access)` returns
 * `reloco::result<translated_address>` -- `reloco::unexpected(err)` if
 * the CPU reports the access would fault (including, notably, a
 * security/permission-domain violation -- e.g. a `Non-secure` access
 * hitting a `Secure`-only Granule Protection Table region under
 * `FEAT_RME` -- reported as `reloco::error::security_violation`, not
 * folded into the generic `page_fault`), the decoded `translated_address`
 * otherwise. See `arch/arm64/address_translate.hpp`'s file docs for the
 * full fault-code-to-`reloco::error` mapping table -- the one part of
 * this layer that is inherently architecture-specific decode logic, not
 * generic dispatch.
 *
 * ## Example
 *
 * @code
 * using namespace structo::arch;
 *
 * auto result = address_translator<arm64::at_tag>::translate<process_tlb_space>(
 *     user_vaddr, translate_access::write);
 * if (!result) {
 *   // result.error() is e.g. reloco::error::permission_denied,
 *   // reloco::error::page_fault, or reloco::error::security_violation.
 *   return reloco::unexpected(result.error());
 * }
 * // result->physical_address is page-aligned; add the VA's own
 * // low-order page offset back in if a byte-exact PA is needed.
 * @endcode
 *
 * ## Required trait member
 *
 * Each `Arch` provides, for every `Space` it supports translating:
 * @code
 * template <typename Space>
 * static reloco::result<translated_address> translate(std::uint64_t vaddr, translate_access access) noexcept;
 * @endcode
 *
 * A `Space` an `Arch` doesn't implement is a compile-time
 * `static_assert` through `address_translator`, not a silent
 * fallback -- unlike `tlb_flush.hpp`'s precision-fallback chain, there
 * is no "coarser" translation to fall back to: either the hardware can
 * answer this exact question or it cannot.
 *
 * Several `Space` tags (`hypervisor_tlb_space`, `guest_tlb_space`,
 * `nonsecure_tlb_space`, `secure_tlb_space`) are only architecturally
 * valid to translate from specific privilege modes -- see
 * `arch/execution_domain.hpp` for a build-time customization point
 * meant for recording which mode a translation unit is compiled for;
 * `address_translator` does not consult it to gate anything yet.
 */

#include <structo/arch/tlb_flush.hpp>

#include <cstdint>
#include <reloco/error.hpp>
#include <type_traits>

namespace structo::arch {

/** @brief Which kind of access to simulate -- the CPU's permission check (readable/writable, privilege level, etc.) depends on this, exactly as it would for a real load or store. */
enum class translate_access : std::uint8_t {
  read,
  write,
};

/**
 * @brief Decoded result of a successful hardware address translation.
 *
 * `physical_address` is page-aligned (the low-order page-offset bits
 * are always zero, matching every `AT`/`ATS1*` instruction's own
 * page-granular output) -- add the input `vaddr`'s own low bits back in
 * if a byte-exact physical address is required.
 */
struct translated_address {
  /** @brief Output address (page-aligned), in whichever physical/intermediate-physical space `Space` translates into (see `tlb_flush.hpp`'s own `Space` docs). */
  std::uint64_t physical_address = 0;
  /** @brief Raw `MAIR_ELx`-encoded memory-attribute byte for the output address (same encoding as a `page_table_entry_traits` `MemAttr` index target), best-effort decoded -- see the backend header for which bits of the raw register this comes from. */
  std::uint8_t mem_attr = 0;
  /** @brief Shareability of the output address: `0` = Non-shareable, `2` = Outer Shareable, `3` = Inner Shareable (`1` is reserved/never produced). */
  std::uint8_t shareability = 0;
  /**
   * @brief Best-effort decode of the translation's `NS` (Non-secure) bit.
   *
   * The ISA's own definition of this bit is itself regime- and
   * instruction-dependent (see the Armv8-A Architecture Reference
   * Manual's `PAR_EL1.NS` description) -- for most `Space`/instruction
   * combinations it reflects the Security state of the output physical
   * address, but for a same-security-state translation it can instead
   * read back as the *input* IPA space selector, or be architecturally
   * `UNKNOWN`. Treat this as informational, not a security boundary
   * decision by itself.
   */
  bool output_non_secure = false;
};

/**
 * @brief Per-architecture hardware-address-translation customization
 * point. See the @file-level docs for the required member-template
 * signature. Intentionally left undefined for any `Arch` that hasn't
 * been adapted, mirroring `tlb_flush_traits<Arch>`/`io_space_traits<Backend>`.
 */
template <typename Arch> struct address_translate_traits;

namespace detail {

/**
 * @brief Best-effort decode of an ARM `FST`/`DFSC`-style 6-bit fault
 * status code (identical encoding on AArch32's `PAR.FS`+`PAR.FS[5]`-ish
 * legacy field and AArch64's `PAR_EL1.FST`/`ESR_ELx.DFSC`) into the one
 * `reloco::error` enum this library uses everywhere. Shared by
 * `arch/arm/address_translate.hpp` and `arch/arm64/address_translate.hpp`
 * since both report the same fault-status encoding.
 *
 * This is necessarily a many-to-one bucketing, not a lossless decode --
 * callers needing the exact fault level/cause should keep the raw
 * `PAR`/`PAR_EL1` value around themselves (not exposed by
 * `translated_address`, which only carries the success case).
 *
 * - Address-size / translation-fault family (table-walk found no
 *   mapping) -> `error::page_fault`.
 * - Access-flag fault (entry present but not yet marked accessed,
 *   software-managed `AF`) -> `error::page_fault`, same bucket: the
 *   caller still needs to resolve a page fault to make progress either
 *   way.
 * - Permission fault (mapping exists but denies this access type) ->
 *   `error::permission_denied`.
 * - Granule Protection Fault (`FEAT_RME`: the access crosses a Physical
 *   Address Space boundary the current Security/Realm/Root state isn't
 *   allowed to cross) -> `error::security_violation` -- the one case
 *   this decode deliberately does *not* fold into `permission_denied`,
 *   since it is specifically a trust-boundary violation, not an
 *   ordinary page-permission check.
 * - Synchronous External abort / memory parity-ECC error on the
 *   table walk itself -> `error::io_error`.
 * - TLB conflict abort (transient, resolved by invalidating and
 *   retrying) -> `error::try_again`.
 * - Everything else (unsupported atomic HW update, AArch32
 *   short-descriptor domain faults, and any future/reserved encoding)
 *   -> `error::unsupported_operation`, the conservative catch-all for a
 *   fault class this decode doesn't specifically distinguish.
 */
[[nodiscard]] constexpr reloco::error fault_status_to_error(unsigned fst) noexcept {
  switch (fst) {
  // Address size fault, levels 0-3, -1, -2; Translation fault, levels 0-3, -1, -2.
  case 0b000000:
  case 0b000001:
  case 0b000010:
  case 0b000011:
  case 0b000100:
  case 0b000101:
  case 0b000110:
  case 0b000111:
  case 0b101001:
  case 0b101010:
  case 0b101011:
  case 0b101100:
    return reloco::error::page_fault;
  // Access flag fault, levels 0-3.
  case 0b001000:
  case 0b001001:
  case 0b001010:
  case 0b001011:
    return reloco::error::page_fault;
  // Permission fault, levels 0-3.
  case 0b001100:
  case 0b001101:
  case 0b001110:
  case 0b001111:
    return reloco::error::permission_denied;
  // Synchronous External abort / synchronous parity-ECC error on translation table walk.
  case 0b010010:
  case 0b010011:
  case 0b010100:
  case 0b010101:
  case 0b010110:
  case 0b010111:
  case 0b011011:
  case 0b011100:
  case 0b011101:
  case 0b011110:
  case 0b011111:
    return reloco::error::io_error;
  // Granule Protection Fault (FEAT_RME): a Security-state/PAS boundary violation.
  case 0b100010:
  case 0b100011:
  case 0b100100:
  case 0b100101:
  case 0b100110:
  case 0b100111:
  case 0b101000:
    return reloco::error::security_violation;
  // TLB conflict abort: transient, retry after invalidating the conflicting entries.
  case 0b110000:
    return reloco::error::try_again;
  // AArch32 short-descriptor domain faults -- access-control, closest to a permission fault.
  case 0b111101:
  case 0b111110:
    return reloco::error::permission_denied;
  default:
    return reloco::error::unsupported_operation;
  }
}

template <typename, typename, typename = void> struct at_has_translate : std::false_type {};
template <typename Traits, typename Space>
struct at_has_translate<Traits, Space,
                         std::void_t<decltype(Traits::template translate<Space>(std::declval<std::uint64_t>(),
                                                                                 std::declval<translate_access>()))>>
    : std::true_type {};

} // namespace detail

/**
 * @brief Public, caller-facing hardware address translator built on top
 * of `address_translate_traits<Arch>`. See the @file-level docs for the
 * full rationale and an example.
 */
template <typename Arch> struct address_translator {
  template <typename Space>
  [[nodiscard]] static reloco::result<translated_address> translate(std::uint64_t vaddr,
                                                                      translate_access access) noexcept {
    static_assert(detail::at_has_translate<address_translate_traits<Arch>, Space>::value,
                  "address_translate_traits<Arch> does not implement translate<Space>() -- there is no coarser "
                  "fallback for a hardware translation query, unlike tlb_flush_traits's precision chain.");
    return address_translate_traits<Arch>::template translate<Space>(vaddr, access);
  }
};

} // namespace structo::arch
