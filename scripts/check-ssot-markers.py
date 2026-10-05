#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

"""
Cross-file "single source of truth" (SSOT) marker consistency checker.

Some values legitimately cannot have a single source of truth: e.g. an ABI
version baked into both a C header `#define` and a packaging `.spec` file,
or a path/limit duplicated between a shell script and a header because the
two languages cannot share one literal definition. This script lets such
duplicated values be marked explicitly so that drift between them is
caught automatically instead of silently rotting.

Marker format
-------------
Put a comment anywhere (leading, trailing, same line as the value, or on a
line of its own) containing:

    ssot: <KEY> [window=<N>] [regex="<PATTERN>"]

`<KEY>` identifies the group of occurrences that must all agree on the
same value (alnum, `_`, `.`, `-`). The actual *value* is not written in the
marker itself -- it lives in the surrounding code, because that is where
it is authoritative. Starting at the marker's own line (the portion before
the marker comment, so trailing markers work) and continuing for the next
`window` lines (default 3), the checker looks for the first value it can
extract and associates it with `<KEY>`.

Examples:

    // ssot: ABI_VERSION
    #define JPLCZ_STRUCTO_ABI_VERSION 7

    Version: 1.4.2  # ssot: PACKAGE_VERSION

    PACKAGE_VERSION="1.4.2"  # ssot: PACKAGE_VERSION

By default the value is extracted with a small cascade of generic patterns
(quoted string, dotted version number, hex literal, plain integer) tried
line-by-line within the window, first match wins. For anything that does
not fit, give an explicit capture regex:

    // ssot: QUEUE_DEPTH regex="depth\\s*=\\s*(\\d+)"
    static constexpr std::size_t kQueueDepth = 64;  // depth = 64 (doc copy)

Every occurrence of the same `<KEY>` across the scanned files must extract
to the same value (after trimming whitespace); mismatches, as well as
markers whose window did not contain an extractable value, are reported as
errors. A `<KEY>` that only ever appears once is reported as a warning (an
SSOT marker only makes sense when comparing two or more locations) but
does not fail the run by default.

This script is deliberately self-contained and dependency-free (standard
library only) so it can be dropped as-is into sibling projects, same as
`check-no-std-types.py`.
"""

import argparse
import re
import sys
from pathlib import Path
from typing import Dict, List, NamedTuple, Optional, Tuple

COLOR_RED = "\033[31;1m"
COLOR_YELLOW = "\033[33;1m"
COLOR_GREEN = "\033[32m"
COLOR_RESET = "\033[0m"

DEFAULT_EXTENSIONS = ["h", "hpp", "ipp", "c", "cc", "cpp", "sh", "spec", "py", "cmake"]
DEFAULT_FILENAMES = ["CMakeLists.txt"]
DEFAULT_EXCLUDE_DIRS = {".git", "build", "build-matrix", "__pycache__", ".venv", ".cache"}

MARKER_PATTERN = re.compile(
    r"ssot:\s*(?P<key>[A-Za-z0-9_.\-]+)(?P<opts>.*)$"
)
OPT_WINDOW = re.compile(r"\bwindow=(\d+)\b")
OPT_REGEX = re.compile(r'\bregex="([^"]*)"')

# Generic fallback extraction cascade, tried in order against each window
# line's raw text until one matches. Group 1 is used when present.
DEFAULT_VALUE_PATTERNS = [
    re.compile(r'"([^"]*)"'),
    re.compile(r"'([^']*)'"),
    re.compile(r"\b(\d+(?:\.\d+){1,3})\b"),
    re.compile(r"\b(0[xX][0-9A-Fa-f]+)\b"),
    re.compile(r"(-?\b\d+\b)"),
]


class Occurrence(NamedTuple):
    key: str
    path: Path
    line: int
    value: Optional[str]
    raw: str


def parse_opts(opts: str) -> Tuple[int, Optional[re.Pattern]]:
    window_match = OPT_WINDOW.search(opts)
    window = int(window_match.group(1)) if window_match else 3

    regex_match = OPT_REGEX.search(opts)
    pattern = re.compile(regex_match.group(1)) if regex_match else None

    return window, pattern


def extract_value(window_lines: List[str], pattern: Optional[re.Pattern]) -> Optional[str]:
    patterns = [pattern] if pattern is not None else DEFAULT_VALUE_PATTERNS
    for line in window_lines:
        for candidate in patterns:
            m = candidate.search(line)
            if m is None:
                continue
            return m.group(1) if candidate.groups else m.group(0)
    return None


def scan_file(path: Path) -> List[Occurrence]:
    try:
        lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
    except OSError:
        return []

    occurrences: List[Occurrence] = []
    for i, line in enumerate(lines):
        m = MARKER_PATTERN.search(line)
        if m is None:
            continue

        key = m.group("key")
        window, pattern = parse_opts(m.group("opts"))

        before_marker = line[: m.start()]
        following = lines[i + 1 : i + 1 + window]
        window_lines = [before_marker] + following

        value = extract_value(window_lines, pattern)
        occurrences.append(Occurrence(key=key, path=path, line=i + 1, value=value, raw=line.strip()))

    return occurrences


