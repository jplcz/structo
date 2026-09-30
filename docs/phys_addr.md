<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `phys_addr<T, SpaceTag, PhysInt>`

`include/structo/phys_addr.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

`phys_addr` carries a physical address together with its address-space tag
(`default_phys_space`, `host_phys_space`, `guest_phys_space`,
`dma_bus_space`, `secure_phys_space`, `nonsecure_phys_space`,
`root_phys_space`, `realm_phys_space`) and integer representation, so
addresses from different address spaces can't be mixed without an explicit
conversion.

`dmap_mapper<VirtBase, PhysSize, ExpectedSpace, PhysBase>` and
`dmap_ptr<T, Mapper, PhysInt>` provide direct-map conversions through a
caller-supplied mapping policy.

See also: [`pfn_translator.md`](pfn_translator.md),
[`phys_translator.md`](phys_translator.md).
