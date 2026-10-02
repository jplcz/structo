// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file monotonic_allocator.hpp
 * @brief Declares nothing of its own -- including this header (rather
 * than just linking `monotonic_allocator.cpp` in) is what guarantees
 * every translation unit sees the same global `operator new`/
 * `operator delete` overloads `monotonic_allocator.cpp` defines, before
 * any of them might use `new`/`delete` (including implicitly, from a
 * global object's own constructor -- see `crt0.cpp`).
 *
 * `reloco`/`structo` containers do **not** use `new`/`delete` by
 * default -- they reach for `reloco::default_allocator()` instead (see
 * `reloco/default_allocator.hpp`, "similar in spirit to Rust's
 * `#[global_allocator]`"), so overriding `operator new` alone would not
 * affect them. `monotonic_allocator.cpp` therefore does both:
 *   - Overrides `reloco::reloco_global_alloc::default_allocator()`
 *     (enabled by this demo's `RELOCO_DEFAULT_ALLOCATOR_CUSTOM` compile
 *     definition, see `CMakeLists.txt`), so any `reloco`/`structo`
 *     container this demo might use is backed by the same arena.
 *   - Overrides plain global `operator new`/`operator delete` (absent
 *     here: this demo links `-nostdlib`, so no libstdc++-provided
 *     default exists), for ordinary `new`-expressions like
 *     `kmain.cpp`'s own demo object.
 *
 * Both are backed by the exact same single arena: a basic *monotonic*
 * (bump-pointer) `reloco::stack_allocator`. "Monotonic" here means
 * exactly what it does for e.g. `std::pmr::monotonic_buffer_resource`:
 * individual blocks are never freed (`operator delete` is a deliberate
 * no-op) -- the only way to reclaim any memory at all is to discard/
 * reset the entire arena at once, which this demo never does. That is
 * more than enough for a short-lived demo kernel that only ever
 * allocates a handful of small, long-lived objects before halting; a
 * real kernel would eventually need something with per-block `free()`
 * (e.g. `structo::buddy_allocator`, or a slab on top of it).
 */

#include <cstddef>

/**
 * @brief Global, freestanding-safe replacements for the usual
 * libstdc++-provided `operator new`/`operator delete` -- see
 * `monotonic_allocator.cpp`.
 *
 * `operator new` never returns `nullptr` and never throws (this demo
 * builds with `-fno-exceptions`): on arena exhaustion it calls
 * `baremetal_panic` instead, exactly like `RELOCO_ASSERT` failures
 * elsewhere in this demo.
 */
void *operator new(std::size_t size);
void *operator new[](std::size_t size);
void operator delete(void *ptr) noexcept;
void operator delete[](void *ptr) noexcept;
void operator delete(void *ptr, std::size_t size) noexcept;
void operator delete[](void *ptr, std::size_t size) noexcept;