def collect_files(root: Path, extensions: List[str], filenames: List[str], exclude_dirs: set) -> List[Path]:
    files: List[Path] = []
    ext_set = {e.lstrip(".") for e in extensions}
    name_set = set(filenames)
    self_path = Path(__file__).resolve()

    for candidate in root.rglob("*"):
        if not candidate.is_file():
            continue
        if candidate.resolve() == self_path:
            # Skip this checker's own source file: its docstring documents
            # the marker syntax using literal examples that would otherwise
            # be misdetected as real markers to cross-check.
            continue
        if any(part in exclude_dirs for part in candidate.relative_to(root).parts[:-1]):
            continue
        if candidate.name in name_set or candidate.suffix.lstrip(".") in ext_set:
            files.append(candidate)

    return sorted(set(files))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=".", help="Root directory to scan recursively (default: .)")
    parser.add_argument(
        "--extension", action="append", dest="extensions", default=None,
        help=f"File extension (without dot) to scan; may be repeated (default: {', '.join(DEFAULT_EXTENSIONS)})"
    )
    parser.add_argument(
        "--filename", action="append", dest="filenames", default=None,
        help=f"Exact filename (extensionless or otherwise) to scan; may be repeated "
             f"(default: {', '.join(DEFAULT_FILENAMES)})"
    )
    parser.add_argument(
        "--exclude-dir", action="append", dest="exclude_dirs", default=None,
        help=f"Directory name to prune while walking; may be repeated (default: {', '.join(sorted(DEFAULT_EXCLUDE_DIRS))})"
    )
    parser.add_argument(
        "--fail-on-single", action="store_true",
        help="Also fail (not just warn) when a key appears only once across all scanned files"
    )
    args = parser.parse_args()

    root = Path(args.root)
    if not root.is_dir():
        print(f"{COLOR_RED}[ERROR] Root directory not found: {root}{COLOR_RESET}", file=sys.stderr)
        return 1

    extensions = args.extensions or DEFAULT_EXTENSIONS
    filenames = args.filenames or DEFAULT_FILENAMES
    exclude_dirs = set(args.exclude_dirs) if args.exclude_dirs else DEFAULT_EXCLUDE_DIRS

    files = collect_files(root, extensions, filenames, exclude_dirs)
    if not files:
        print(f"{COLOR_YELLOW}[WARN] No files found under {root}{COLOR_RESET}")
        return 0

    by_key: Dict[str, List[Occurrence]] = {}
    for f in files:
        for occ in scan_file(f):
            by_key.setdefault(occ.key, []).append(occ)

    print(f"Scanned {len(files)} file(s) under {root}/ for `ssot:` markers.\n")

    if not by_key:
        print(f"{COLOR_GREEN}[SUCCESS] No `ssot:` markers found.{COLOR_RESET}")
        return 0

    errors = 0
    warnings = 0

    for key in sorted(by_key):
        occurrences = by_key[key]

        missing = [o for o in occurrences if o.value is None]
        for o in missing:
            print(f"{COLOR_RED}{o.path}:{o.line}:{COLOR_RESET} key `{key}` -- could not extract a value "
                  f"within its window  ({o.raw})")
            errors += 1

        present = [o for o in occurrences if o.value is not None]
        distinct_values = sorted({o.value for o in present})

        if len(present) == 1 and not missing:
            print(f"{COLOR_YELLOW}[WARN]{COLOR_RESET} key `{key}` only appears once "
                  f"({present[0].path}:{present[0].line} = `{present[0].value}`); nothing to cross-check it against.")
            warnings += 1
            if args.fail_on_single:
                errors += 1
            continue

        if len(distinct_values) > 1:
            print(f"{COLOR_RED}[ERROR]{COLOR_RESET} key `{key}` has inconsistent values:")
            for o in present:
                print(f"  {o.path}:{o.line}: `{o.value}`  ({o.raw})")
            errors += 1
        elif present:
            print(f"{COLOR_GREEN}[OK]{COLOR_RESET} key `{key}` consistent (`{distinct_values[0]}`) "
                  f"across {len(present)} occurrence(s).")

    print()
    if errors:
        print(f"{COLOR_RED}[ERROR] {errors} SSOT marker issue(s) found"
              f"{f', {warnings} warning(s)' if warnings and not args.fail_on_single else ''}.{COLOR_RESET}")
        return 1

    if warnings:
        print(f"{COLOR_YELLOW}[SUCCESS with warnings] All cross-checked SSOT markers are consistent "
              f"({warnings} single-occurrence warning(s)).{COLOR_RESET}")
        return 0

    print(f"{COLOR_GREEN}[SUCCESS] All SSOT markers are consistent.{COLOR_RESET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
