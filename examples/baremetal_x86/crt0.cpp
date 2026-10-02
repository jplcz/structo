// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

/** @file crt0.cpp
 * @brief The tiny slice of a hosted `crt1.o`/`__libc_csu_init` this
 * freestanding, `-nostdlib` demo still needs: walks the linker-provided
 * `.init_array` section (see `linker.ld`) and calls each entry in link
 * order, exactly as a hosted C runtime would before `main()`. Without
 * this, every translation unit's global/static objects with
 * non-trivial constructors (see `monotonic_allocator.cpp`'s own heap
 * context, or `kmain.cpp`'s demo "boot proof" object) would simply
 * never run.
 *
 * Called once from `boot.s`, before `kmain`. No `.fini_array` handling
 * exists (or is needed): this kernel never exits, so global destructors
 * would never run anyway (`linker.ld` discards `.fini_array` outright).
 */

extern "C" {
using init_fn = void (*)();
// Provided by the linker (see `linker.ld`'s `.init_array` section), not
// defined anywhere -- their *addresses* (not any value stored at them)
// bound the array of function pointers placed there by the compiler.
extern init_fn __init_array_start[];
extern init_fn __init_array_end[];
}

extern "C" void structo_run_global_constructors() noexcept {
  for (init_fn *fn = __init_array_start; fn != __init_array_end; ++fn)
    (*fn)();
}
