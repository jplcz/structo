<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# SDK stub `.so` generator (`elf_to_stub_so.py`)

> **Status: implemented.** See `scripts/elf_to_stub_so.py`. This
> document remains the source-of-truth design reference.

## Motivation

An SDK that lets third parties compile and link against a vendor-provided
shared library (e.g. a kernel's userspace API library, or any
`.so` the real device image ships) only needs that library's **link-time
ABI surface** at SDK-build time: its `SONAME`, its exported dynamic
symbol names/types/bindings, and their symbol-versioning
(`@@VERSION`) info. It does not need the library's actual machine code
or data -- the real, fully-implemented library is what actually loads at
runtime on the target device, resolved by `SONAME` through the ordinary
dynamic linker. Shipping the real, fully-built `.so` inside the SDK
purely so `ld`/`ld.lld` has something to resolve symbols against during
the SDK consumer's link step wastes the space of every function body,
string constant, and data section the real library carries -- often the
overwhelming majority of its file size -- for zero benefit to the SDK
consumer's build.

This tool produces a **stub `.so`**: a tiny, separately-built shared
object with the same `SONAME` and the same exported/versioned dynamic
symbol set as the base library, but with every function body and data
object replaced by a nominal placeholder. Linking against the stub
produces byte-for-byte the same dynamic symbol requirements
(`DT_NEEDED`, versioned relocations) a consumer would get linking
against the real library, so the stub is a drop-in replacement for the
SDK's link step -- while being a small fraction of the real library's
size. The real library is never shipped in the SDK at all; it is
expected to already be present on the target device/runtime image this
SDK targets.

This is comparable in spirit to Android NDK's generated stub libraries
and LLVM's `llvm-ifs` "interface stub" tool. Unlike those, this tool
produces the stub by **compiling a small, generated C translation unit
with the project's own real cross compiler** rather than hand-assembling
raw ELF bytes: the resulting `.so`'s program headers, hash tables,
dynamic section, and versioning tables are therefore produced by the
real toolchain and are correct and portable by construction, with no
need for this repo to carry a from-scratch ELF writer.

Because the tool exists purely to shrink an SDK's link-time footprint,
it deliberately does **not** attempt to preserve:
- exact function signatures (impossible to recover from an ELF's dynamic
  symbol table alone -- the stub's functions take no arguments and
  return nothing; this is harmless because the stub is never executed),
- exact object sizes (`st_size` is not reproduced; every stubbed data
  symbol is a nominal 1-byte placeholder),
- any non-default (`@`, as opposed to `@@`) compatibility symbol
  versions,
- `STT_TLS` symbols (skipped, with a warning -- rare for the kind of
  library this targets; revisit if a real need for TLS shows up),
- `STT_GNU_IFUNC` resolver semantics (emitted as an ordinary stub
  function; harmless since it is never called),
- any actual data content a consumer might `dlsym()` and read directly
  at SDK-build time (unsupported; the stub is link-only).

## What the tool reads from the base `.so`

Via `pyelftools`, from the base shared object (which may be either a
stripped release build or the full pre-strip ELF -- only `.dynsym` and
friends are read; `.symtab` is irrelevant here):
- `.dynsym`/`.dynstr`: every **defined** (`st_shndx != SHN_UNDEF`),
  **exported** (`STB_GLOBAL` or `STB_WEAK` binding) symbol of type
  `STT_FUNC`, `STT_GNU_IFUNC`, or `STT_OBJECT`. Anything else
  (`STT_TLS`, `STT_NOTYPE`, `STT_FILE`, `STT_SECTION`, local binding,
  undefined/imported symbols) is skipped.
- `.gnu.version`/`.gnu.version_d`: the per-symbol version index and the
  defined-version table, to recover each selected symbol's default
  (`@@`) version string, if the library uses symbol versioning at all.
- `DT_SONAME`: used as the stub's own `SONAME` unless `--soname`
  overrides it. If the base library has no `DT_SONAME` at all, the base
  file's name is used instead.

## What the tool generates

