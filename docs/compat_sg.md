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

## Length field units: `length_unit_bytes` / `length_unit_pages`

`sg_descriptor_layout`'s `LengthField` normally stores a literal byte
count (`length_unit_bytes`, the default for every layout, preserving the
original behavior). Some hardware descriptors instead store a *page
count* -- e.g. a PFN plus a bit count of whole pages, with no byte-level
offset field at all, since every such descriptor is implicitly
page-aligned. Passing `length_unit_pages` as `sg_descriptor_layout`'s
final template parameter tells `compact_sg_codec` to scale the length
field by `PageTraits::page_size` on encode/decode instead of treating it
as a literal byte count, and to size each chunk in whole pages (rejecting
an entry whose length is not an exact multiple of the page size, the
same way a layout with `LengthField = void` already requires one whole
page per descriptor).

For example, a descriptor packed as:

```c
struct example_sglist_entry {
  uint32_t phys_lo;        // low 32 bits of a 44-bit PFN
  uint32_t phys_hi : 12;   // high 12 bits of the PFN
  uint32_t count   : 20;   // whole 4 KB pages (not bytes) in this entry
};
```

(a little-endian 64-bit value with a contiguous 44-bit PFN in bits
`[43:0]` and a 20-bit page count in bits `[63:44]`) is described as:

```cpp
using pfn_field = reloco::bitfield<0, 44>;
using count_field = reloco::bitfield<44, 20>;
using layout = structo::sg_descriptor_layout<uint64_t, pfn_field, void, count_field, void, 0,
                                              structo::length_unit_pages>;
using codec = structo::compact_sg_codec<layout, structo::page_4k>;
```

## More example compact descriptor layouts

Besides the byte-length-field (`EncodesAndDecodesPageFragments`) and
page-count-field (above) shapes, `tests/test_sg_translation.cpp`'s
`CompactSgCodecTest` suite exercises two more realistic layouts:

### A raw, page-aligned address field (NVMe-style PRP entries)

Some formats (e.g. NVMe's PRP list entries) store the plain physical
address itself rather than a frame number right-justified at bit 0 --
the low, page-offset bits are simply always zero on the wire, since a
PRP entry always points at the start of exactly one page. Setting the
PFN field's `Offset` to `PageTraits::page_shift` instead of `0`
reproduces this: `compact_sg_codec` still does its internal arithmetic
in frame-number units, but `bitfield<page_shift, bits>` places those
bits back at their natural position in the word, leaving the low
`page_shift` bits as the implicit all-zero page offset:

```cpp
using pfn_field = reloco::bitfield<12, 52>; // bits [63:12]; bits [11:0] always zero
using layout = structo::sg_descriptor_layout<uint64_t, pfn_field>; // no Offset/Length field: one page per entry
using codec = structo::compact_sg_codec<layout, structo::page_4k>;
```

A descriptor's raw `.value()` is then exactly the page's physical
address (e.g. `0x200000`), not a shifted frame number.

### An opaque leading header word

`HeaderSize` (the `sg_descriptor_layout`/`chained_sg_layout` constructor's
second-to-last parameter) reserves a fixed number of bytes at the front
of the encoded array that `compact_sg_codec` zero-initializes and skips
over, without interpreting its contents -- e.g. for a caller-defined
entry count or cookie written in after encoding completes:

```cpp
using pfn_field = reloco::bitfield<0, 52>;
using length_field = reloco::bitfield<52, 12>;
using layout = structo::sg_descriptor_layout<uint64_t, pfn_field, void, length_field, void, sizeof(uint64_t)>;
using codec = structo::compact_sg_codec<layout, structo::page_4k>;
```

`codec::encode()` reserves one zeroed `uint64_t` before the first data
descriptor; `codec::decode()` always skips exactly `header_size` bytes
regardless of what a caller later writes there.

See also: [`sg_list.md`](sg_list.md), [`sg_translator.md`](sg_translator.md),
[`phys_page.md`](phys_page.md).
