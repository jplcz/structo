<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# PSCI function decoder (`arm_psci.hpp`)

`include/structo/hypervisor/arm_psci.hpp`

Decode/encode helpers for PSCI (Power State Coordination Interface, Arm
DEN0022) function IDs, built directly on `arm_smccc.hpp`: every
standard PSCI call is an SMCCC `Fast` call owned by
`smccc_owner::standard`, so decoding one is "decode the generic
Function Identifier, then classify the function number" -- no separate
bit layout involved.

Only the SMCCC-based **PSCI 0.2+** numbering is covered (0-9 base, 10-17
added in 1.0, 18-20 in 1.1). The older pre-0.2 "PSCI 0.1" ABI uses raw,
firmware/DT-defined magic numbers instead of SMCCC Function Identifiers
and is out of scope -- there is nothing generic to decode there.

```cpp
const smccc_function_id id = decode_smccc_function_id(static_cast<std::uint32_t>(x0));
const reloco::result<psci_function> fn = decode_psci_function(id);
if (!fn) {
  x0 = encode_smccc_return_code<std::uint64_t>(smccc_return_code::not_supported);
  return;
}
if (id.convention == smccc_convention::smc64 && !psci_function_has_smc64_variant(*fn)) {
  x0 = encode_psci_return_code<std::uint64_t>(psci_return_code::not_supported);
  return;
}
switch (*fn) {
case psci_function::version: x0 = encode_psci_version({1, 1}); break;
case psci_function::cpu_on: x0 = encode_psci_return_code<std::uint64_t>(handle_cpu_on(x1, x2, x3)); break;
// ...
}
```

## API

- `enum class psci_function : std::uint16_t { version, cpu_suspend, cpu_off, cpu_on, affinity_info, migrate, migrate_info_type, migrate_info_up_cpu, system_off, system_reset, features, cpu_freeze, cpu_default_suspend, node_hw_state, system_suspend, set_suspend_mode, stat_residency, stat_count, system_reset2, mem_protect, mem_protect_check_range };`
- `RELOCO_CONSTEXPR20 reloco::result<psci_function> decode_psci_function(smccc_function_id id) noexcept` --
  `error::invalid_argument` if `id.owner != smccc_owner::standard`
  (not a PSCI call at all); `error::unsupported_operation` if it is,
  but the function number isn't one of the above.
- `constexpr std::uint32_t encode_psci_function_id(psci_function, smccc_convention = smc32) noexcept` --
  always `smccc_call_type::fast`/`smccc_owner::standard`, since every
  standard PSCI function is both.
- `constexpr bool psci_function_has_smc64_variant(psci_function) noexcept` --
  `true` only for the functions that carry a 64-bit-sized argument/
  result (`cpu_suspend`, `cpu_on`, `affinity_info`, `migrate`,
  `migrate_info_up_cpu`, `cpu_default_suspend`, `node_hw_state`,
  `system_suspend`, `stat_residency`, `stat_count`, `system_reset2`,
  `mem_protect_check_range`); an SMC64-convention call naming any other
  function must be rejected the same as an unrecognized one.
- `enum class psci_return_code { success = 0, not_supported = -1, invalid_parameters = -2, denied = -3, already_on = -4, on_pending = -5, internal_failure = -6, not_present = -7, disabled = -8, invalid_address = -9 };` --
  PSCI's own return-code table, distinct from `arm_smccc.hpp`'s small
  generic `smccc_return_code` set.
- `template <typename RegisterType> constexpr RegisterType encode_psci_return_code(psci_return_code) noexcept` --
  same register-width sign-extension rule as
  `encode_smccc_return_code()`.
- `struct psci_version { std::uint16_t major; std::uint16_t minor; };` --
  `PSCI_VERSION`'s own result payload (not a `psci_return_code`).
- `constexpr psci_version decode_psci_version(std::uint32_t) noexcept` /
  `constexpr std::uint32_t encode_psci_version(psci_version) noexcept`.

All `noexcept`, header-only, and `constexpr` except `decode_psci_function`
(`RELOCO_CONSTEXPR20`: `constexpr` from C++20 onward only, plain otherwise --
`reloco::result<T>`'s destructor isn't `constexpr` under C++17). Depends only
on `arm_smccc.hpp` and `<reloco/error.hpp>`/`<reloco/expected.hpp>` (both
already freestanding-safe), so it stays safe for `RELOCO_KERNEL`/
EL3-firmware builds.
