<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::debug_symtab_view`

`include/structo/debug_symtab.hpp`, `include/structo/debug_symtab_resolver.hpp`

An allocation-free, read-only decoder for the compressed `DSYM`
(`"DSY1"`) debug symbol table blob format -- see
[`debug_symtab_format.md`](debug_symtab_format.md) for the full binary
format specification this header implements, and
`scripts/elf_symtab_to_blob.py` for the offline encoder that builds
blobs from an ELF's `.symtab`.

## Why this exists

A release build often strips (or never emits) its symbol table to save
flash/ROM space -- exactly the space the symbol table, including every
`STB_LOCAL` static the compiler named, would otherwise cost on the
*running target*. That is pure space-saving, not a secrecy measure, but
it also makes a post-mortem backtrace on that target useless
(`<unknown@0x...>` everywhere). A `DSYM` blob moves the symbol table
*off* the running target: built offline from the pre-strip ELF, kept on
a host/debug server/diagnostics partition instead of in the flashed
image, and loaded into a caller-owned memory block only when a crash
handler/unwinder actually needs to resolve an address.

## Building a blob

```sh
./scripts/elf_symtab_to_blob.py build/kernel.elf -o build/kernel.dsym
```

By default every address-bearing `.symtab` symbol is kept -- including
`STB_LOCAL` statics and `STV_HIDDEN`/`STV_INTERNAL` symbols, since those
are exactly what a release strip removes first -- except addresses
already exported via `.dynsym` (already resolvable at runtime through
the ordinary dynamic-linker path). See `--help` for the full set of
knobs (`--max-name-len`, `--group-size`, `--drop-local`,
`--drop-hidden-visibility`, `--keep-dynamic`, `--exclude-regex`,
`--exclude-list`, `--format {bin,c-array}`, ...).

## Decoding a blob

```cpp
#include <structo/debug_symtab.hpp>

reloco::span<const std::byte> blob = /* loaded from a debug transport, a
                                         diagnostics partition, ... */;
auto made = structo::debug_symtab_view::try_create(blob);
if (!made)
  return made.error(); // Bad magic/addr_width/offsets, or a CRC mismatch.
auto view = made.value();

char scratch[32]; // >= the blob's max_name_len() to avoid double truncation.
auto resolved = view.try_resolve(fault_addr, reloco::span<char>(scratch));
if (resolved) {
  // resolved->symbol_base, resolved->name (points into `scratch`),
  // resolved->is_exact, resolved->name_truncated
}
```

`try_create()` validates the blob's magic, address width, and every
offset/size field before trusting any of them, and (unless
`verify_crc = false` is passed) recomputes `payload_crc32` over the
blob's payload -- appropriate the first time a blob is loaded over an
unreliable transport. `try_resolve()` performs an
`O(log checkpoints) + O(group_size)` nearest-preceding-symbol lookup and
copies the matched (possibly truncated) name into the caller's `scratch`
span; neither method allocates or throws.

### Build-ID correlation

```cpp
if (!view.try_matches_build_id(running_build_id_bytes))
  return error::security_violation; // Wrong blob for this binary.
```

Advisory: returns `true` unconditionally if the blob carries no
build-ID (not every image is linked with `--build-id`), otherwise
compares byte-for-byte against the currently-running binary's own
`.note.gnu.build-id`. Lets a crash handler confirm an externally-loaded
blob actually matches the exact build that faulted before trusting any
name it resolves.

## microfmt integration

`include/structo/debug_symtab_resolver.hpp` adapts a `debug_symtab_view`
to microfmt's `symbol_resolver_traits<Tag>` customization point, kept
separate from `debug_symtab.hpp` so a consumer that only needs the
decoder never pays for pulling in microfmt:

```cpp
#include <structo/debug_symtab_resolver.hpp>

auto resolver = microfmt::symbol_resolver_ref::make<structo::debug_symtab_resolver_tag>(view);
char name_scratch[32];
microfmt::symbol_resolution_context ctx(name_scratch);
microfmt::println("fault at {}", microfmt::make_remote_symbol(fault_addr, resolver, ctx));
// e.g. "fault at hidden_helper_one+0x8"
```

### Resolution order: debug blob, then the image's own exported symbols

Because the encoder drops addresses already present in `.dynsym` by
default, chain two resolvers by hand when the image also exports
symbols of its own -- try the compact blob first, and only on a miss
fall back to the image's own dynamic-symbol resolver (e.g.
`microfmt::dl_symbol_resolver_tag` for a userspace `dladdr()`-capable
target):

```cpp
auto debug_resolver = microfmt::symbol_resolver_ref::make<structo::debug_symtab_resolver_tag>(view);
auto dynamic_resolver = microfmt::symbol_resolver_ref::make<microfmt::dl_symbol_resolver_tag>();

microfmt::symbol_resolution_context ctx(name_scratch);
bool ok = debug_resolver.resolve(fault_addr, ctx) || dynamic_resolver.resolve(fault_addr, ctx);
```

See also: [`debug_symtab_format.md`](debug_symtab_format.md) for the
binary format and encoder rules in full.
