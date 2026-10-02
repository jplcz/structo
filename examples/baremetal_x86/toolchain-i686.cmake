# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause
#
# Cross-compilation toolchain file for the i686 (x86, 32-bit protected
# mode) target this demo's kernel image is built for -- entirely
# independent of whatever compiler configured the main `structo` build.
#
# Usage (from this directory):
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-i686.cmake
#   cmake --build build
#
# Requires the `i686-linux-gnu-gcc`/`i686-linux-gnu-g++` cross-compiler
# package (e.g. Ubuntu/Debian's `g++-i686-linux-gnu`). `CMAKE_SYSTEM_NAME
# Generic` tells CMake this is not a normal hosted target -- it must not
# assume a working hosted C/C++ runtime is linkable by default (this
# demo links with `-nostdlib` and its own `linker.ld` instead).

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER i686-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER i686-linux-gnu-g++)
set(CMAKE_ASM_COMPILER i686-linux-gnu-gcc)

# The cross-compiler can still link ordinary hosted executables (it is a
# full i686-linux-gnu toolchain, not a bare -elf one) -- but this
# project's own targets never do (`-nostdlib` plus a custom linker
# script), so skip CMake's default "can the compiler link an executable"
# sanity check in favor of a plain static-library compile check, which
# exercises the same compiler/flags without requiring a full hosted link.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
