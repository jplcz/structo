// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file arm_smccc.hpp
 * @brief Decode/encode helpers for the Arm SMC Calling Convention (SMCCC,
 * Arm DEN0028)'s 32-bit Function Identifier, plus the small amount of
 * register-width-aware plumbing a Secure Monitor needs to reply to a
 * trapped `SMC`/`HVC` with the generic return codes the spec defines.
 *
 * ## One Function Identifier format, two monitor targets
 *
 * The Function Identifier is always a single 32-bit value -- `type`
 * (bit 31: Fast vs Yielding call), `convention` (bit 30: SMC32 vs
 * SMC64), a 6-bit Owning Entity Number (bits [29:24]), and a 16-bit
 * function number (bits [15:0]) -- regardless of which of the two very
 * different monitors is decoding it:
 *
 * - An **AArch64 EL3 monitor** (Armv8+) receives it in the low 32 bits
 *   of `X0` (the upper 32 bits are reserved/SBZ by the caller and must
 *   simply be ignored, never validated as an error) and can service
 *   *either* convention: `SMC32` callers get their results truncated to
 *   `W0`-`W3`, `SMC64` callers get the full `X0`-`X3`.
 * - A **classic Armv7 32-bit Monitor-mode** handler (TrustZone, pre-EL3)
 *   receives it directly in `R0` and only ever has 32-bit registers to
 *   work with -- it can decode an `SMC64`-convention Function
 *   Identifier perfectly well (the bit is right there in `R0`), it just
 *   cannot *service* one: there is no 64-bit argument/result register
 *   file to honor the call with, so the correct behavior is to decode
 *   far enough to recognize `convention == smc64`, then immediately
 *   reply `SMCCC_RET_NOT_SUPPORTED` rather than attempt to execute it.
 *
 * `decode_smccc_function_id()`/`encode_smccc_function_id()` below work
 * identically for both (they only ever see a `std::uint32_t`);
 * `smccc_convention_supported<RegisterType>()` and
 * `encode_smccc_return_code<RegisterType>()` are the two helpers that
 * are parameterized on the monitor's own register width (`std::uint32_t`
 * for Armv7 Monitor mode, `std::uint64_t` for AArch64 EL3) so each
 * monitor flavor gets the correct behavior/sign-extension for free
 * without duplicating logic.
 *
 * @code
 * // AArch64 EL3 monitor, right after an SMC trap, function ID in X0:
 * const auto id = structo::hypervisor::decode_smccc_function_id(static_cast<std::uint32_t>(x0));
 * if (!structo::hypervisor::smccc_convention_supported<std::uint64_t>(id.convention)) {
 *   x0 = structo::hypervisor::encode_smccc_return_code<std::uint64_t>(
 *       structo::hypervisor::smccc_return_code::not_supported); // unreachable on AArch64: always supported
 * }
 *
 * // Armv7 32-bit Monitor mode, function ID in R0:
 * const auto id = structo::hypervisor::decode_smccc_function_id(r0);
 * if (!structo::hypervisor::smccc_convention_supported<std::uint32_t>(id.convention)) {
 *   r0 = structo::hypervisor::encode_smccc_return_code<std::uint32_t>(
 *       structo::hypervisor::smccc_return_code::not_supported); // SMC64 caller on an AArch32-only monitor
 *   return;
 * }
 * @endcode
 */

#include <cstdint>
#include <type_traits>

