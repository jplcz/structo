<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `sg_translator`

`include/structo/sg_translator.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

`sg_translator` applies a caller-supplied translation policy to an
[`sg_list`](sg_list.md), the scatter-gather counterpart to
[`phys_translator`](phys_translator.md)'s single-address translation.

See also: [`sg_list.md`](sg_list.md), [`phys_translator.md`](phys_translator.md).
