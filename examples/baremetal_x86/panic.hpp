// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file panic.hpp
 * @brief Declares `baremetal_panic`, this demo's `RELOCO_KERNEL_PANIC`
 * target (see `CMakeLists.txt`'s `-D` flags). Force-included ahead of
 * every other header in every translation unit (`-include panic.hpp`),
 * so the declaration is visible before `reloco`/`structo` headers --
 * which expand `RELOCO_ASSERT`'s failure path directly to
 * `RELOCO_KERNEL_PANIC(...)` -- are ever parsed.
 */

extern "C" [[noreturn]] void baremetal_panic(const char *expression, const char *file, int line,
                                             const char *message) noexcept;

#define RELOCO_KERNEL_PANIC(expr, file, line, msg) ::baremetal_panic((expr), (file), (line), (msg))
