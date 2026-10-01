# Per-architecture page table configurations

`structo/arch/page_table_traits.hpp` gives you the generic primitives
(`page_table_level<...>`, `page_table_levels<...>`) to describe *any*
multi-level hardware page table. Hand-assembling the right shift/index-bit
numbers for a real CPU architecture is tedious and easy to get subtly
wrong (an off-by-one in a shift silently produces a table that compiles
but decodes addresses incorrectly at runtime). The headers below are
ready-made `page_table_levels<...>` aliases for the architectures/
granules structo's target platforms (kernels, hypervisors, realms)
actually run on, so callers normally never need to hand-assemble a
config themselves.

Each alias is just a type: it costs nothing at runtime, and using it
with `page_table_range.hpp`/`page_table_occupancy.hpp` works exactly as
it would with a hand-written config -- these headers only remove the
need to recompute the numbers.

## `structo/arch/arm64/page_table_traits.hpp` -- `structo::arch::arm64`

ARMv8-A/AArch64 (VMSAv8-64). AArch64 doesn't always start its
translation-table walk at level 0: `TCR_ELx.{T0SZ,T1SZ}` can shrink the
input address size so the walk starts at whichever level's table already
covers the needed VA range, skipping the (otherwise wasted) levels
above it. The provided configs reflect this with a `level1`/`level2`
naming scheme: the suffix names the level the walk is *rooted* at, not
the number of levels.

The architecture defines three translation granules (4KB, 16KB, 64KB);
all three reshape every level's index-bit width (and which levels permit
an early-terminating block mapping), so each granule needs its own
`page_table_levels<...>` config, not just a different `LeafPageTraits`:

| Config | Granule | Root level | Levels | VA bits | VA space |
|---|---|---|---|---|---|
| `level1` | 4KB | L1 | 3 | 39 | 512GB |
| `level2` | 4KB | L2 | 2 | 30 | 1GB |
| `level1_16k` | 16KB | L1 | 3 | 47 | 128TB |
| `level2_16k` | 16KB | L2 | 2 | 36 | 64GB |
| `level1_64k` | 64KB | L1 | 3 | 48 | 256TB |
| `level2_64k` | 64KB | L2 | 2 | 42 | 4TB |

The unsuffixed `level1`/`level2` names are the original 4KB-granule
configs and are unchanged by the addition of the `_16k`/`_64k` variants,
so existing 4KB-only code keeps compiling without modification.

Which levels permit an early-terminating block mapping is granule-
dependent:

- **4KB**: level 1 (1GB blocks) and level 2 (2MB blocks).
- **16KB**: level 2 only (32MB blocks) -- level 1 cannot block-map.
- **64KB**: level 2 only (512MB blocks) -- level 1 cannot block-map.

`level1_64k`'s root is also special: rather than the full 13 index bits
every other 64KB-granule level uses (one full 64KB table = 8192
entries), it only needs 6 index bits (64 entries) to reach 48-bit VA.
The architecture lets that top-level table be "folded"/truncated to just
those 64 entries instead of a full 64KB table, saving 63/64 of what
would otherwise be unused table memory. `page_table_level<6, 42, false>`
expresses exactly that -- the generic primitive doesn't care whether a
level's table happens to be smaller than the granule, only that the
index-bit tiling is self-consistent.

A level-0-rooted, full 4-level, 48-bit, 4KB-granule configuration is
also legal (256TB of VA space) -- see `page_table_traits.hpp`'s own
file-level `@code` example, which already demonstrates that shape
generically; it is not repeated here.

## `structo/arch/arm/page_table_traits.hpp` -- `structo::arch::arm::lpae`

ARMv7 with the Large Physical Address Extension (LPAE). LPAE is
architecturally a single-granule (4KB) extension -- unlike AArch64, it
has no 16KB/64KB granule option, so there is only one page-size variant
per root level:

| Config | Root level | Levels | VA bits | Root entries |
|---|---|---|---|---|
| `level1` | L1 | 3 | 32 | 4 (only 2 index bits) |
| `level2` | L2 | 2 | 30 | 512 |

`level1`'s root table has only 4 entries (2 index bits) because LPAE's
full 32-bit VA space only needs 2 bits once the two lower 9-bit levels
are accounted for (`32 - 21 = 11`... concretely: level 1 covers bits
`[31:30]`, a 2-bit field). `level2` is shaped identically to
`arm64::level2` (same index-bit widths, same VA span) since both are
2-level, 4KB-granule, 9-bits-per-level configs.

## `structo/arch/riscv/page_table_traits.hpp` -- `structo::arch::riscv`

RISC-V Sv39/Sv48/Sv57. All three are inherently 4KB-granule -- RISC-V
has no alternate base page size for these formats, so there is no
`_16k`/`_64k`-style variant to add here. What RISC-V has instead of
alternate granules is **uniform leaf permission**: every level,
including the root, allows a leaf PTE (a "superpage"), unlike ARM64
(level 0 never allows it) or x86-64 (PML4/PML5 never allow it).

| Config | Levels | VA bits |
|---|---|---|
| `sv39` | 3 | 39 |
| `sv48` | 4 | 48 |
| `sv57` | 5 | 57 |

## `structo/arch/x86/page_table_traits.hpp` -- `structo::arch::x86`

x86/x86-64. Like RISC-V, the x86 PTE format uses a single fixed 4KB base
granule across every mode -- "huge pages" (2MB/4MB/1GB) are not a
different granule reshaping the whole hierarchy, they're already
expressed as an early-terminating leaf at the appropriate *existing*
level (`AllowsLeaf`), so there is likewise no `_16k`/`_64k`-style variant
for x86.

| Config | Levels | VA bits | Notes |
|---|---|---|---|
| `i386` | 2 | 32 | Root (PDE) allows a 4MB PSE leaf |
| `pae` | 3 | 32 | Root (PDPTE) has only 4 entries and never allows a leaf (no 1GB pages in PAE); PDE allows a 2MB leaf |
| `long_mode_4level` | 4 | 48 | PML4 never allows a leaf; PDPTE allows 1GB, PDE allows 2MB |
| `long_mode_5level` | 5 | 57 | PML5 and PML4 never allow a leaf; PDPTE allows 1GB, PDE allows 2MB |

## Adding a new configuration

If a config you need isn't listed above, write it directly with
`structo::arch::page_table_level<...>`/`page_table_levels<...>` (see
[`page_table_traits.md`](page_table_traits.md)) -- the `static_assert`
self-consistency checks there catch most mistakes (gaps, overlaps, a
root that doesn't cover exactly `VaBits`) at compile time. Consider
upstreaming genuinely reusable shapes into the matching `structo::arch::<arch>`
header instead of re-deriving them in every caller.

See also: [`page_table_traits.md`](page_table_traits.md) (the generic
primitives these configs are built from), [`page_table_range.md`](page_table_range.md)
(walking one level of any of these configs over a VA range),
[`phys_page.md`](phys_page.md) (`page_4k`/`page_16k`/`page_64k`, used
here as the leaf-page-size parameter).
