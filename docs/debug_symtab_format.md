<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Debug symbol table blob format (`DSYM`)

> **Status: implemented.** See `include/structo/debug_symtab.hpp` (decoder),
> `include/structo/debug_symtab_resolver.hpp` (microfmt integration), and
> `scripts/elf_symtab_to_blob.py` (ELF -> blob encoder); `docs/debug_symtab.md`
> is the per-header usage reference. This document remains the source of
> truth for the binary format itself.

## Motivation

The primary driver is a build/release practice already common in
kernel/firmware shops: `strip`/`objcopy --strip-all` (or just never
emitting a symbol table into the flashed/shipped image in the first
place) to save flash/ROM space, which is exactly the space the full
symbol table -- including every `STB_LOCAL` static function the compiler
named -- would otherwise cost on the *running target*. That stripping is
not a secrecy measure; it is pure space-saving, and it is also exactly
what makes a post-mortem backtrace on that target useless (`<unknown@
0x...>` everywhere). `DSYM` moves the symbol table *off* the running
target and into a small, separately-loaded blob: built offline from the
pre-strip ELF's `.symtab`, kept on a host/debug server/diagnostics
partition instead of in the flashed image, and loaded into a
caller-owned memory block only when a crash handler/unwinder actually
needs to resolve an address (e.g. fetched over a debug transport after a
fault, or mapped in by host-side postmortem tooling, or DMA'd in from a
separate diagnostics region). A small, allocation-free decoder then
binary-searches it to resolve an address to its nearest preceding symbol
name, in the same spirit as POSIX `dladdr()`/microfmt's
`dl_symbol_resolver.hpp`, but without a dynamic linker, a resident copy
of the full symbol table, or any requirement that the symbol table ship
inside the running binary at all.

