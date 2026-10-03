#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause
"""Builds a tiny SDK "stub" `.so` from a base shared object
(`docs/stub_so_generator.md`): same `SONAME` and the same
exported/versioned dynamic symbol set as the base library, but with
every function body and data object replaced by a nominal placeholder.
Linking an SDK consumer against the stub produces the same dynamic
symbol requirements linking against the real library would, so the
stub is a drop-in replacement for an SDK's link step -- at a tiny
fraction of the real library's size. The real, fully-implemented
library is never shipped in the SDK; it is expected to already be
present (under the same `SONAME`) on the target this SDK is for.

Symbol versioning is reproduced with `.symver` assembler directives
embedded in the generated stub source (the same idiom glibc's own
compat symbols use), not a linker version script -- see
`docs/stub_so_generator.md` for why, and the one thing this does not
reproduce (version-node dependency edges).

Requires pyelftools (`pip install pyelftools`) and a working cross
compiler for the base library's target architecture.

Example:
    ./scripts/elf_to_stub_so.py base_lib.so --cc /path/to/target-triple-gcc -o stub_lib.so
    ./scripts/elf_to_stub_so.py base_lib.so --cc clang \\
        --cflag --target=aarch64-unknown-linux-gnu --cflag --sysroot=/opt/sdk/sysroot \\
        --soname libfoo.so.1 -o stub_libfoo.so --verify
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

try:
    from elftools.elf.elffile import ELFFile
except ImportError:  # pragma: no cover - environment-dependent
    sys.exit("error: pyelftools is required (pip install pyelftools)")

# Symbol types this tool knows how to stub out. Anything else (notably
# STT_TLS) is skipped with a warning; see docs/stub_so_generator.md.
_FUNCTION_TYPES = {"STT_FUNC", "STT_GNU_IFUNC"}
_OBJECT_TYPES = {"STT_OBJECT"}


@dataclass
class StubSymbol:
    name: str
    is_function: bool
    version: Optional[str]  # None if the base library has no version for it.


def read_soname(elf: ELFFile) -> Optional[str]:
    dynamic = elf.get_section_by_name(".dynamic")
    if dynamic is None:
        return None
    for tag in dynamic.iter_tags():
        if tag.entry.d_tag == "DT_SONAME":
            return tag.soname
    return None


def read_version_definitions(elf: ELFFile) -> dict[int, str]:
    """Maps a `.gnu.version` index to its version-definition name, from
    `.gnu.version_d`. Index 0/1 (local/global, no version) are never
    present in this map; callers treat a missing index as "unversioned"."""
    verdef_section = elf.get_section_by_name(".gnu.version_d")
    if verdef_section is None:
        return {}

    names: dict[int, str] = {}
    for verdef, verdef_aux_iter in verdef_section.iter_versions():
        aux_entries = list(verdef_aux_iter)
        if not aux_entries:
            continue
        # The first aux entry of a verdef is that definition's own name.
        names[verdef["vd_ndx"]] = aux_entries[0].name
    return names


def collect_stub_symbols(elf: ELFFile, verbose: bool) -> list[StubSymbol]:
    dynsym = elf.get_section_by_name(".dynsym")
    if dynsym is None:
        sys.exit("error: input has no .dynsym; nothing to stub")

    version_names = read_version_definitions(elf)
    versym_section = elf.get_section_by_name(".gnu.version")
    versym_entries = list(versym_section.iter_symbols()) if versym_section is not None else None

    stub_symbols: list[StubSymbol] = []
    seen_names: set[str] = set()
    skipped_tls = 0
    skipped_other = 0

    for index, sym in enumerate(dynsym.iter_symbols()):
        entry = sym.entry
        sym_type = entry["st_info"]["type"]
        sym_bind = entry["st_info"]["bind"]

        if entry["st_shndx"] in ("SHN_UNDEF", "SHN_ABS", "SHN_COMMON") or not sym.name:
            continue
        if sym_bind not in ("STB_GLOBAL", "STB_WEAK"):
            continue
        if sym_type == "STT_TLS":
            skipped_tls += 1
            continue
        if sym_type in _FUNCTION_TYPES:
            is_function = True
        elif sym_type in _OBJECT_TYPES:
            is_function = False
        else:
            skipped_other += 1
            continue

        version: Optional[str] = None
        if versym_entries is not None and index < len(versym_entries):
            versym_index = versym_entries[index].entry["ndx"]
            if isinstance(versym_index, int):
                if versym_index & 0x8000:
                    # High bit set: a non-default (@, not @@) compat
                    # version of this name. Only the default version is
                    # reproduced; see docs/stub_so_generator.md.
                    continue
                version = version_names.get(versym_index)

        if sym.name in seen_names:
            continue
        seen_names.add(sym.name)

        stub_symbols.append(StubSymbol(name=sym.name, is_function=is_function, version=version))

    if verbose:
        print(f"verbose: kept {len(stub_symbols)} symbols; skipped {skipped_tls} TLS, "
              f"{skipped_other} other-typed", file=sys.stderr)

    return sorted(stub_symbols, key=lambda s: s.name)


def generate_stub_source(symbols: list[StubSymbol]) -> tuple[str, dict[str, list[str]]]:
    """Returns the generated C source plus, for each distinct version
    string seen, the internal alias names that need hiding via a
    `local:` version-script entry (see generate_version_script())."""
    lines: list[str] = []
    lines.append("/* GENERATED FILE -- produced by scripts/elf_to_stub_so.py. */")
    lines.append("/* Link-only stub: no function/object here does anything real. */")
    lines.append("")

    internal_names_by_version: dict[str, list[str]] = {}

    for i, sym in enumerate(symbols):
        if sym.version is None:
            if sym.is_function:
                lines.append(f"void {sym.name}(void) {{}}")
            else:
                lines.append(f"char {sym.name}[1];")
            continue

        # A plain-named, non-static definition aliased to the real
        # exported name via `.symver`: the version node must still be
        # declared in a version script for the linker to accept the
        # `@@VERSION` suffix, but the script only needs to `local:`-hide
        # this internal alias -- `.symver` does the actual name/version
        # assignment, not the script. A `static` definition does not
        # work here: this toolchain only promotes a `.symver`-aliased
        # symbol to a versioned, exported one when the aliased
        # definition itself has global (non-static) linkage.
        internal_name = f"__stub_{i}_{sym.name}"
        if sym.is_function:
            lines.append(f"void {internal_name}(void) {{}}")
        else:
            lines.append(f"char {internal_name}[1];")
        lines.append(f'__asm__(".symver {internal_name},{sym.name}@@{sym.version}");')
        internal_names_by_version.setdefault(sym.version, []).append(internal_name)

    lines.append("")
    return "\n".join(lines), internal_names_by_version


def generate_version_script(internal_names_by_version: dict[str, list[str]]) -> Optional[str]:
    """A minimal version script declaring one node per distinct version
    string, each only `local:`-hiding the internal `.symver` alias names
    for that version -- the real name/version assignment lives entirely
    in the `.symver` directives in the generated C source. Version-node
    dependency edges are not reproduced; see docs/stub_so_generator.md."""
    if not internal_names_by_version:
        return None

    lines: list[str] = []
    for version in sorted(internal_names_by_version):
        names = "; ".join(internal_names_by_version[version])
        lines.append(f"{version} {{")
        lines.append(f"  local: {names};")
        lines.append("};")
    lines.append("")
    return "\n".join(lines)


def verify_stub(stub_path: Path, expected: list[StubSymbol]) -> None:
    expected_set = {(s.name, s.version) for s in expected}

    with open(stub_path, "rb") as f:
        elf = ELFFile(f)
        actual_symbols = collect_stub_symbols(elf, verbose=False)
    actual_set = {(s.name, s.version) for s in actual_symbols}

    missing = expected_set - actual_set
    extra = actual_set - expected_set
    if missing or extra:
        if missing:
            print(f"error: stub is missing symbols: {sorted(missing)}", file=sys.stderr)
        if extra:
            print(f"error: stub has unexpected extra symbols: {sorted(extra)}", file=sys.stderr)
        sys.exit("error: --verify found a mismatch between the stub and the base library")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build a tiny SDK stub .so from a base shared object's dynamic symbol table.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("input", help="Path to the base shared object (.so) to derive the stub from")
    parser.add_argument("-o", "--output", required=True, help="Path to write the generated stub .so to")
    parser.add_argument("--cc", required=True,
                         help="Cross compiler driver to invoke to build the stub (no default)")
    parser.add_argument("--soname", default=None,
                         help="Override the stub's SONAME (default: the base library's own "
                              "DT_SONAME, or its filename if it has none)")
    parser.add_argument("--cflag", action="append", default=[],
                         help="Extra flag (repeatable) appended verbatim to the compiler "
                              "invocation, e.g. --cflag=--target=aarch64-linux-gnu")
    parser.add_argument("--keep-intermediate", metavar="DIR", default=None,
                         help="Write the generated stub.c (and .o, if separable) into DIR "
                              "instead of a throwaway temp directory")
    parser.add_argument("--verify", action="store_true",
                         help="After building, diff the stub's exported/versioned symbol set "
                              "against the base library's and fail on any mismatch")
    parser.add_argument("--verbose", action="store_true", help="Print symbol selection statistics")
    args = parser.parse_args()

    input_path = Path(args.input)
    with open(input_path, "rb") as f:
        elf = ELFFile(f)
        soname = args.soname or read_soname(elf) or input_path.name
        symbols = collect_stub_symbols(elf, args.verbose)

    if not symbols:
        print("warning: no symbols survived filtering; stub will export nothing", file=sys.stderr)

    stub_source, internal_names_by_version = generate_stub_source(symbols)
    version_script = generate_version_script(internal_names_by_version)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    keep_dir = Path(args.keep_intermediate) if args.keep_intermediate else None
    if keep_dir is not None:
        keep_dir.mkdir(parents=True, exist_ok=True)
        work_dir_ctx = None
        work_dir = keep_dir
    else:
        work_dir_ctx = tempfile.TemporaryDirectory(prefix="elf_to_stub_so_")
        work_dir = Path(work_dir_ctx.name)

    try:
        stub_c_path = work_dir / "stub.c"
        stub_c_path.write_text(stub_source, encoding="utf-8")

        cmd = [
            args.cc,
            "-shared", "-fPIC", "-nostdlib",
            f"-Wl,--soname,{soname}",
        ]
        if version_script is not None:
            stub_map_path = work_dir / "stub.map"
            stub_map_path.write_text(version_script, encoding="utf-8")
            cmd.append(f"-Wl,--version-script,{stub_map_path}")
        cmd += [
            *args.cflag,
            "-o", str(output_path),
            str(stub_c_path),
        ]
        if args.verbose:
            print(f"verbose: running: {' '.join(cmd)}", file=sys.stderr)
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
            sys.exit(f"error: {args.cc} failed with exit code {result.returncode}")
    finally:
        if work_dir_ctx is not None:
            work_dir_ctx.cleanup()

    if args.verify:
        verify_stub(output_path, symbols)

    print(f"wrote {output_path} ({len(symbols)} symbols, soname={soname})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
