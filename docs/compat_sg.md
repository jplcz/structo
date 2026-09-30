<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Scatter-gather compatibility codecs

`include/structo/compat_sg.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

`sg_descriptor_layout` and `chained_sg_layout` describe descriptor storage
layouts. `compact_sg_codec`, `chained_sg_codec`, and `two_level_sg_codec`
encode and decode page ranges using those layouts, page traits, and an
optional address-space tag.

See also: [`sg_list.md`](sg_list.md), [`sg_translator.md`](sg_translator.md),
[`phys_page.md`](phys_page.md).