Because the whole point is recovering symbols the shipped binary no
longer carries, **the encoder keeps every address-bearing symbol by
default, including `STB_LOCAL` statics and hidden-visibility symbols** --
see [Encoder rules](#encoder-rules-elf---blob); dropping categories of
symbols is an explicit opt-out (`--drop-local`, `--drop-hidden-
visibility`), not the default, and is a separate concern from the
format's own space-saving knobs:

1. **Truncation.** The encoder caps every symbol name at a configurable
   `max_name_len` (default 31 bytes); longer names are cut and marked
   (see [Name records](#name-records)), trading perfect fidelity for a
   bounded per-symbol cost -- the dominant cost for C++ binaries with long
   mangled names.
2. **Compact encoding.** Delta/LEB128 addresses, intra-group name
   de-duplication, and no separate string table (see [Design
   summary](#design-summary)) keep the *kept* symbols' table itself small,
   so "keep everything by default" stays affordable.
3. **Skip what's already resolvable another way.** If the source ELF is
   shipped as a shared object (or any image with a `.dynsym`/dynamic
   section), its exported symbols are already resolvable at runtime
   through the ordinary dynamic-linker path (`dladdr()`, `dl_symbol_
   resolver.hpp`, or an in-kernel equivalent) -- keeping them in this blob
   too would just duplicate information the unwinder can get elsewhere
   for free. The encoder cross-references `.dynsym` and drops any
   already-exported address from the blob by default (see [Encoder
   rules](#encoder-rules-elf---blob)); a resolver pairs this blob with a
   fallback to the image's own dynamic-symbol lookup (see [Resolution
   order](#resolution-order-debug-blob-then-the-images-own-exported-symbols)).
4. **Optional additional redaction.** `--exclude-regex`/`--exclude-list`
   remain available for a caller who, independent of the space-saving
   goal, also wants specific names (e.g. proprietary algorithm names)
   left out of a blob that leaves the building. This is secondary to the
   format's main purpose and off by default. The decoder cannot
   distinguish "never existed", "dropped by the default type/binding
   filter", "already exported dynamically", and "explicitly redacted" --
   all four just mean an address in that range resolves to whatever
   symbol precedes it in *this* blob, if any (a caller chaining in a
   dynamic-symbol fallback still resolves the third case correctly).

## Design summary

* Sorted-by-address, binary-searchable symbol table with **delta +
  LEB128**-encoded addresses and intra-group name de-duplication, decoded
  through a sparse **checkpoint table** so a lookup never has to decode
  the whole blob -- only one bounded-size group.
* Name bytes are additionally **Huffman-coded**, with a small canonical
  decode table built from -- and embedded alongside -- the *same blob's*
  own (already-truncated) kept symbol names, so the code is always a
  good fit for this blob's alphabet with no corpus-mismatch risk and no
  separate decoder-side table to keep in sync (see [Name
  records](#name-records)).
* Fixed 56-byte header, little-endian regardless of host/target
  endianness (the decoder always byte-swaps; this matches "blob built on
  a dev machine, loaded on a possibly different-endian target").
* A copied GNU build-ID lets the decoder confirm, before trusting any
  resolved name, that the blob was actually built from the ELF the
  target is currently running (see [Build-ID
  correlation](#build-id-correlation-matching-a-blob-to-its-stripped-binary)).
* No allocation anywhere in the decoder: resolution reads directly out of
  the caller's `span<const std::byte>` blob and copies at most
  `max_name_len` bytes into a caller-supplied scratch buffer.
* Address width (32- or 64-bit) is a per-blob flag, auto-detected by the
  encoder from the input ELF's class (`ELFCLASS32`/`ELFCLASS64`); the
  decoder branches on it at runtime (no template parameter), so one
  decoder binary can resolve blobs built for either width.

## Blob layout

```
+-------------------------------+  offset 0
| header (56 bytes, fixed)      |
+-------------------------------+  header.huffman_table_offset (if huffman_symbol_count > 0)
| Huffman decode table           |
| (header.huffman_table_size     |
|  bytes)                        |
+-------------------------------+  header.build_id_offset (if present)
| build-ID bytes                 |
| (header.build_id_size bytes)   |
+-------------------------------+  header.checkpoint_table_offset
| checkpoint table               |
| (header.checkpoint_count       |
|  records)                      |
+-------------------------------+  header.entry_stream_offset
| entry stream                   |
| (header.entry_stream_size      |
|  bytes)                        |
+-------------------------------+  end of blob
```

All four regions are placed by the encoder (default: the Huffman decode
table, then build-ID bytes, then the checkpoint table, then the entry
stream, each immediately after the previous one), but the decoder always
follows the header's offset fields rather than assuming adjacency, so an
embedder is free to place additional, decoder-opaque data between or
around them.

### Header (56 bytes)

All multi-byte fields are little-endian.

| Offset | Size | Field | Description |
|---|---|---|---|
| 0 | 4 | `magic` | ASCII `"DSY2"` (`0x32595344` as a little-endian `uint32_t`load) -- identifies the format *and* version 2 in one check; version 1 (`"DSY1"`, uncompressed raw name bytes) is superseded by this Huffman-coded format, not kept as a parallel decode path, so an old (v1-only) decoder's magic check rejects a v2 blob outright instead of silently misreading it. |
| 4 | 1 | `addr_width` | `4` or `8` -- bytes per address field, auto-detected from the source ELF's class. |
| 5 | 1 | `max_name_len` | Configured truncation cap in bytes; `1..=127`. |
| 6 | 1 | `truncation_marker` | ASCII byte appended in place of a truncated name's last character (default `'~'`); see [Name records](#name-records). |
| 7 | 1 | `reserved0` | Zero; reserved. |
| 8 | 2 | `group_size` | Symbols per checkpoint group (default 16); bounds a lookup's worst-case linear scan. |
| 10 | 2 | `reserved1` | Zero; reserved. |
| 12 | 4 | `symbol_count` | Total number of symbol entries in the blob. |
| 16 | 4 | `checkpoint_count` | `ceil(symbol_count / group_size)`; `0` if `symbol_count == 0`. |
| 20 | 4 | `checkpoint_table_offset` | Byte offset from blob start to the checkpoint table. |
| 24 | 4 | `entry_stream_offset` | Byte offset from blob start to the entry stream. |
| 28 | 4 | `entry_stream_size` | Size in bytes of the entry stream. |
| 32 | 4 | `build_id_offset` | Byte offset from blob start to the raw build-ID bytes copied from the source ELF's `.note.gnu.build-id` (see [Build-ID correlation](#build-id-correlation-matching-a-blob-to-its-stripped-binary)); `0` if the ELF had no build-ID note. |
| 36 | 1 | `build_id_size` | Length in bytes of the build-ID (typically 20 for the default SHA-1 note); `0` if absent. |
| 37 | 3 | `reserved2` | Zero; reserved. |
| 40 | 4 | `payload_crc32` | CRC-32 (IEEE 802.3 polynomial) of every byte in the blob *after* the header, i.e. `[56, blob.size())` -- which includes the Huffman decode table, build-ID bytes, checkpoint table, and entry stream; validates a blob loaded over an unreliable transport (flash, debug probe, ...) before any offsets inside it are trusted. |
| 44 | 4 | `huffman_table_offset` | Byte offset from blob start to the [Huffman decode table](#huffman-decode-table); `0` if `huffman_symbol_count == 0` (see below). |
| 48 | 4 | `huffman_table_size` | Size in bytes of the Huffman decode table; `0` if `huffman_symbol_count == 0`. |
| 52 | 2 | `huffman_symbol_count` | Number of distinct byte values in the per-blob Huffman alphabet; `0` means **raw fallback** -- name bytes are stored uncompressed (same as `"DSY1"`'s name records), used for a degenerate/empty alphabet (fewer than 2 distinct bytes across every kept name) where a code table could not help. |
| 54 | 2 | `reserved3` | Zero; reserved. |

The decoder's very first step is always: check `blob.size() >= 56`, check
`magic`, check `addr_width` is `4` or `8`, then (if the caller asked for
integrity checking) recompute `payload_crc32` over `[56, blob.size())`
and compare. Every other field is only trusted after that.

### Huffman decode table

Present only when `huffman_symbol_count > 0`; this blob's own kept
(already-truncated) symbol name bytes are frequency-analyzed by the
encoder and assigned a **canonical Huffman code**, with the code
itself -- not just its use -- embedded directly in the blob, so the
decoder never ships or assumes any fixed, corpus-dependent table of its
own:

| Size | Field | Description |
|---|---|---|
| 1 | `max_code_len` | Longest code length in bits, `1..=15` (capped the same way DEFLATE caps its literal/length codes, via the length-limiting fix-up described in [Encoder rules](#encoder-rules-elf---blob)). |
| `max_code_len` | `length_counts[1..=max_code_len]` | One byte per code length `1..=max_code_len`: how many symbols in the alphabet have that code length. |
| `huffman_symbol_count` | `sorted_symbols` | One byte per alphabet symbol (a raw byte value, `0..=255`), in **canonical order**: ascending by code length, then ascending by byte value within the same length. |

`huffman_table_size` is always exactly `1 + max_code_len +
huffman_symbol_count`, redundant with the fields above but checked by the
decoder at `try_create()` time as a cheap corruption guard before any
code is decoded. From `length_counts`/`sorted_symbols` alone, both the
encoder and decoder reconstruct the same canonical codes with the
standard algorithm (first code of each length is `(first_code[len-1] +
length_counts[len-1]) << 1`, codes within a length increase by one in
`sorted_symbols` order) -- no code values are stored directly, only the
lengths, exactly like a DEFLATE dynamic Huffman block's code-length
sequence.

### Build-ID correlation: matching a blob to its stripped binary

Because the blob is loaded independently of the (stripped) binary it
describes -- fetched separately, possibly updated out of step with a
reflash -- a resolved address is only meaningful if the blob was actually
built from the same ELF the target is currently running. The encoder
copies the source ELF's GNU build-ID note (`.note.gnu.build-id`, the same
identifier `gdb`/`eu-unstrip`/crash-reporting tooling already use to pair
a stripped binary with its separate debug-info file) verbatim into the
blob, referenced by `build_id_offset`/`build_id_size`, so
`debug_symtab_view::try_matches_build_id(span<const std::byte>
running_build_id)` can confirm the two agree before any resolved name is
trusted -- the same check `gdb`'s `"the debug information is out of
date"` warning is built on, just performed by the target/host crash
handler instead of `gdb`. If the source ELF was linked without
`--build-id` (no note present), `build_id_size` is `0`; the encoder
prints a warning recommending the link flag, and a caller skips/forgoes
the correlation check in that case (it is advisory, not required for
`try_resolve()` to function).

### Checkpoint table

`checkpoint_count` fixed-size records, **sorted ascending by `address`**
(a direct consequence of symbols being emitted in address order), each:

| Size | Field | Description |
|---|---|---|
| `addr_width` (4 or 8) | `address` | Absolute address of the group's first symbol. |
| 4 | `stream_offset` | Byte offset from `entry_stream_offset` to the group's first entry record (always that entry's [name record](#name-records) -- see below, the first entry in a group never stores its own address, since it equals the checkpoint's `address`). |

Record size is therefore `addr_width + 4` bytes (8 or 12). A lookup
binary-searches this table directly (`span::binary_search_by` on
`address`) to find the last checkpoint `<= target`, an `O(log
checkpoint_count)` step with no decoding of the entry stream at all.

### Entry stream

A flat sequence of `checkpoint_count` groups, back to back, each holding
up to `group_size` entries (the last group may hold fewer, if
`symbol_count` is not a multiple of `group_size`). Within a group:

* **Entry 0** (the checkpoint's own symbol): just a [name
  record](#name-records) -- its address is the checkpoint's `address`
  field, so it is never re-encoded.
* **Entries 1..group_size-1**: a ULEB128-encoded `address_delta` (always
  `> 0`; strictly ascending addresses, duplicate addresses are merged by
  the encoder -- see [Encoder rules](#encoder-rules-elf---blob)) followed
  by a [name record](#name-records).

A group is **self-contained**: decoding it never needs state from a
previous or following group, which is what lets a lookup jump straight to
a checkpoint's `stream_offset` and scan forward at most `group_size - 1`
entries without touching the rest of the blob.

### Name records

Each entry's name record is:

| Size | Field | Description |
|---|---|---|
| 1 | `control` | Bit 7: `repeat_previous` flag. Bits 6..0: `length` (`0..=127`), the number of *decoded* name bytes (not the number of bits/bytes the coded form occupies). |
| variable (0 if `repeat_previous`) | `coded_bytes` | `length` symbols, each coded per [Huffman decode table](#huffman-decode-table) and packed MSB-first across bytes with no padding *between* symbols; after the last symbol, the stream is padded with zero bits up to the next byte boundary, so the record immediately following (whether a ULEB128 address delta or the next entry's own `control` byte) always starts at a fresh byte -- no bit-cursor state is ever carried across name records. When `huffman_symbol_count == 0` (raw fallback), `coded_bytes` is simply `length` raw bytes, byte-aligned already, identical to `"DSY1"`. |

If the original symbol name was cut to fit `max_name_len`, the final
decoded byte is `truncation_marker` instead of the original character --
`truncation_marker` is just another byte value in the Huffman alphabet
like any other, coded the same way.

`repeat_previous` means "identical to the immediately preceding decoded
entry's name in this same group" -- a cheap de-duplication win for runs of
truncated names that collapse to the same prefix (common with templated/
overloaded names once cut to `max_name_len`), independent of (and
additional to) Huffman-coding the non-repeated case, without a separate
string table or offsets to maintain. **Entry 0 of every group must not set
`repeat_previous`** (there is no preceding entry to reference within a
self-contained group), so a group's first name is always written out in
full; the encoder enforces this.

Names are inlined directly in the entry stream rather than held in a
separate deduplicated string table with per-entry offsets: with names
already capped at `max_name_len` and most backtraces only ever touching a
handful of groups, this removes a whole offset field per entry (4 bytes)
and keeps every group fully self-decodable without a second table to
cross-reference.

Byte-aligning every record (rather than packing bits continuously across
the whole entry stream) costs up to 7 wasted bits per non-repeated name
-- negligible next to the bits saved by Huffman-coding the name bytes
themselves -- in exchange for every record remaining independently
decodable without tracking a running bit offset through the rest of the
group.

## Lookup algorithm

Given a target address `addr` and a blob already magic/width-checked:

1. Binary-search the checkpoint table for the last record with
   `address <= addr`. If none (addr precedes every symbol), resolution
   fails (no symbol).
2. Set `current = checkpoint.address`, `name = <none>`,
   `best_address = checkpoint.address`, `best_name = <entry 0's name
   record, decoded>` (entry 0's address is always `current`).
3. If `addr == checkpoint.address`, stop: entry 0 is the (exact) match.
4. Otherwise walk entries `1..group_size-1` from `checkpoint.stream_offset`
   (stopping early if the group is shorter than `group_size`, per
   `symbol_count`): decode the ULEB128 delta, add it to `current`; if
   `current > addr`, stop -- the *previous* decoded entry is the match;
   otherwise this entry becomes the new best candidate (`best_address =
   current`, `best_name` updated, and `is_exact = (current == addr)`
   lets the walk stop immediately on an exact hit too) and the walk
   continues.
5. If the walk exhausts the group without exceeding `addr`, the group's
   last entry is the match (covers "address falls after the last symbol
   in the table", e.g. `.text`'s tail).
6. Copy `best_name` (already `<= max_name_len` bytes) into the caller's
   scratch `span<char>` (truncated further, with no marker added, if the
   caller's scratch is smaller than `max_name_len` -- same
   "caller-owned storage, bounded copy" contract as
   `symbol_resolution_context::scratch` in microfmt). Return
   `best_address` and whether the match was exact.

Worst case per lookup: `O(log checkpoint_count)` for the binary search
plus a bounded `O(group_size)` scan/decode -- independent of
`symbol_count` for the second part, so larger blobs only grow the cheap
binary-search term.

## Encoder rules (ELF -> blob)

`scripts/elf_symtab_to_blob.py` (Python, using `pyelftools`, matching the
style of `scripts/ttf_to_font_header.py`/`scripts/sysregs/
gen_sysreg_headers.py`):

1. Read `.symtab` (fall back to `.dynsym` with a warning if `.symtab` was
   stripped -- far fewer symbols, but still useful) from the input ELF.
   Auto-detect `addr_width` from the ELF class (`ELFCLASS32` ->
   4, `ELFCLASS64` -> 8). Read `.note.gnu.build-id` if present, for
   `build_id_offset`/`build_id_size`.
2. **Filter** -- default keeps everything address-bearing, since the goal
   is restoring symbols a size-optimized release strip already removed
   from the target, not adding a second layer of hiding:
   * Default: keep `STT_FUNC` symbols (`--include-objects` also keeps
     `STT_OBJECT`) with `st_shndx != SHN_UNDEF` and `st_value != 0`;
     `STT_NOTYPE`/`STT_FILE`/`STT_SECTION` are always dropped (never
     address-resolution-relevant). **`STB_LOCAL` binding and
     `STV_HIDDEN`/`STV_INTERNAL` visibility are kept by default** --
     these are precisely the static-function names a stripped release
     binary loses first, and recovering them is this format's main
     job. `--drop-local`/`--drop-hidden-visibility` opt back *out* for a
     caller who wants a smaller blob and is fine losing those names too.
   * Optional, off by default: `--exclude-regex PATTERN` (repeatable)
     and/or `--exclude-list FILE` (one exact name or `glob:`-prefixed
     pattern per line) drop additionally named symbols -- independent,
     deliberate redaction layered on top of the above, for the rarer
     case a caller also wants specific names left out of a blob that
     leaves the building.
3. **Drop already-exported dynamic symbols.** If the ELF carries a
   non-empty `.dynsym` (shared objects, PIE executables, and any
   custom-loaded "kernel as a `.so`" image), read every defined dynamic
   symbol's address and drop that address from the surviving `.symtab`-
   derived set -- it is already resolvable at runtime through the
   dynamic-linker path without this blob (see [Resolution
   order](#resolution-order-debug-blob-then-the-images-own-exported-symbols)).
   This is on by default whenever `.dynsym` is non-empty; `--keep-
   dynamic` opts out (e.g. the target has no dynamic linker actually
   resolving `.dynsym` at runtime, so the blob should stay
   self-contained). Matching is by address, not name: an address present
   in both tables is dropped regardless of whether the two names agree.
4. **Merge duplicate addresses**: if two surviving symbols share an
   address (aliases, ifunc resolvers, ...), keep the first in symbol-table
   order and drop the rest (logged at `--verbose`) -- the format has one
   name per address.
5. **Sort** the surviving set ascending by address.
6. **Truncate** every name to `max_name_len` bytes, replacing the final
   byte with `truncation_marker` when a cut occurred.
7. **Build the Huffman code.** Over every name byte that will actually be
   written out literally (i.e. excluding bytes belonging to a name that
   `repeat_previous` will dedup away -- see step 8), count byte
   frequencies and build an unrestricted Huffman tree, then apply a
   length-limiting fix-up (the classic zlib/DEFLATE "overflow"
   redistribution: clamp any code longer than `max_code_len = 15` down to
   15, then repeatedly donate one code from the deepest-still-short
   length bucket to the next one up until the Kraft inequality holds
   again) so no code exceeds 15 bits, then assign canonical codes
   (ascending by length, then by byte value) the same way DEFLATE assigns
   dynamic Huffman codes. If fewer than 2 distinct bytes appear (empty or
   single-byte-alphabet corpus), skip this step entirely and use the raw
   fallback (`huffman_symbol_count = 0`, name bytes written uncompressed
   in step 8).
8. **Group** into `group_size`-sized runs and emit the checkpoint table +
   entry stream exactly as decoded above (including the entry-0-never-
   repeats and strictly-ascending-delta invariants), bit-packing each
   literal name's bytes per the Huffman code from step 7 (or writing them
   raw, if that step was skipped) and byte-aligning after each record.
9. Emit the 56-byte header and, if `huffman_symbol_count > 0`, the
   Huffman decode table from step 7 (computing `payload_crc32` over the
   finished Huffman table + build-ID bytes + checkpoint table + entry
   stream), and write the result as a raw `.bin` blob (default) or, with
   `--format c-array`, a `#include`-able C++ header exposing it as a
   `static constexpr std::byte[]` (matching `ttf_to_font_header.py`'s
   "raw asset or generated header, caller's choice" convention) for
   embedding directly into a firmware image, a separate diagnostics
   partition, or a host-side symbol server, independent of the stripped
   binary itself.

CLI sketch:

```
elf_symtab_to_blob.py INPUT.elf -o SYMBOLS.bin \
    [--max-name-len 31] [--group-size 16] [--truncation-marker '~'] \
    [--include-objects] [--drop-local] [--drop-hidden-visibility] \
    [--keep-dynamic] \
    [--exclude-regex PATTERN]... [--exclude-list FILE] \
    [--format {bin,c-array}] [--array-name NAME] [--namespace NS] \
    [--verbose]
```

## Decoder API (`include/structo/debug_symtab.hpp`)

Implemented as sketched below (see `docs/debug_symtab.md` for the
usage-oriented reference). Follows this repo's established
conventions -- `try_`-prefixed fallible construction/lookup returning
`reloco::result<...>`/`reloco::optional<...>`, no allocation, no
exceptions (see `docs/coding-guide.md`).

```cpp
namespace structo {

class debug_symtab_view {
public:
  // Validates magic/addr_width/size and (optionally) payload_crc32.
  [[nodiscard]] static reloco::result<debug_symtab_view>
  try_create(reloco::span<const std::byte> blob, bool verify_crc = true) noexcept;

  // Advisory: confirms the blob's copied GNU build-ID matches the
  // currently-running binary's; see "Build-ID correlation" above.
  // Returns true unconditionally if the blob carries no build-ID.
  [[nodiscard]] bool try_matches_build_id(reloco::span<const std::byte> running_build_id) const noexcept;

  struct resolved {
    uintptr_t symbol_base{0};
    reloco::string_view name{};  // Points into the caller's scratch span.
    bool is_exact{false};
    bool name_truncated{false};  // Last copied byte was `truncation_marker`.
  };

  // O(log checkpoints) + O(group_size); copies into `scratch`, never allocates.
  [[nodiscard]] reloco::optional<resolved>
  try_resolve(uintptr_t addr, reloco::span<char> scratch) const noexcept;

private:
  reloco::span<const std::byte> blob_;
};

} // namespace structo
```

### microfmt integration

A `structo::debug_symtab_resolver_tag` specializing
`microfmt::symbol_resolver_traits<Tag>` (`context_type =
debug_symtab_view`) lets a `debug_symtab_view` drop straight into
`microfmt::remote_symbol_view`/`make_remote_symbol`, exactly like
`dl_symbol_resolver_tag` today:

```cpp
auto view_blob = structo::debug_symtab_view::try_create(blob_span).value();
auto resolver = microfmt::symbol_resolver_ref::make<structo::debug_symtab_resolver_tag>(view_blob);

char name_scratch[32]; // >= configured max_name_len to avoid double truncation.
microfmt::symbol_resolution_context symbol_context{name_scratch};
microfmt::format_to(out, "{:#}", microfmt::make_remote_symbol(pc, resolver, symbol_context));
// e.g. "my_namespace::my_long_function_na~+0x24"
```

`image_name`/`image_load_base` are left empty/`0` in `raw_resolved_symbol`
(this format describes one flat address space/image; multi-image support,
if ever needed, is a natural `v2` extension via the header's `reserved`
fields rather than a breaking change here).

### Resolution order: debug blob, then the image's own exported symbols

Because the encoder drops addresses already present in `.dynsym` by
default, a `debug_symtab_view` alone under-reports symbols for an image
that exports any -- those addresses resolve correctly only once a second,
fallback resolver covering the image's own dynamic symbols is consulted.
The intended pattern chains two `symbol_resolver_ref`s by hand (no new
combinator type needed, since there are only ever two steps and no
blending of results like `hw_rng_combinator` does): try the compact blob
first (it covers everything `.dynsym` does not -- stripped locals,
hidden-visibility names, anything truncated/merged by the encoder), and
only on a miss fall back to the image's own dynamic-symbol resolver
(`dl_symbol_resolver_tag` for a userspace `dladdr()`-capable target, or an
equivalent in-kernel `.dynsym`/`DT_SYMTAB` walker for a freestanding
loader):

```cpp
auto debug_blob = structo::debug_symtab_view::try_create(blob_span).value();
auto debug_resolver = microfmt::symbol_resolver_ref::make<structo::debug_symtab_resolver_tag>(debug_blob);
auto dynamic_resolver = microfmt::symbol_resolver_ref::make<microfmt::dl_symbol_resolver_tag>();

char name_scratch[32];
microfmt::raw_resolved_symbol raw{};
bool ok = debug_resolver.resolve(pc, microfmt::span<char>(name_scratch), raw)
       || dynamic_resolver.resolve(pc, microfmt::span<char>(name_scratch), raw);
```

This ordering also means the blob's own `--drop-local`/`--drop-hidden-
visibility`/`--keep-dynamic` flags and this two-resolver chaining are
independent choices: a blob built with `--keep-dynamic` is usable
standalone (no fallback needed, at the cost of the duplicated
`.dynsym` entries it would otherwise have skipped), while the default
(dynamic symbols excluded) requires pairing it with the fallback above to
cover the whole address space.

## Testing plan (once implemented)

* Round-trip: a small synthetic ELF (or a hand-built in-memory blob
  matching the spec) decoded back to the expected `(address, name)`
  pairs, including truncation and `repeat_previous` cases.
* Boundary addresses: before the first symbol, exactly on a symbol,
  between two symbols, after the last symbol, on the last entry of a
  group, on the first entry of a non-first group (checkpoint-boundary
  coverage).
* Corrupt/short blob rejection (bad magic, truncated header, bad
  `addr_width`, CRC mismatch).
* Build-ID correlation: matching bytes accepted, mismatched bytes
  rejected, absent build-ID (`build_id_size == 0`) always accepted.
* Huffman decode correctness: a multi-symbol alphabet round-trips
  exactly; the degenerate `huffman_symbol_count == 0` raw-fallback path
  (empty blob, or every kept name reducing to fewer than 2 distinct
  bytes); `repeat_previous` interacting correctly with Huffman-coded
  literal names; a name whose bit-packed encoding crosses one or more
  byte boundaries; a maximal `length == 127` name; a corrupt/truncated
  Huffman table (bad `max_code_len`, `huffman_table_size` mismatch,
  `length_counts` not summing to `huffman_symbol_count`) rejected by
  `try_create()`.
* `elf_symtab_to_blob.py` against a real small compiled ELF fixture,
  asserting the filter/truncation/merge rules above -- in particular that
  `STB_LOCAL`/hidden-visibility symbols are kept by default and only
  dropped with `--drop-local`/`--drop-hidden-visibility`, and that a
  `.dynsym`-exported address is excluded by default and only kept with
  `--keep-dynamic` -- run as part of the existing test suite (`tests/`)
  the same way other generated-asset tooling is exercised.

---

Please review/edit this document; implementation (`debug_symtab.hpp`,
`debug_symtab_format.hpp` internal codec primitives, the encoder script,
and tests) starts once it is approved.
