<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `phys_translator<Policy>`

`include/structo/phys_translator.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

`phys_translator` applies a caller-supplied `Policy` to translate between
virtual and physical addresses, keeping the translation strategy (identity,
direct-map, page-table walk, ...) out of the callers that merely need an
address translated.

See also: [`sg_translator.md`](sg_translator.md),
[`phys_addr.md`](phys_addr.md).
