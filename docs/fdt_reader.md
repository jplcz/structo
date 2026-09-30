<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `fdt_reader`

`include/structo/fdt_reader.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`
> and re-homed into the `structo` namespace (the header adds `using
> namespace reloco;` so unmoved reloco types remain reachable unqualified).
> Documented here as extracted from reloco's own reference, with some
> prose still to be reconciled with the new namespace split.

Read-only, bounds-checked view over a caller-owned Flattened Device Tree
(DTB, `/dts-v1/`) blob, built on `iterator_adaptor` (see `include/structo/iterator.hpp`) so the
struct block can be walked with a plain range-`for` loop. `try_create(span<const std::byte>)` is the
only way to obtain one -- it validates the fixed 40-byte header (magic,
`version`, `last_comp_version`, and every offset/size field checked
against the blob via `span::try_subspan`) up front, in O(1), before any
struct-block parsing happens, returning `error::invalid_argument` (bad
magic/misaligned/misconfigured header) or `error::out_of_bounds` (an
offset/size field points outside the blob) instead of trapping on
malformed or attacker-controlled input:

```cpp
auto made = structo::fdt::fdt_reader::try_create(structo::span<const std::byte>(blob, blob_size));
if (!made)
  return made.error();
auto reader = std::move(made).value();

for (auto entry : reader.mem_reserves()) {
  if (!entry)
    return entry.error();
  // entry->address, entry->size
}

for (auto ev : reader) {
  if (!ev)
    return ev.error();
  switch (ev->kind) {
  case structo::fdt::fdt_event_kind::begin_node:
    // ev->node_name
    break;
  case structo::fdt::fdt_event_kind::end_node:
    break;
  case structo::fdt::fdt_event_kind::property:
    // ev->prop.name, ev->prop.try_as_u32()/try_as_u64()/try_as_string()
    break;
  }
}
```

Each `next()` call yields a `result<fdt_event>`: `begin_node`/`end_node`
bracket a node (`node_name` valid for `begin_node`), and `property` carries
a `fdt_property_view` (`name` plus the raw `value` bytes, with fallible
`try_as_u32()`/`try_as_u64()`/`try_as_string()` convenience accessors that
bounds- and format-check before decoding). Every read -- token, string,
property length -- goes through a `span::try_subspan`-checked helper, so a
truncated, misaligned, or otherwise corrupt blob fails that one event with
`error::invalid_argument` or `error::out_of_bounds` and then permanently
stops iterating (the fused `iterator_adaptor` contract), rather than
reading past the blob or trapping. `mem_reserves()` returns a second,
independent `mem_reserve_iterator` over the `/memreserve/`-style memory
reservation list, which stops at the `{0, 0}` terminator entry or fails
with `error::invalid_argument` if the region runs out first. Unlike
`fdt_writer`, `fdt_reader` is an ordinary copyable/movable view (no
declared move-only/poisoning semantics): copying it just forks an
independent read cursor over the same immutable bytes, with no
shared-mutation hazard. `fdt_reader` and `fdt_writer` share their binary-
format constants and big-endian codec helpers through the internal
`detail/fdt_format.hpp`, so the two can never drift apart on token IDs,
header layout, or byte order.

`try_create` needs the *whole* `totalsize`-length span up front, which is
a chicken-and-egg problem when a blob's actual length isn't already known
(e.g. a bootloader hands over just a pointer, with only the fixed 40-byte
header guaranteed readable). `fdt_reader::try_probe_size(span<const
std::byte> header)` solves that: it validates only the header's magic and
version fields and returns the declared `totalsize`, needing just
`detail::header_size` (40) bytes to be available -- so the caller can
map/copy/allocate exactly that many bytes next, then hand the full span
to `try_create`:

```cpp
auto probed = structo::fdt::fdt_reader::try_probe_size(structo::span<const std::byte>(header_bytes, 40));
if (!probed)
  return probed.error();
// *probed is the blob's totalsize; allocate/map that many bytes, then:
auto made = structo::fdt::fdt_reader::try_create(structo::span<const std::byte>(blob, *probed));
```

See also: [`fdt_writer.md`](fdt_writer.md), [`fdt_index.md`](fdt_index.md),
[`fdt_memory.md`](fdt_memory.md).
