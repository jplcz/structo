#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause
"""Builds a compressed `DSYM` debug symbol table blob
(`docs/debug_symtab_format.md`) from an ELF's `.symtab`, for
`structo::debug_symtab_view` (`include/structo/debug_symtab.hpp`) to
decode at runtime when resolving addresses a size-optimized release
`strip` already removed from the shipped binary.

By default every address-bearing symbol is kept -- including
`STB_LOCAL` statics and `STV_HIDDEN`/`STV_INTERNAL` symbols, since those
are exactly what a release strip removes first and recovering them is
this format's whole point -- except addresses already exported via
`.dynsym` (already resolvable at runtime through the ordinary
dynamic-linker path, so keeping them here would just waste space); see
`--drop-local`, `--drop-hidden-visibility`, and `--keep-dynamic` to
change these defaults, and `--exclude-regex`/`--exclude-list` for
additional, deliberate redaction.

Requires pyelftools (`pip install pyelftools`).

Example:
    ./scripts/elf_symtab_to_blob.py build/kernel.elf -o build/kernel.dsym
    ./scripts/elf_symtab_to_blob.py build/kernel.elf \\
        --format c-array --array-name g_kernel_debug_symtab \\
        --namespace my_kernel::debug \\
        -o build/kernel_debug_symtab.hpp
"""

from __future__ import annotations

import argparse
import fnmatch
import re
import sys
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

try:
    from elftools.elf.elffile import ELFFile
except ImportError:  # pragma: no cover - environment-dependent
    sys.exit("error: pyelftools is required (pip install pyelftools)")

MAGIC = 0x31595344  # "DSY1", little-endian uint32
HEADER_SIZE = 48
MAX_NAME_LEN_LIMIT = 127
DEFAULT_MAX_NAME_LEN = 31
DEFAULT_GROUP_SIZE = 16
DEFAULT_TRUNCATION_MARKER = ord("~")

# Symbol types/visibilities that are never address-resolution-relevant,
# regardless of any other flag.
_NEVER_KEPT_TYPES = {"STT_NOTYPE", "STT_FILE", "STT_SECTION"}


@dataclass
class Symbol:
    name: str
    address: int


def encode_uleb128(value: int) -> bytes:
    """Encodes `value` (>= 0) as an unsigned LEB128 varint."""
    if value < 0:
        raise ValueError("encode_uleb128: value must be non-negative")
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            out.append(byte | 0x80)
        else:
            out.append(byte)
            return bytes(out)


def read_build_id(elf: "ELFFile") -> bytes:
    """Reads `.note.gnu.build-id`'s payload, or `b""` if absent."""
    section = elf.get_section_by_name(".note.gnu.build-id")
    if section is None:
        return b""
    for note in section.iter_notes():
        if note["n_type"] == "NT_GNU_BUILD_ID" or note["n_name"] == "GNU":
            return bytes.fromhex(note["n_desc"]) if isinstance(note["n_desc"], str) else bytes(note["n_desc"])
    return b""


def read_dynsym_addresses(elf: "ELFFile") -> set[int]:
    """Collects every defined `.dynsym` symbol's address."""
    section = elf.get_section_by_name(".dynsym")
    addrs: set[int] = set()
    if section is None:
        return addrs
    for sym in section.iter_symbols():
        if sym["st_shndx"] == "SHN_UNDEF" or sym["st_value"] == 0:
            continue
        addrs.add(sym["st_value"])
    return addrs


def load_exclude_list(path: str) -> list[str]:
    patterns = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                patterns.append(line)
    return patterns


def name_excluded(name: str, regexes: list["re.Pattern[str]"], list_patterns: list[str]) -> bool:
    for rx in regexes:
        if rx.search(name):
            return True
    for pattern in list_patterns:
        if pattern.startswith("glob:"):
            if fnmatch.fnmatch(name, pattern[len("glob:"):]):
                return True
        elif pattern == name:
            return True
    return False


