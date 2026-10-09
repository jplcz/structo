<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::debug_symtab_resolver_tag`

`include/structo/debug_symtab_resolver.hpp`

Adapts [`structo::debug_symtab_view`](debug_symtab.md) to microfmt's
`symbol_resolver_traits<Tag>` customization point, so a `DSYM` blob (see
[`debug_symtab_format`](debug_symtab_format.md)) can back a
`microfmt::remote_symbol_view`/`microfmt::make_remote_symbol` call
directly.

It lives apart from `debug_symtab.hpp` (which only depends on reloco) so
a consumer that only needs the decoder never pulls in microfmt. This
mirrors microfmt's own split between `inspector/symbol_resolver.hpp`
(the generic customization point) and one concrete backend.

## Usage

```cpp
#include <structo/debug_symtab_resolver.hpp>

void report_fault(std::span<const std::byte> blob, std::uintptr_t fault_addr) {
  // `blob` is the raw DSYM image (e.g. embedded in the kernel or loaded
  // alongside it). try_create validates it and returns a result; value()
  // is used here for brevity -- real code should handle failure.
  auto view = structo::debug_symtab_view::try_create(blob).value();

  // Type-erased microfmt resolver. The template argument is the tag that
  // selects this header's symbol_resolver_traits specialization; the
  // argument is the context (the debug_symtab_view to search). `view`
  // must outlive `resolver`.
  auto resolver = microfmt::symbol_resolver_ref::make<structo::debug_symtab_resolver_tag>(view);

  // Caller-provided scratch space. The resolved symbol name is written
  // here (aliases it), so the printed text is only valid while `scratch`
  // and `ctx` are alive.
  char scratch[64];
  microfmt::symbol_resolution_context ctx(scratch);

  // Formats `fault_addr` as "name+offset" using the DSYM table;
  // fault_addr is the (absolute) address to resolve.
  microfmt::println("fault at {}", microfmt::make_remote_symbol(fault_addr, resolver, ctx));
}
```

## API

| Entity | Description |
|---|---|
| `structo::debug_symtab_resolver_tag` | Empty tag type selecting the `debug_symtab_view`-backed traits specialization. |
| `microfmt::symbol_resolver_traits<structo::debug_symtab_resolver_tag>` | Stateful specialization; `context_type` is `structo::debug_symtab_view`. |
| `static bool resolve(reloco::value_ref<const context_type> view, uintptr_t addr, span<char> scratch, raw_resolved_symbol &out_raw) noexcept` | Calls `view.try_resolve(addr, scratch)`; returns `false` if it fails, otherwise fills `out_raw.symbol_name`, `symbol_base`, `is_exact` and returns `true`. |

`symbol_name` aliases `scratch` (per `debug_symtab_view::try_resolve`'s
contract), so it is valid only as long as `scratch` is. `image_name` and
`image_load_base` are left empty/zero because a `DSYM` blob only ever
describes one image.
