<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause

-->

# `structo::arch::execution_domain` / `STRUCTO_DOMAIN_*`

`include/structo/arch/execution_domain.hpp`, `include/structo/structo_config.hpp`

`execution_domain` is the privilege world/mode a translation unit is
being compiled to run as (ARM TrustZone Secure/Non-secure, EL3/Monitor,
Hyp/EL2, Secure EL2), resolved at build time from a `STRUCTO_DOMAIN_*`
macro defined in `structo_config.hpp`'s reloco-style customization-point
layer.

## `structo_config.hpp`: one shared configuration entry point

Mirrors `reloco/reloco_config.hpp`'s role and conventions exactly: a
single header every structo header that wants to honor a build-time
customization point includes first, so configuration can be injected
regardless of which structo header an application includes first.
Overrides are supplied either with a plain compiler `-D` flag, or by
placing a `detail/porting/structo_user_config.hpp` somewhere on the
include path (not copied there automatically by CMake yet, unlike
reloco's `JPLCZ_RELOCO_PORTING_HEADERS`) -- included here, before any
`#ifndef`-guarded default, so its `#define`s always win. Like
`reloco_user_config.hpp`, that file must never `#include` any structo
header itself, to avoid re-entering an include chain still on the
stack.

## `STRUCTO_DOMAIN_*`: at most one, or a build-time `#error`

| Macro | `execution_domain` | Meaning |
|---|---|---|
| `STRUCTO_DOMAIN_SECURE` | `secure` | ARM TrustZone Secure world, PL1/EL1. |
| `STRUCTO_DOMAIN_NONSECURE` | `nonsecure` | ARM TrustZone Non-secure world, PL1/EL1. |
| `STRUCTO_DOMAIN_MONITOR` | `monitor` | ARM EL3/Monitor mode. |
| `STRUCTO_DOMAIN_HYPERVISOR` | `hypervisor` | ARM Non-secure Hyp mode/EL2. |
| `STRUCTO_DOMAIN_SECURE_HYPERVISOR` | `secure_hypervisor` | ARM Secure EL2 (`FEAT_SEL2`, AArch64 only). |
| *(none defined)* | `unspecified` | The default, and the only value this library itself ever assumes. |

Defining more than one of these is a hard `#error` raised right inside
`structo_config.hpp` itself (via `#if defined(A) + defined(B) + ... >
1`), rather than surfacing as a confusing redefinition error somewhere
downstream. `structo::arch::current_execution_domain` is the resulting
`constexpr execution_domain` value every translation unit can read.

## Scaffolding for future use -- not a gate applied anywhere yet

**No structo header currently restricts anything based on
`current_execution_domain`.** `arch/tlb_flush.hpp`'s `tlb_flusher<Arch>`
and `arch/address_translate.hpp`'s `address_translator<Arch>` do not
reject instantiating, say, `secure_tlb_space` from a translation unit
built with `STRUCTO_DOMAIN_NONSECURE`, even though several `Space` tags
(`hypervisor_tlb_space`, `guest_tlb_space`, `nonsecure_tlb_space`,
`secure_tlb_space`) are only architecturally valid to target from
specific privilege modes, as their own docs describe. A caller who wants
that enforced today still needs its own `static_assert`/runtime check;
this header only gives every translation unit one canonical, shared
place to ask "which world am I?", so such a check -- here, or added to
the two headers above in a later change -- has a single answer to
consult instead of every caller inventing its own.

## Example

```cpp
// Compiled as this kernel's EL2 hypervisor image, e.g. via -DSTRUCTO_DOMAIN_HYPERVISOR.
#include <structo/arch/execution_domain.hpp>

static_assert(structo::arch::current_execution_domain == structo::arch::execution_domain::hypervisor);
```

See also: [`domain_space_traits.md`](domain_space_traits.md) (what this
header's enumerators actually unlock: translating an `execution_domain`
into the matching tag in four independent tag families scattered across
the library, and back), [`tlb_flush.md`](tlb_flush.md)/
[`address_translate.md`](address_translate.md) (the two headers whose
`Space` tags this one's doc-comments reference as future gating
candidates).