def collect_symbols(args: argparse.Namespace, elf: "ELFFile", verbose: bool) -> list[Symbol]:
    symtab = elf.get_section_by_name(".symtab")
    using_dynsym_fallback = False
    if symtab is None:
        symtab = elf.get_section_by_name(".dynsym")
        using_dynsym_fallback = True
        if symtab is None:
            sys.exit("error: input ELF has neither .symtab nor .dynsym")
        print("warning: .symtab not present (binary already stripped); falling back to .dynsym "
              "-- far fewer symbols, but still useful", file=sys.stderr)

    dynsym_addrs: set[int] = set() if using_dynsym_fallback or args.keep_dynamic else read_dynsym_addresses(elf)

    exclude_regexes = [re.compile(p) for p in args.exclude_regex]
    exclude_list_patterns = load_exclude_list(args.exclude_list) if args.exclude_list else []

    kept_types = {"STT_FUNC"}
    if args.include_objects:
        kept_types.add("STT_OBJECT")

    hidden_visibilities = {"STV_HIDDEN", "STV_INTERNAL"}

    dropped_dynamic = 0
    dropped_local = 0
    dropped_hidden = 0
    dropped_excluded = 0
    seen_addresses: dict[int, Symbol] = {}
    merged = 0

    for sym in symtab.iter_symbols():
        entry = sym.entry
        st_info = entry["st_info"]
        sym_type = st_info["type"]
        sym_bind = st_info["bind"]
        visibility = entry["st_other"]["visibility"]

        if sym_type in _NEVER_KEPT_TYPES or sym_type not in kept_types:
            continue
        if entry["st_shndx"] == "SHN_UNDEF" or entry["st_value"] == 0:
            continue
        if not sym.name:
            continue

        if args.drop_local and sym_bind == "STB_LOCAL":
            dropped_local += 1
            continue
        if args.drop_hidden_visibility and visibility in hidden_visibilities:
            dropped_hidden += 1
            continue
        if name_excluded(sym.name, exclude_regexes, exclude_list_patterns):
            dropped_excluded += 1
            continue

        address = entry["st_value"]
        if address in dynsym_addrs:
            dropped_dynamic += 1
            continue

        if address in seen_addresses:
            merged += 1
            continue
        seen_addresses[address] = Symbol(name=sym.name, address=address)

    if verbose:
        print(f"verbose: kept {len(seen_addresses)} symbols; dropped {dropped_local} local, "
              f"{dropped_hidden} hidden-visibility, {dropped_dynamic} already-exported-dynamic, "
              f"{dropped_excluded} excluded, {merged} duplicate-address aliases", file=sys.stderr)

    return sorted(seen_addresses.values(), key=lambda s: s.address)


def truncate_name(name: str, max_name_len: int, truncation_marker: int) -> bytes:
    raw = name.encode("utf-8", errors="replace")
    if len(raw) <= max_name_len:
        return raw
    truncated = bytearray(raw[:max_name_len])
    truncated[-1] = truncation_marker
    return bytes(truncated)


@dataclass
class EncodedBlob:
    checkpoint_table: bytes
    entry_stream: bytes
    checkpoint_count: int


def encode_entries(symbols: list[Symbol], addr_width: int, group_size: int, max_name_len: int,
                    truncation_marker: int) -> EncodedBlob:
    checkpoint_records = bytearray()
    entry_stream = bytearray()
    checkpoint_count = 0

    for group_start in range(0, len(symbols), group_size):
        group = symbols[group_start:group_start + group_size]
        checkpoint_records += group[0].address.to_bytes(addr_width, "little")
        checkpoint_records += len(entry_stream).to_bytes(4, "little")
        checkpoint_count += 1

        previous_name: Optional[bytes] = None
        previous_addr = group[0].address
        for i, sym in enumerate(group):
            name_bytes = truncate_name(sym.name, max_name_len, truncation_marker)
            if i > 0:
                delta = sym.address - previous_addr
                entry_stream += encode_uleb128(delta)
                previous_addr = sym.address

            if previous_name is not None and name_bytes == previous_name and i > 0:
                entry_stream.append(0x80)  # repeat_previous flag, length bits unused.
            else:
                entry_stream.append(len(name_bytes) & 0x7F)
                entry_stream += name_bytes
            previous_name = name_bytes

    return EncodedBlob(checkpoint_table=bytes(checkpoint_records), entry_stream=bytes(entry_stream),
                        checkpoint_count=checkpoint_count)


def build_blob(args: argparse.Namespace, symbols: list[Symbol], addr_width: int, build_id: bytes) -> bytes:
    encoded = encode_entries(symbols, addr_width, args.group_size, args.max_name_len, args.truncation_marker)

    checkpoint_table_offset = HEADER_SIZE
    build_id_offset = checkpoint_table_offset + len(encoded.checkpoint_table)
    entry_stream_offset = build_id_offset + len(build_id)

    header = bytearray(HEADER_SIZE)
    header[0:4] = MAGIC.to_bytes(4, "little")
    header[4] = addr_width
    header[5] = args.max_name_len
    header[6] = args.truncation_marker
    header[7] = 0  # reserved0
    header[8:10] = args.group_size.to_bytes(2, "little")
    header[10:12] = (0).to_bytes(2, "little")  # reserved1
    header[12:16] = len(symbols).to_bytes(4, "little")
    header[16:20] = encoded.checkpoint_count.to_bytes(4, "little")
    header[20:24] = checkpoint_table_offset.to_bytes(4, "little")
    header[24:28] = entry_stream_offset.to_bytes(4, "little")
    header[28:32] = len(encoded.entry_stream).to_bytes(4, "little")
    header[32:36] = build_id_offset.to_bytes(4, "little")
    header[36] = len(build_id)
    header[37:40] = b"\x00\x00\x00"  # reserved2

    payload = bytes(encoded.checkpoint_table) + build_id + encoded.entry_stream
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header[40:44] = crc.to_bytes(4, "little")
    header[44:48] = (0).to_bytes(4, "little")  # reserved3

    return bytes(header) + payload


