// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Builds the exact same testbed program, but with `gdb_printers.hpp`
// included so the printers are embedded in `.debug_gdb_scripts` and
// auto-loaded by GDB with no `source` command or `<binary>-gdb.py` file.
// See tools/gdb/testbed/README.md.

#include <structo/sync/gdb_printers.hpp>

#include "testbed.cpp"
