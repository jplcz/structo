<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Vendored FreeBSD libc string routines

This directory vendors a handful of files, verbatim apart from the minimal
portability patches noted below, from FreeBSD's `lib/libc/string/` (not
`sys/libkern/`). They supply `strlen`/`memchr`/`memcmp`/`memset`/`memcpy`/
`memmove` for `baremetal_x86_demo`'s `-nostdlib` link, since libstdc++'s
own `std::char_traits<char>` implementation (pulled in transitively via
`reloco::string_view`/`console_ref`) calls some of these internally. Build
with `-fno-builtin` (see `../CMakeLists.txt`): without it, the compiler
would "recognize" these as the very builtins they define and miscompile
them into infinite recursion.

## Provenance and license per file

| File | Upstream path | License |
|---|---|---|
| `strlen.c` | `lib/libc/string/strlen.c` | BSD-2-Clause (Xin LI) |
| `memchr.c` | `lib/libc/string/memchr.c` | MIT (Rich Felker et al., musl-derived) |
| `memcmp.c` | `lib/libc/string/memcmp.c` | BSD-3-Clause (Regents of the University of California) |
| `memset.c` | `lib/libc/string/memset.c` | BSD-3-Clause (Regents of the University of California) |
| `memcpy.c` | `lib/libc/string/memcpy.c` | BSD-3-Clause (thin `#define MEMCOPY` + `#include "bcopy.c"` wrapper) |
| `memmove.c` | `lib/libc/string/memmove.c` | BSD-3-Clause (thin `#define MEMMOVE` + `#include "bcopy.c"` wrapper) |
| `bcopy.c` | `lib/libc/string/bcopy.c` | BSD-3-Clause (Regents of the University of California; shared implementation for `memcpy`/`memmove`/`bcopy`) |

Each file retains its own original upstream license header; see the top of
each `.c` file for the exact copyright/license text.

## Portability patches applied (Linux/glibc headers vs. FreeBSD headers)

These files were originally written against FreeBSD's own libc headers;
compiling them as-is against Linux/glibc headers needed the following
minimal changes:

- `strlen.c`: `#include <sys/limits.h>` (a FreeBSD-only header, does not
  exist on Linux) replaced with `#include <limits.h>`; added
  `#include <stdint.h>`; added a compatibility shim defining the BSD-only
  `LONG_BIT` macro as `(__CHAR_BIT__ * __SIZEOF_LONG__)` when not already
  defined (Linux's `<limits.h>` doesn't define it; `__SIZEOF_LONG__` is
  used, rather than `sizeof(long)`, since the shim is evaluated in `#if`
  preprocessor context where `sizeof` isn't valid).
- `bcopy.c`: added `#include <stdint.h>` (Linux's `<sys/types.h>` doesn't
  itself pull in `intptr_t`/`uintptr_t` the way FreeBSD's does).

No other logic was changed.