def format_c_array(blob: bytes, args: argparse.Namespace) -> str:
    namespaces = [n for n in args.namespace.split("::") if n] if args.namespace else []
    lines: list[str] = []
    lines.append("// clang-format off")
    lines.append("//")
    lines.append("// GENERATED FILE -- produced by scripts/elf_symtab_to_blob.py.")
    lines.append("// Do not hand-edit; re-run the generator against the source ELF instead.")
    lines.append("//")
    lines.append("// clang-format on")
    lines.append("")
    lines.append("#pragma once")
    lines.append("")
    lines.append(f"/** @file {Path(args.output).name}")
    lines.append(" * @brief Generated `DSYM` debug symbol table blob (see")
    lines.append(" * `structo::debug_symtab_view` in `structo/debug_symtab.hpp`).")
    lines.append(" * Produced by `scripts/elf_symtab_to_blob.py`; see that script's")
    lines.append(" * `--help` to regenerate this file from the source ELF.")
    lines.append(" */")
    lines.append("")
    lines.append("#include <cstddef>")
    lines.append("")
    for ns in namespaces:
        lines.append(f"namespace {ns} {{")
    if namespaces:
        lines.append("")

    lines.append(f"inline constexpr std::byte {args.array_name}[{len(blob)}] = {{")
    bytes_per_line = 16
    for i in range(0, len(blob), bytes_per_line):
        chunk = blob[i:i + bytes_per_line]
        values = ", ".join(f"std::byte{{0x{b:02X}}}" for b in chunk)
        lines.append(f"    {values},")
    lines.append("};")

    if namespaces:
        lines.append("")
        for ns in reversed(namespaces):
            lines.append(f"}} // namespace {ns}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build a compressed DSYM debug symbol table blob from an ELF's .symtab.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("input", help="Path to the source ELF (pre-strip image carrying .symtab)")
    parser.add_argument("-o", "--output", required=True, help="Path to write the generated blob/header to")
    parser.add_argument("--max-name-len", type=int, default=DEFAULT_MAX_NAME_LEN,
                         help=f"Per-symbol name truncation cap in bytes, 1-{MAX_NAME_LEN_LIMIT}; "
                              f"default {DEFAULT_MAX_NAME_LEN}")
    parser.add_argument("--group-size", type=int, default=DEFAULT_GROUP_SIZE,
                         help=f"Entries per checkpoint group; default {DEFAULT_GROUP_SIZE}")
    parser.add_argument("--truncation-marker", default="~",
                         help="Single byte appended in place of a truncated name's last "
                              "character; default '~'")
    parser.add_argument("--include-objects", action="store_true",
                         help="Also keep STT_OBJECT symbols (default: STT_FUNC only)")
    parser.add_argument("--drop-local", action="store_true",
                         help="Drop STB_LOCAL symbols (default: kept, since these are exactly "
                              "what a release strip removes first)")
    parser.add_argument("--drop-hidden-visibility", action="store_true",
                         help="Drop STV_HIDDEN/STV_INTERNAL symbols (default: kept)")
    parser.add_argument("--keep-dynamic", action="store_true",
                         help="Keep symbols whose address is already exported via .dynsym "
                              "(default: dropped, since those resolve via the dynamic linker "
                              "already)")
    parser.add_argument("--exclude-regex", action="append", default=[],
                         help="Regex (repeatable) matching symbol names to drop")
    parser.add_argument("--exclude-list", default=None,
                         help="File with one exact name or 'glob:'-prefixed pattern per line to drop")
    parser.add_argument("--format", choices=["bin", "c-array"], default="bin",
                         help="Output format; default bin")
    parser.add_argument("--array-name", default="g_debug_symtab_blob",
                         help="Array name for --format c-array; default g_debug_symtab_blob")
    parser.add_argument("--namespace", default="",
                         help="'::'-separated namespace to wrap the array in for --format c-array")
    parser.add_argument("--verbose", action="store_true", help="Print filtering/dedup statistics")
    args = parser.parse_args()

    if not (1 <= args.max_name_len <= MAX_NAME_LEN_LIMIT):
        parser.error(f"--max-name-len must be between 1 and {MAX_NAME_LEN_LIMIT}")
    if not (1 <= args.group_size <= 0xFFFF):
        parser.error("--group-size must be between 1 and 65535")
    marker_bytes = args.truncation_marker.encode("utf-8")
    if len(marker_bytes) != 1:
        parser.error("--truncation-marker must be exactly one byte")
    args.truncation_marker = marker_bytes[0]

    with open(args.input, "rb") as f:
        elf = ELFFile(f)
        addr_width = 8 if elf.elfclass == 64 else 4
        build_id = read_build_id(elf)
        symbols = collect_symbols(args, elf, args.verbose)

    if not symbols:
        print("warning: no symbols survived filtering; blob will be empty", file=sys.stderr)

    blob = build_blob(args, symbols, addr_width, build_id)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if args.format == "c-array":
        output_path.write_text(format_c_array(blob, args), encoding="utf-8")
    else:
        output_path.write_bytes(blob)

    print(f"wrote {output_path} ({len(symbols)} symbols, {len(blob)} bytes, addr_width={addr_width})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
