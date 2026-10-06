<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Arm SMC Calling Convention (`arm_smccc.hpp`)

`include/structo/hypervisor/arm_smccc.hpp`

Decode/encode helpers for the Arm SMC Calling Convention (SMCCC, Arm
DEN0028)'s 32-bit Function Identifier, plus the small amount of
register-width-aware plumbing a Secure Monitor needs to reply with the
spec's generic return codes. One format, two monitor targets:

- An **AArch64 EL3 monitor** (Armv8+) decodes the Function Identifier
  out of the low 32 bits of `X0` and can service either convention
  (`SMC32` results go in `W0`-`W3`, `SMC64` results in the full
  `X0`-`X3`).
- A classic **Armv7 32-bit Monitor-mode** handler (TrustZone, pre-EL3)
  decodes it straight out of `R0` and can only ever service `SMC32` --
  it still needs to *recognize* an `SMC64` attempt (the bit is right
  there in `R0`) in order to reject it cleanly with
  `SMCCC_RET_NOT_SUPPORTED` rather than attempt to execute it.

```cpp
// AArch64 EL3 monitor, right after an SMC trap, function ID in X0:
const auto id = structo::hypervisor::decode_smccc_function_id(static_cast<std::uint32_t>(x0));

// Armv7 32-bit Monitor mode, function ID in R0:
const auto id = structo::hypervisor::decode_smccc_function_id(r0);
if (!structo::hypervisor::smccc_convention_supported<std::uint32_t>(id.convention)) {
  r0 = structo::hypervisor::encode_smccc_return_code<std::uint32_t>(
      structo::hypervisor::smccc_return_code::not_supported);
  return;
}
```

## API

- `enum class smccc_call_type { yielding = 0, fast = 1 };` -- bit 31.
- `enum class smccc_convention { smc32 = 0, smc64 = 1 };` -- bit 30.
- `enum class smccc_owner : std::uint8_t { arch, cpu, sip, oem, standard, standard_hyp, vendor_hyp, trusted_app, trusted_app_end, trusted_os, trusted_os_end };` --
  named Owning Entity Number (bits [29:24]) values/range bounds.
- `bool is_trusted_app_owner(std::uint8_t owner) noexcept` / `bool is_trusted_os_owner(std::uint8_t owner) noexcept` --
  classify a decoded `owner` against the Trusted Application (48-49)
  and Trusted OS (50-63) ranges.
- `struct smccc_function_id { smccc_call_type type; smccc_convention convention; std::uint8_t owner; std::uint16_t function_number; };`
- `constexpr smccc_function_id decode_smccc_function_id(std::uint32_t function_id_register) noexcept`
- `constexpr std::uint32_t encode_smccc_function_id(smccc_call_type, smccc_convention, std::uint8_t owner, std::uint16_t function_number) noexcept`
- `enum class smccc_return_code { success = 0, not_supported = -1, not_required = -2, invalid_parameter = -3 };` --
  the SMCCC spec's own generic return codes (distinct from PSCI's
  separate, larger error code table).
- `template <typename RegisterType> constexpr bool smccc_convention_supported(smccc_convention) noexcept` --
  `RegisterType` is `std::uint32_t` (Armv7 32-bit Monitor mode) or
  `std::uint64_t` (AArch64 EL3); always `true` for `smc32`, `true` for
  `smc64` only when `RegisterType = std::uint64_t`.
- `template <typename RegisterType> constexpr RegisterType encode_smccc_return_code(smccc_return_code) noexcept` --
  sign-extends the (always-negative-or-zero) code to the full width of
  `RegisterType`, matching the spec's requirement that an `X0` reply
  carry the full 64-bit sign extension even though the code itself is a
  small `std::int32_t`.

All of the above are `constexpr`/`noexcept`, header-only, and depend on
nothing but `<cstdint>`/`<type_traits>` -- safe for `RELOCO_KERNEL`/
freestanding EL3 firmware builds.
