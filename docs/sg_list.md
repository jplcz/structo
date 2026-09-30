<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `sg_entry<SpaceTag, PhysInt>` / `sg_list<Container>`

`include/structo/sg_list.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

`sg_entry` represents a single scatter-gather segment (a physical address
and a length). `sg_list<Container>` is a container-parameterized list of
`sg_entry`s, following the same caller-supplied-`Container` convention as
[`fdt_index`](fdt_index.md).

See also: [`sg_translator.md`](sg_translator.md),
[`compat_sg.md`](compat_sg.md).
