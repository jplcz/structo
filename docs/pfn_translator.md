<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `phys_pfn<SpaceTag, PageTraits, PhysInt>`

`include/structo/pfn_translator.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

`phys_pfn` represents a typed physical page-frame number, convertible to
and from a `phys_addr` of the same address-space tag via `PageTraits`.

See also: [`phys_addr.md`](phys_addr.md), [`phys_page.md`](phys_page.md).