namespace structo::hypervisor {

/** @brief Bit 31 of the Function Identifier: Fast calls run with interrupts masked and return
 * synchronously; Yielding calls may be interrupted/rescheduled by the monitor/dispatcher before
 * completion (used by Standard Secure Service calls that can take a while, e.g. some SiP calls). */
enum class smccc_call_type : std::uint32_t {
  yielding = 0,
  fast = 1,
};

/** @brief Bit 30 of the Function Identifier: which register width the call's arguments/results use. */
enum class smccc_convention : std::uint32_t {
  /** @brief Arguments/results in `W0`-`W3` (or further `W` registers, caller/function-specific). */
  smc32 = 0,
  /** @brief Arguments/results in `X0`-`X3` (or further `X` registers). Only serviceable by a monitor
   * with 64-bit registers -- see `smccc_convention_supported()`. */
  smc64 = 1,
};

/** @brief Named Owning Entity Number (bits [29:24]) ranges/values from the SMCCC spec. Compare the
 * decoded `smccc_function_id::owner` against these rather than against a raw literal. Owners 7-47 are
 * reserved (no currently-defined meaning); `trusted_app_end`/`trusted_os_end` are inclusive range ends,
 * not single owners -- use `is_trusted_app_owner()`/`is_trusted_os_owner()` for the full ranges. */
enum class smccc_owner : std::uint8_t {
  arch = 0,
  cpu = 1,
  sip = 2,
  oem = 3,
  standard = 4,
  standard_hyp = 5,
  vendor_hyp = 6,
  trusted_app = 48,
  trusted_app_end = 49,
  trusted_os = 50,
  trusted_os_end = 63,
};

/** @brief Whether @p owner (a decoded `smccc_function_id::owner`) falls in the Trusted Application
 * owner range (48-49 inclusive). */
[[nodiscard]] constexpr bool is_trusted_app_owner(std::uint8_t owner) noexcept {
  return owner >= static_cast<std::uint8_t>(smccc_owner::trusted_app) &&
         owner <= static_cast<std::uint8_t>(smccc_owner::trusted_app_end);
}

/** @brief Whether @p owner (a decoded `smccc_function_id::owner`) falls in the Trusted OS owner range
 * (50-63 inclusive). */
[[nodiscard]] constexpr bool is_trusted_os_owner(std::uint8_t owner) noexcept {
  return owner >= static_cast<std::uint8_t>(smccc_owner::trusted_os) &&
         owner <= static_cast<std::uint8_t>(smccc_owner::trusted_os_end);
}

/** @brief A fully decoded SMCCC Function Identifier. See the @file docs for the bit layout. */
struct smccc_function_id {
  smccc_call_type type = smccc_call_type::yielding;
  smccc_convention convention = smccc_convention::smc32;
  /** @brief 6-bit Owning Entity Number (bits [29:24]), 0-63. Compare against `smccc_owner`/
   * `is_trusted_app_owner()`/`is_trusted_os_owner()` rather than against raw literals. */
  std::uint8_t owner = 0;
  /** @brief 16-bit function number (bits [15:0]), meaning defined per-owner. */
  std::uint16_t function_number = 0;
};

/**
 * @brief Decodes a raw Function Identifier register value into its four fields.
 * @param function_id_register The full 32-bit Function Identifier -- `W0` on an Armv7 32-bit Monitor-
 * mode trap, or the low 32 bits of `X0` (`static_cast<std::uint32_t>(x0)`) on an AArch64 EL3 trap; the
 * upper 32 bits of `X0` are reserved/SBZ by the caller and must not be inspected here.
 */
[[nodiscard]] constexpr smccc_function_id decode_smccc_function_id(std::uint32_t function_id_register) noexcept {
  smccc_function_id id{};
  id.type = ((function_id_register >> 31) & 0x1U) != 0 ? smccc_call_type::fast : smccc_call_type::yielding;
  id.convention = ((function_id_register >> 30) & 0x1U) != 0 ? smccc_convention::smc64 : smccc_convention::smc32;
  id.owner = static_cast<std::uint8_t>((function_id_register >> 24) & 0x3FU);
  id.function_number = static_cast<std::uint16_t>(function_id_register & 0xFFFFU);
  return id;
}

/** @brief Inverse of `decode_smccc_function_id()`: packs the four fields back into a raw 32-bit
 * Function Identifier, e.g. to build the value a guest/NS-world caller should place in `R0`/`W0`/`X0`
 * before trapping. @p owner above 63 (outside the 6-bit field) is truncated, not rejected -- callers
 * building a Function Identifier from one of the named `smccc_owner` values never hit this. */
[[nodiscard]] constexpr std::uint32_t encode_smccc_function_id(smccc_call_type type, smccc_convention convention,
                                                               std::uint8_t owner,
                                                               std::uint16_t function_number) noexcept {
  return (static_cast<std::uint32_t>(type) << 31) | (static_cast<std::uint32_t>(convention) << 30) |
         ((static_cast<std::uint32_t>(owner) & 0x3FU) << 24) | static_cast<std::uint32_t>(function_number);
}

/** @brief The SMCCC-defined generic return codes (distinct from PSCI's own, larger error code table,
 * which a `standard`-owned PSCI call's own handler is responsible for returning instead of these). */
enum class smccc_return_code : std::int32_t {
  success = 0,
  not_supported = -1,
  not_required = -2,
  invalid_parameter = -3,
};

/**
 * @brief Whether a monitor with `RegisterType`-wide registers can service @p convention.
 *
 * `smc64` needs a 64-bit argument/result register file (`X0`-`X3` and beyond) to carry its payload --
 * an Armv7 32-bit Monitor-mode handler (`RegisterType = std::uint32_t`) has no such registers and must
 * reject it (see `encode_smccc_return_code`) rather than attempt to service it; an AArch64 EL3 monitor
 * (`RegisterType = std::uint64_t`) can service both conventions.
 */
template <typename RegisterType>
[[nodiscard]] constexpr bool smccc_convention_supported(smccc_convention convention) noexcept {
  static_assert(std::is_same_v<RegisterType, std::uint32_t> || std::is_same_v<RegisterType, std::uint64_t>,
                "RegisterType must be std::uint32_t (Armv7 32-bit Monitor mode) or "
                "std::uint64_t (AArch64 EL3 monitor)");
  return convention == smccc_convention::smc32 || std::is_same_v<RegisterType, std::uint64_t>;
}

/**
 * @brief Encodes @p code as the bit pattern a monitor should place in its first result register
 * (`R0`/`W0` or `X0`).
 *
 * The SMCCC spec requires an error code to be sign-extended to the full width of the result register,
 * so a `std::uint64_t` (`X0`) reply gets the 64-bit sign-extension of @p code even though the code
 * itself is only ever one of the small negative `std::int32_t` values above; a `std::uint32_t` (`R0`/
 * `W0`) reply is the plain 32-bit bit pattern.
 */
template <typename RegisterType>
[[nodiscard]] constexpr RegisterType encode_smccc_return_code(smccc_return_code code) noexcept {
  static_assert(std::is_same_v<RegisterType, std::uint32_t> || std::is_same_v<RegisterType, std::uint64_t>,
                "RegisterType must be std::uint32_t (Armv7 32-bit Monitor mode) or "
                "std::uint64_t (AArch64 EL3 monitor)");
  // Widen through std::int64_t first so a 64-bit register gets the full-width sign
  // extension the spec requires; truncating back down to std::uint32_t is a no-op
  // since `code` always already fits in 32 bits.
  return static_cast<RegisterType>(static_cast<std::int64_t>(static_cast<std::int32_t>(code)));
}

} // namespace structo::hypervisor
