# GDB pretty-printer testbed

This directory is a self-contained testbed for developing and validating
structo's GDB pretty printers (`tools/gdb/structo_printers.py` /
`include/structo/sync/gdb_printers.hpp`). It is **not** part of the main
CMake build, the test suite, or CI -- build and run it explicitly, as
described below, whenever you add or change a printer.

This mirrors reloco's own `tools/gdb/testbed/` layout and conventions
exactly (see that project's `docs/gdb-pretty-printers.md`); only the
printed types differ.

## Layout

- `testbed.cpp` -- one `main()` with a small, recognizable instance of every
  `structo::sync` type that has a printer, plus a `// GDB_CHECK: <name> =>
  <substring>` comment next to each one and a `// GDB_BREAK` marker on the
  line to stop at.
- `testbed_embedded.cpp` -- the exact same program, but `#include
  <structo/sync/gdb_printers.hpp>` first, so the printers are embedded via
  `.debug_gdb_scripts` instead of loaded separately.
- `run.sh` -- builds both binaries and runs the checks under GDB in all
  three delivery modes (see below), reporting PASS/FAIL per case.

## Running it

```
tools/gdb/testbed/run.sh
```

Requires a C++17 compiler (`$CXX`, default `g++`) and `gdb` (`$GDB`) on
`PATH`, plus `reloco`'s and `microfmt`'s `include/` directories -- by
default assumed to be checked out as siblings of this repository
(`../reloco/include`, `../microfmt/include`); override with
`RELOCO_INCLUDE_DIR`/`MICROFMT_INCLUDE_DIR` if not. Add `--keep-build` to
keep `tools/gdb/testbed/build/` (binaries, generated `<binary>-gdb.py`,
and one transcript per mode) around after a failure for manual inspection;
without it, `build/` is removed on exit (it's already gitignored either
way).

The script builds `testbed.cpp`/`testbed_embedded.cpp`, sets a breakpoint
at the `GDB_BREAK` line, runs to it, and issues one `print <name>` per
`GDB_CHECK` case, in three modes that mirror the three ways
`docs/gdb-pretty-printers.md` documents using the printers:

1. **`source-script`** -- `testbed_plain`, printers loaded with an explicit
   `source tools/gdb/structo_printers.py`.
2. **`autoload-gdb-py`** -- `testbed_plain`, printers loaded automatically
   from a generated `testbed_plain-gdb.py` sitting next to the binary (GDB's
   `<binary>-gdb.py` convention), no `source` command.
3. **`embedded-header`** -- `testbed_embedded`, printers auto-loaded purely
   from the `.debug_gdb_scripts` section embedded by
   `include/structo/sync/gdb_printers.hpp`; no sidecar file, no `source`
   command.

A case passes if its expected substring appears anywhere in that mode's
transcript. This is a substring check, not an exact-output diff, so it's
robust to incidental formatting but can't catch every regression -- treat
it as a smoke test, not a replacement for reading the transcript when
something looks wrong (`--keep-build` + `build/transcript_<mode>.txt`).

Exit code is non-zero if any case fails in any mode.

## Adding a test case for a new/changed printer

1. In `tools/gdb/structo_printers.py`, write or update the printer class and
   its `pp.add_printer(...)` registration.
2. Regenerate the embedded header: `python3 tools/gdb/generate_embedded_header.py`.
3. In `testbed.cpp`, declare one local variable of the new type, initialized
   with distinctive, easy-to-eyeball values (e.g. a lock actually held by a
   recognizable owner, rather than left default-constructed), *before* the
   `GDB_BREAK` marker near the end of `main()` (there's an
   `ADD_NEW_CASE_HERE` comment marking the spot). A fresh `Traits` type
   local to this file is fine -- see `test_lock_traits` and friends.
4. Add a `// GDB_CHECK: <variable_name> => <expected substring>` comment
   directly below the declaration. The expected substring should be
   something only a correct printer would produce (e.g. a value it
   formats), not incidental GDB chrome like `$1 = ` or braces.
5. Run `tools/gdb/testbed/run.sh` and confirm the new case (and all
   existing ones) `PASS` in all three modes.

## Why this exists as a separate testbed instead of a unit test

GDB pretty printers can't be exercised by the normal C++ test suite: they
run inside GDB's embedded Python interpreter against a live/inspected
process, not inside the program itself. This testbed is the practical
substitute -- an actual compiled binary, run under real GDB, checked
against real `print` output -- for both delivery mechanisms at once.