1. A C translation unit (`stub.c`) with, for every selected symbol, a
   local definition plus a `.symver` assembler directive binding it to
   its exported, versioned name. Verified against the real toolchain
   (GNU `ld`/binutils): `.symver` alone, with no version script at all,
   is rejected (`version node not found for symbol ...`) -- some
   version script is unavoidable, but it only needs to *declare* each
   version node and `local:`-hide the generated internal alias name;
   the actual name/version assignment is done entirely by `.symver`,
   not by listing symbols in the script. (A `static`-linkage internal
   definition does not work with this scheme -- this toolchain only
   lets a `.symver` alias become a versioned, exported symbol when the
   aliased definition itself has ordinary global linkage; hiding it
   from the dynamic symbol table is instead done via the script's
   `local:` entry.)
   - `STT_FUNC`/`STT_GNU_IFUNC`, versioned (has a `.gnu.version_d`
     entry): a plain, non-`static` function with a generated internal
     name (e.g. `__stub_0_NAME`), paired with
     `__asm__(".symver __stub_0_NAME,NAME@@VERSION");`, plus one
     `local: __stub_0_NAME;` entry in the generated version script's
     `VERSION { ... };` node.
   - `STT_FUNC`/`STT_GNU_IFUNC`, unversioned: an ordinary global
     `void NAME(void) {}`, no `.symver` and no version-script entry
     needed.
   - `STT_OBJECT`, versioned: `char __stub_0_NAME[1];` plus the same
     `.symver`/`local:` pairing as above.
   - `STT_OBJECT`, unversioned: an ordinary global `char NAME[1];`.
   All symbols are declared with C linkage (the generated file is
   compiled as C, and the stub never needs C++ name mangling since it
   only has to match the *exported* symbol name already recorded in the
   base library's `.dynsym`).

   Limitation: this generates one trivial version-script node per
   distinct version string (just a `local:` list, no `global:`
   enumeration), but does not reproduce *dependency edges between
   version nodes* (e.g. that `LIBFOO_2.0` depends on `LIBFOO_1.0`) --
   that graph only exists in a version script's node-parent syntax, not
   in anything `.symver` can express. This is acceptable here since the
   SDK's link step only needs each symbol's own version string to match
   the real library exactly, not the inter-version dependency graph.
   `STB_WEAK` binding is also not reproduced: every stubbed symbol is
   emitted as an ordinary global/default-bound symbol, since weak vs.
   global binding affects runtime symbol-override resolution, not
   whether the SDK consumer's link step can resolve the symbol.
2. The stub `.so` itself, produced by invoking the **caller-specified
   cross compiler** (there is no default -- the correct compiler/target
   triple/sysroot is entirely dependent on the SDK's target
   architecture, which this tool has no way to guess) to compile and
   link `stub.c` with `-shared -fPIC -nostdlib -Wl,--soname,<soname>`
   (plus `-Wl,--version-script,<generated script>` only when at least
   one selected symbol is versioned), plus any `--cflag` passthrough
   flags the caller supplies. `-nostdlib`
   is always passed (confirmed correct); any additional
   toolchain-specific flags (e.g. `-nostartfiles`, a target triple, a
   sysroot) are left entirely to `--cflag` passthrough rather than
   guessed, since those vary by cross compiler.
3. Optionally, if `--verify` is given, the tool re-opens the freshly
   built stub `.so` with `pyelftools` and diffs its exported symbol
   names + default versions against the base library's selected set,
   failing loudly on any mismatch (missing symbols, extra symbols
   pulled in from an accidentally-linked libc, etc.) -- this mainly
   catches cross-compiler/flag mistakes, e.g. forgetting `-nostdlib`.

## CLI

```
./scripts/elf_to_stub_so.py base_lib.so --cc /path/to/target-triple-gcc -o stub_lib.so
./scripts/elf_to_stub_so.py base_lib.so --cc clang \
    --cflag --target=aarch64-unknown-linux-gnu --cflag --sysroot=/opt/sdk/sysroot \
    --soname libfoo.so.1 -o stub_libfoo.so --verify
```

- `base_lib.so` (positional): the base shared object to derive the stub
  from (either the shipped stripped build or the full pre-strip ELF --
  only `.dynsym` is read either way).
- `--cc PATH` (**required**): the cross compiler driver to invoke to
  build the stub. There is no default.
- `-o/--output PATH` (**required**): output stub `.so` path.
- `--soname NAME`: override the stub's `SONAME` (default: the base
  library's own `DT_SONAME`, or its filename if it has none).
- `--cflag FLAG` (repeatable): an extra flag appended verbatim to the
  compiler invocation (e.g. target triple, sysroot, extra linker flags
  via `-Wl,...`). Supply as many as needed.
- `--keep-intermediate DIR`: write the generated `stub.c`
  (and the linker's `.o`, if separable) into `DIR` instead of a
  throwaway temp directory, and leave them there for inspection.
- `--verify`: after building, diff the stub's exported/versioned symbol
  set against the base library's and fail if they don't match exactly.

## Resolved design decisions

- `-nostdlib` is always passed to the stub link step (confirmed
  correct); any further toolchain-specific flags are left to `--cflag`
  passthrough.
- Symbol versioning is the whole point of matching the base library's
  link-time ABI precisely, so it is never approximated or skipped: the
  tool binds every versioned symbol to its exact `NAME@@VERSION` using
  `.symver` assembler directives embedded directly in the generated
  stub source. A minimal, generated version script is still required
  alongside it (confirmed via testing against real toolchains -- `ld`
  rejects `.symver` with no version script at all), but it only
  declares each version node and `local:`-hides the internal alias
  names; the name/version assignment itself is entirely `.symver`'s
  job. `--verify` double-checks the stub's resulting versions match the
  base library's exactly, so a toolchain/flag mistake that silently
  drops versioning is caught rather than shipped. Verified end-to-end
  against hand-built fixtures and real system libraries (`libz.so.1`,
  `libc.so.6`): a consumer linked against the generated stub records
  identical versioned relocations (e.g. `foo@MYLIB_2.0`) to one linked
  against the real library.
