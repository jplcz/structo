<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo_config.hpp` (build-time configuration)

`include/structo/structo_config.hpp`

The single build-time customization entry point for every optional
structo feature macro, mirroring the role and conventions of
`reloco/reloco_config.hpp` in the reloco library structo is built on.
Include it first from any structo header that honors a customization
point defined here (today: `arch/execution_domain.hpp`, see
[`execution_domain`](execution_domain.md)).

Never edit this file to configure a build. Instead, supply
`structo/detail/porting/structo_user_config.hpp` (also found as
`detail/porting/structo_user_config.hpp` relative to the header
directory), or pass the macros with compiler `-D` flags. When the user
config header exists it is `#include`d *before* any default is applied,
so everything it defines takes precedence. It must not include any
structo header, directly or transitively (it is reached at the very top
of a structo include chain, where `#pragma once` would silently skip a
re-entered header).

## Usage

```cpp
// structo/detail/porting/structo_user_config.hpp
//
// Optional user override header. Found automatically by structo_config.hpp
// via __has_include. Either drop it in place by hand, or point the CMake
// variable JPLCZ_STRUCTO_PORTING_HEADERS at the directory containing it and
// the build stages/installs it for you (like reloco's
// JPLCZ_RELOCO_PORTING_HEADERS).
//
// Do NOT #include any structo header from here.

// Declares that this translation unit is compiled to run as a kernel in the
// ARM TrustZone Non-secure world (PL1/EL1). Define AT MOST ONE
// STRUCTO_DOMAIN_* macro (any value works, only definedness matters);
// defining two or more is a hard #error. structo::arch::current_execution_domain
// then becomes structo::arch::execution_domain::nonsecure instead of
// ::unspecified.
#define STRUCTO_DOMAIN_NONSECURE 1
```

Equivalent, with no header at all:

```sh
# Same effect from the command line; both mechanisms are interchangeable
# since structo_config.hpp only reacts to whether the macro is defined.
c++ -DSTRUCTO_DOMAIN_HYPERVISOR ...
```

## Macros

| Macro | Meaning |
|---|---|
| `STRUCTO_DOMAIN_SECURE` | ARM TrustZone Secure world, PL1/EL1 (kernel built to run as the Secure OS). |
| `STRUCTO_DOMAIN_NONSECURE` | ARM TrustZone Non-secure world, PL1/EL1 (ordinary kernel or hypervisor guest). |
| `STRUCTO_DOMAIN_MONITOR` | ARM EL3/Monitor mode (Secure Monitor firmware, e.g. TF-A-like BL31). |
| `STRUCTO_DOMAIN_HYPERVISOR` | ARM Non-secure Hyp mode/EL2. |
| `STRUCTO_DOMAIN_SECURE_HYPERVISOR` | ARM Secure EL2 (`FEAT_SEL2`, AArch64 only). |

The five are mutually exclusive; the header counts how many are defined
and emits a `#error` if more than one is. They select the
`structo::arch::execution_domain` enumerator
`structo::arch::current_execution_domain` is set to. With none defined,
it is `execution_domain::unspecified`. Currently this is purely
informational: no structo header gates any Space-tagged operation (e.g.
[`tlb_flush`](tlb_flush.md), [`address_translate`](address_translate.md))
on it.

See also: [`execution_domain`](execution_domain.md).
