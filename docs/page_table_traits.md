<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::arch::page_table_level` / `page_table_levels` / `page_table_entry`

`include/structo/arch/page_table_traits.hpp`

Compile-time, self-consistency-checked per-level traits for a multi-
level hardware page table, plus an opaque, architecture-tagged raw
page-table-entry handle and the extension point a future walker layer
will specialize to interpret it.

## Why this exists

Every hardware multi-level page-table format (ARM64's 4KB/16KB/64KB
granules, x86-64's 4- and 5-level long-mode paging, RISC-V's
Sv39/Sv48/Sv57) carves a virtual address into the same *shape* -- a
handful of fixed-width index fields, most-significant first, each
selecting one entry out of one level's table, followed by a page offset
-- but every architecture (and sometimes every level within one
architecture) disagrees on field widths, shift amounts, and which levels
may terminate the walk early with a large/huge/block mapping instead of
pointing at the next level's table. Following `asid_allocator.hpp`'s
architecture-agnostic-bookkeeping philosophy, this header supplies the
*shape* -- compile-time index decomposition, validated once at compile
time -- and leaves every bit of the actual entry encoding to a per-
architecture trait the embedder supplies.

## Compile-time, not runtime, level traits

A page-table format's level count, field widths, and shift amounts are
fixed by the architecture and page-granule choice a kernel is built for,
never a runtime-probed value (unlike `asid_allocator`'s `asid_bits`).
Making every level a compile-time template parameter lets `index_of()`
compile down to a single shift-and-mask with compile-time-constant
operands, and lets a mis-specified level set (one that doesn't tile the
virtual address contiguously down to the leaf page's shift) fail as a
`static_assert`, not a boot-time page-fault storm.

## What this header intentionally does NOT add (yet)

This is the *decomposition* layer only: turning a virtual address into a
`(level_0_index, level_1_index, ..., page_offset)` tuple. It deliberately
does **not** provide a page-table walker, entry allocation, or any
interpretation of what bits inside a `page_table_entry<Tag>` mean --
that is `page_table_entry_traits<Tag>`'s job, declared as an extension
point (mirroring `target_ptr.hpp`'s `target_ptr_space_traits<SpaceTag>`
pattern) for a future walker layer to specialize and consume.

## API

- `page_table_level<IndexBits, Shift, AllowsLeaf = true>` -- one level's
  index-decoding traits: `index_bits`, `shift`, `entry_count` (`1 <<
  IndexBits`), `index_mask`, `allows_leaf`, and `index_of(va)`.
  `AllowsLeaf` is meaningless (always effectively `true`) for whichever
  level is last in a `page_table_levels` list -- there is no "next
  level" to continue to there.
- `page_table_levels<LeafPageTraits, VaBits, Levels...>` -- the
  aggregate, root level first:
  - `level_count`, `va_bits`, `leaf_page_traits`.
  - `level<LevelIndex>` -- the `page_table_level<...>` type for that level.
  - `entry_count<LevelIndex>()`, `allows_leaf<LevelIndex>()`.
  - `index_of<LevelIndex>(va)` -- that level's table index.
  - `page_offset(va)` -- byte offset within the final, leaf-level page.
  - `static_assert`s at instantiation time that `Levels...` tile the
    virtual address contiguously down to `LeafPageTraits::page_shift`,
    and that the root level covers exactly `VaBits` significant bits.
- `page_table_entry<Tag, Int = std::uint64_t>` -- opaque, tagged raw
  entry storage: `value` (raw bits), `is_null()`/`operator bool` (`value
  == 0` convention), `operator==`/`!=`. Carries no interpretation of any
  other bit; entries from unrelated `Tag`s are unrelated types, exactly
  as `tagged_asid<Tag>` keeps a process ASID and a VMID apart.
- `page_table_entry_traits<Tag>` -- declared, not defined; a future
  walker layer's specialization point (see the header's Doxygen cookbook
  for the expected shape: `is_present`/`is_leaf`/`child_table_addr`/
  `leaf_frame_addr`/`make_table_entry`/`make_leaf_entry`).

## Example: ARM64, 4KB granule, 4-level (48-bit VA)

```cpp
using arm64_4k_4level = structo::arch::page_table_levels<
    structo::page_4k, 48,
    structo::arch::page_table_level<9, 39, false>, // L0 (root): no block mappings on this granule
    structo::arch::page_table_level<9, 30, true>,   // L1: may terminate early with a 1GB block
    structo::arch::page_table_level<9, 21, true>,   // L2: may terminate early with a 2MB block
    structo::arch::page_table_level<9, 12, true>    // L3 (leaf-adjacent): always a 4KB page
>;

void decompose(std::uintptr_t va) {
  auto l0 = arm64_4k_4level::index_of<0>(va); // bits [47:39]
  auto l1 = arm64_4k_4level::index_of<1>(va); // bits [38:30]
  auto l2 = arm64_4k_4level::index_of<2>(va); // bits [29:21]
  auto l3 = arm64_4k_4level::index_of<3>(va); // bits [20:12]
  auto offset = arm64_4k_4level::page_offset(va); // bits [11:0]
}
```

Other shapes are expressed the same way -- e.g. RISC-V Sv39 (3 levels,
39-bit VA):

```cpp
using sv39 = structo::arch::page_table_levels<
    structo::page_4k, 39,
    structo::arch::page_table_level<9, 30, true>,
    structo::arch::page_table_level<9, 21, true>,
    structo::arch::page_table_level<9, 12, true>
>;
```

A misconfigured level set (a gap or overlap between levels, or a root
level that doesn't cover exactly `VaBits`) fails to compile with a
`static_assert` naming the violated invariant, rather than silently
producing wrong index math.

See also: [`asid_allocator.md`](asid_allocator.md) (the architecture-
agnostic-bookkeeping philosophy this header follows), [`target_ptr.md`](target_ptr.md)
(the `target_ptr_space_traits<SpaceTag>` declared-but-undefined-hook
pattern `page_table_entry_traits<Tag>` mirrors), [`phys_page.md`](phys_page.md)
(`page_traits<Size, Shift>`, used here as the leaf-page-size parameter),
[`page_table_arch_configs.md`](page_table_arch_configs.md) (ready-made
configs built from these primitives for ARM64, ARMv7-LPAE, RISC-V, and
x86/x86-64), [`page_table_entry_fields.md`](page_table_entry_fields.md)
(real `page_table_entry_traits<Tag>` specializations with named field
accessors for each of those architectures).
