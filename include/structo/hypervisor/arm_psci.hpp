// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file arm_psci.hpp
 * @brief Decode/encode helpers for PSCI (Power State Coordination
 * Interface, Arm DEN0022) function IDs, built directly on top of
 * `arm_smccc.hpp`'s generic Function Identifier decode: every standard
 * PSCI call is an SMCCC `Fast` call owned by `smccc_owner::standard`
 * (owner 4), so decoding one is "decode the generic Function
 * Identifier, then classify the function number against the fixed
 * PSCI table below" -- no separate bit-layout to reverse-engineer.
 *
 * ## Versions covered
 *
 * Only the SMCCC-based PSCI 0.2+ numbering is covered here -- `psci_function`'s
 * values 0-9 are PSCI 0.2's base set, 10-17 are added in PSCI 1.0, and 18-20
 * in PSCI 1.1. The much older, pre-0.2 "PSCI 0.1" ABI (function IDs are raw,
 * platform/firmware-defined magic numbers discovered via firmware/device-tree
 * configuration, *not* SMCCC Function Identifiers at all -- no owner/
 * convention/type bits, no fixed numbering) is out of scope: there is nothing
 * generic to decode, by design.
 *
 * ## One function number, an SMC32 *and* an SMC64 Function Identifier
 *
 * Several PSCI functions (`CPU_SUSPEND`, `CPU_ON`, `AFFINITY_INFO`,
 * `MIGRATE`, `MIGRATE_INFO_UP_CPU`, `CPU_DEFAULT_SUSPEND`, `NODE_HW_STATE`,
 * `SYSTEM_SUSPEND`, `STAT_RESIDENCY`, `STAT_COUNT`, `SYSTEM_RESET2`,
 * `MEM_PROTECT_CHECK_RANGE`) carry a 64-bit-sized argument or return value
 * (typically a CPU "entry point address" or similar) and therefore have both
 * an SMC32 *and* an SMC64 Function Identifier for the same `psci_function`
 * (differing only in `arm_smccc.hpp`'s convention bit); the rest have no
 * SMC64 variant at all -- see `psci_function_has_smc64_variant()`. A monitor
 * must reject an SMC64-convention call to one of the latter (and, on an
 * Armv7 32-bit Monitor-mode handler, *any* SMC64-convention PSCI call --
 * see `arm_smccc.hpp`'s `smccc_convention_supported()`).
 *
 * @code
 * const smccc_function_id id = decode_smccc_function_id(static_cast<std::uint32_t>(x0));
 * const reloco::result<psci_function> fn = decode_psci_function(id);
 * if (!fn) {
 *   x0 = encode_smccc_return_code<std::uint64_t>(smccc_return_code::not_supported);
 *   return;
 * }
 * if (id.convention == smccc_convention::smc64 && !psci_function_has_smc64_variant(*fn)) {
 *   x0 = encode_psci_return_code<std::uint64_t>(psci_return_code::not_supported);
 *   return;
 * }
 * switch (*fn) {
 * case psci_function::version: x0 = encode_psci_version({1, 1}); break;
 * case psci_function::cpu_on: x0 = encode_psci_return_code<std::uint64_t>(handle_cpu_on(x1, x2, x3)); break;
 * // ...
 * }
 * @endcode
 */

#include "arm_smccc.hpp"

#include <reloco/error.hpp>
#include <reloco/expected.hpp>

#include <cstdint>

namespace structo::hypervisor {

/**
 * @brief PSCI function numbers (the `smccc_function_id::function_number` value for every standard
 * PSCI call), independent of the SMC32/SMC64 convention bit -- see the @file docs for which of these
 * also have an SMC64 Function Identifier.
 */
enum class psci_function : std::uint16_t {
  version = 0,
  cpu_suspend = 1,
  cpu_off = 2,
  cpu_on = 3,
  affinity_info = 4,
  migrate = 5,
  migrate_info_type = 6,
  migrate_info_up_cpu = 7,
  system_off = 8,
  system_reset = 9,
  // PSCI 1.0:
  features = 10,
  cpu_freeze = 11,
  cpu_default_suspend = 12,
  node_hw_state = 13,
  system_suspend = 14,
  set_suspend_mode = 15,
  stat_residency = 16,
  stat_count = 17,
  // PSCI 1.1:
  system_reset2 = 18,
  mem_protect = 19,
  mem_protect_check_range = 20,
};

/**
 * @brief Decodes @p id's function number into a `psci_function`.
 * @return `error::invalid_argument` if @p id isn't owned by `smccc_owner::standard` at all (i.e. it
 * isn't a PSCI call to begin with); `error::unsupported_operation` if it is, but the function number
 * doesn't match any function in the table above (a future PSCI revision's function, or simply not
 * implemented) -- the caller's usual response to either is the same (`SMCCC_RET_NOT_SUPPORTED`), but
 * the distinction is kept since "not a PSCI call" and "unrecognized PSCI call" are different bugs to a
 * caller trying to route multiple owners through one dispatch point.
 *
 * `RELOCO_CONSTEXPR20`, not a plain `constexpr` (unlike everything else in this header):
 * `reloco::result<T>`'s destructor is only `constexpr` from C++20 onward, so a non-template function
 * returning it by value fails the "`constexpr` function must have a literal return type" check under
 * C++17 (a class *template*'s member function, like `phys_addr::try_add`, gets a pass -- the check is
 * deferred until actual instantiation in a constant expression -- but a concrete, non-template free
 * function like this one is checked immediately).
 */
[[nodiscard]] RELOCO_CONSTEXPR20 reloco::result<psci_function> decode_psci_function(smccc_function_id id) noexcept {
  if (id.owner != static_cast<std::uint8_t>(smccc_owner::standard)) {
    return reloco::unexpected(reloco::error::invalid_argument);
  }
  switch (id.function_number) {
  case 0: return psci_function::version;
  case 1: return psci_function::cpu_suspend;
  case 2: return psci_function::cpu_off;
  case 3: return psci_function::cpu_on;
  case 4: return psci_function::affinity_info;
  case 5: return psci_function::migrate;
  case 6: return psci_function::migrate_info_type;
  case 7: return psci_function::migrate_info_up_cpu;
  case 8: return psci_function::system_off;
  case 9: return psci_function::system_reset;
  case 10: return psci_function::features;
  case 11: return psci_function::cpu_freeze;
  case 12: return psci_function::cpu_default_suspend;
  case 13: return psci_function::node_hw_state;
  case 14: return psci_function::system_suspend;
  case 15: return psci_function::set_suspend_mode;
  case 16: return psci_function::stat_residency;
  case 17: return psci_function::stat_count;
  case 18: return psci_function::system_reset2;
  case 19: return psci_function::mem_protect;
  case 20: return psci_function::mem_protect_check_range;
  default: return reloco::unexpected(reloco::error::unsupported_operation);
  }
}

/**
 * @brief Builds the raw Function Identifier for @p function at the given @p convention -- always a
 * `smccc_call_type::fast` call and `smccc_owner::standard`-owned, since every standard PSCI function is
 * both (the convention is the only thing a caller ever needs to choose).
 */
[[nodiscard]] constexpr std::uint32_t encode_psci_function_id(psci_function function,
                                                               smccc_convention convention =
                                                                   smccc_convention::smc32) noexcept {
  return encode_smccc_function_id(smccc_call_type::fast, convention, static_cast<std::uint8_t>(smccc_owner::standard),
                                   static_cast<std::uint16_t>(function));
}

/**
 * @brief Whether @p function has a distinct SMC64 Function Identifier.
 *
 * Only functions that carry a 64-bit-sized argument or return value (a CPU entry-point address, a
 * residency count, ...) have one; the rest (`version`, `cpu_off`, `migrate_info_type`, `system_off`,
 * `system_reset`, `features`, `cpu_freeze`, `set_suspend_mode`, `mem_protect`) take/return nothing wider
 * than 32 bits and were never assigned an SMC64 counterpart by the spec -- an SMC64-convention call
 * naming one of those must be rejected as `psci_return_code::not_supported`, exactly like an SMC64 call
 * to a function number PSCI doesn't define at all.
 */
[[nodiscard]] constexpr bool psci_function_has_smc64_variant(psci_function function) noexcept {
  switch (function) {
  case psci_function::cpu_suspend:
  case psci_function::cpu_on:
  case psci_function::affinity_info:
  case psci_function::migrate:
  case psci_function::migrate_info_up_cpu:
  case psci_function::cpu_default_suspend:
  case psci_function::node_hw_state:
  case psci_function::system_suspend:
  case psci_function::stat_residency:
  case psci_function::stat_count:
  case psci_function::system_reset2:
  case psci_function::mem_protect_check_range: return true;
  default: return false;
  }
}

/** @brief PSCI's own return code table (Arm DEN0022) -- distinct from, and not overlapping in meaning
 * with, `arm_smccc.hpp`'s small generic `smccc_return_code` set; a PSCI function's result register
 * always carries one of these instead. */
enum class psci_return_code : std::int32_t {
  success = 0,
  not_supported = -1,
  invalid_parameters = -2,
  denied = -3,
  already_on = -4,
  on_pending = -5,
  internal_failure = -6,
  not_present = -7,
  disabled = -8,
  invalid_address = -9,
};

/** @brief Encodes @p code as the bit pattern for a monitor's first PSCI result register (`R0`/`W0` or
 * `X0`), sign-extended to the full width of `RegisterType` exactly like `encode_smccc_return_code()` --
 * see that function's docs for why. */
template <typename RegisterType>
[[nodiscard]] constexpr RegisterType encode_psci_return_code(psci_return_code code) noexcept {
  static_assert(std::is_same_v<RegisterType, std::uint32_t> || std::is_same_v<RegisterType, std::uint64_t>,
                "RegisterType must be std::uint32_t (Armv7 32-bit Monitor mode) or "
                "std::uint64_t (AArch64 EL3 monitor)");
  return static_cast<RegisterType>(static_cast<std::int64_t>(static_cast<std::int32_t>(code)));
}

/** @brief `PSCI_VERSION`'s own decoded result payload (distinct from the generic/error result registers
 * every other PSCI function uses): a major/minor version pair, not a `psci_return_code`. */
struct psci_version {
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
};

/** @brief Decodes a raw `PSCI_VERSION` result register value (bits [31:16] major, [15:0] minor). */
[[nodiscard]] constexpr psci_version decode_psci_version(std::uint32_t version_register) noexcept {
  return psci_version{static_cast<std::uint16_t>(version_register >> 16),
                       static_cast<std::uint16_t>(version_register & 0xFFFFU)};
}

/** @brief Inverse of `decode_psci_version()`: packs a major/minor pair back into the raw result register
 * value a `PSCI_VERSION` implementation should return. */
[[nodiscard]] constexpr std::uint32_t encode_psci_version(psci_version version) noexcept {
  return (static_cast<std::uint32_t>(version.major) << 16) | static_cast<std::uint32_t>(version.minor);
}

} // namespace structo::hypervisor
