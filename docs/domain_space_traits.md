<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::domain_space_traits<Domain>` / `space_domain_of<SpaceTag>`

`include/structo/arch/domain_space_traits.hpp`

Translates a [`structo::arch::execution_domain`](execution_domain.md)
into the matching empty tag struct in each of this library's four
independent "which world/space does this address/TLB entry/register
belong to" tag families -- and back again.

## Four independent tag families, one shared key

Four modules each define their own family of tags for a different
problem, so each has a different, non-overlapping set of consumers:

| Family | Module | Example tags |
|---|---|---|
| `tlb_space` | [`tlb_flush.hpp`](tlb_flush.md) / [`address_translate.hpp`](address_translate.md) | `process_tlb_space`, `secure_tlb_space`, `hypervisor_tlb_space`, ... |
| `phys_space` | `phys_addr.hpp` | `host_phys_space`, `secure_phys_space`, `root_phys_space`, ... |
| `io_space` | `io_address.hpp` | `device_io_space`, `secure_io_space`, `hypervisor_io_space`, ... |
| `virt_space` | `target_ptr.hpp` | `user_space`, `kernel_space`, `secure_world_space`, ... |

Nothing ties these four families together on its own -- a caller
holding a `secure_tlb_space`-tagged flush and wanting the matching
`phys_addr` space tag (`secure_phys_space`) for the same world
previously had no choice but to know, and spell out, that correspondence
itself at every call site. `domain_space_traits<Domain>` is that single,
shared correspondence table, keyed on `execution_domain`.

## Coverage is deliberately incomplete

Only `execution_domain::secure` has a tag in all four families. Every
other domain is missing at least one member, and none was ever filled
in with an arbitrary guess:

| `Domain` | `tlb_space` | `phys_space` | `io_space` | `virt_space` |
|---|---|---|---|---|
| `secure` | `secure_tlb_space` | `secure_phys_space` | `secure_io_space` | `secure_world_space` |
| `nonsecure` | `nonsecure_tlb_space` | `nonsecure_phys_space` | `nonsecure_io_space` | -- |
| `monitor` | `root_tlb_space` | `root_phys_space` | -- | -- |
| `hypervisor` | `hypervisor_tlb_space` | `host_phys_space` | `hypervisor_io_space` | -- |
| `secure_hypervisor` | `hypervisor_tlb_space` | -- | -- | -- |
| `unspecified` | *(no specialization at all -- hard compile error if instantiated)* | | | |

Reasons a cell is empty (see the header's `@file` docs for the full
rationale): EL3/Monitor and EL2/Hyp have no dedicated `target_ptr.hpp`
tag of their own; `nonsecure` has no virtual-address tag because
`user_space`/`kernel_space` already mean "the ordinary, implicitly
Non-secure case" and picking one as *the* Non-secure tag would be
arbitrary; `monitor` has no `io_address.hpp` tag (no "Root-world-only
MMIO alias" concept exists yet); `secure_hypervisor` (`FEAT_SEL2`)
shares `hypervisor_tlb_space`'s raw AArch64 encoding with plain
`hypervisor` (see [`address_translate.md`](address_translate.md)) but
has no Secure-specific `phys_addr`/`io_address` tag yet.

`guest_*_space`/`guest_vm_space`, `realm_*_space`, `dma_bus_space`,
`default_*_space`, `port_io_space`, `device_io_space`,
`untagged_tlb_space`, `user_space`, `kernel_space`, and `gpt_tlb_space`
are deliberately unreachable through either direction of this header:
none of them names a privilege world/mode a translation unit is
compiled to run as -- they name a role (the guest, a DMA initiator, an
unspecified default) or an out-of-scope extension concept instead.

## `current_domain_spaces`

`domain_space_traits<current_execution_domain>` -- the correspondence
table for whichever domain this translation unit was built for.
Ill-formed if built with no `STRUCTO_DOMAIN_*` macro defined at all,
same as instantiating `domain_space_traits<execution_domain::
unspecified>` directly.

## `space_domain_of<SpaceTag>`: the reverse lookup

Matches a tag struct from any of the four families back to the
`execution_domain` it represents. Unlike `domain_space_traits`, this
direction never hard-fails: any tag with no unambiguous domain
(including every tag the previous section lists as unreachable)
resolves to `execution_domain::unspecified` instead of a compile error,
since matching an arbitrary, possibly-unrelated tag against a domain is
the expected use here, not a programming mistake.

`hypervisor_tlb_space` is a deliberate, documented example:
`space_domain_of<hypervisor_tlb_space>` is `unspecified`, *not*
`hypervisor`, because that one tag is shared verbatim between
`execution_domain::hypervisor` and `::secure_hypervisor` -- query
`domain_space_traits<execution_domain::hypervisor>`/
`<execution_domain::secure_hypervisor>::tlb_space` directly instead of
relying on the ambiguous reverse direction.

## `same_domain_v<TagA, TagB>`

`true` if `TagA` and `TagB` -- from any of the four families, in any
combination, including `TagA == TagB` -- represent the same known
`execution_domain`. Always `false` if either resolves to `unspecified`,
so two unrelated domain-agnostic tags (e.g. two different
`default_phys_space` uses for genuinely different platforms) never
compare equal merely for being the same type.

## Example

```cpp
using namespace structo::arch;

static_assert(std::is_same_v<domain_space_traits<execution_domain::secure>::phys_space, secure_phys_space>);
static_assert(space_domain_of<secure_tlb_space> == execution_domain::secure);
static_assert(same_domain_v<secure_tlb_space, secure_phys_space>);
static_assert(!same_domain_v<hypervisor_tlb_space, host_phys_space>); // ambiguous tag resolves to unspecified
```

See also: [`execution_domain.md`](execution_domain.md) (the `Domain`
key this header is built on), [`tlb_flush.md`](tlb_flush.md) /
[`address_translate.md`](address_translate.md) (the `tlb_space` family
this header cross-references).
