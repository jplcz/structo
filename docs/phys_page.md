<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `page_view<PageTraits, OsTraits>`

`include/structo/phys_page.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

`page_traits<Size, Shift>` describes a page size at compile time.
`os_traits_base<Derived, OsPage>` and `page_view<PageTraits, OsTraits>`
adapt operating-system page objects (e.g. a kernel's `struct page`) to
typed page operations without copying or owning the underlying object.

See also: [`pfn_translator.md`](pfn_translator.md),
[`region_set.md`](region_set.md).
