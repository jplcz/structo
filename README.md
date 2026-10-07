<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# jplcz_structo

<img src="assets/logo/structo-logo-small.png" alt="structo logo" width="200">

`structo` is a header-only C++17 library of OS kernel/hypervisor/trusted-
boundary building blocks: Flattened Device Tree (DTB) decoding and
physical memory region management, built directly on
[`jplcz_reloco`](https://github.com/jplcz/reloco)'s allocation-free,
fallible-by-default primitives (`fdt_reader`/`fdt_index`/`fdt_memory`,
`region_set`/`phys_addr`, `expected`/`result`).

It targets the same class of code as `jplcz_reloco`/`jplcz_microfmt`:
kernel boot paths, hypervisor/VMM code, and any trusted-boundary call site
where allocation-free, exception-free, caller-owned-storage components
matter -- but specialized to the handful of things almost every such boot
path needs from firmware/bootloader-supplied data before an allocator or
MMU exists.

## What's here

See [docs/reference.md](docs/reference.md) for the full per-header
reference (one page per header, linked from there) covering every public
type, [docs/coding-guide.md](docs/coding-guide.md) for contribution
conventions (no `std::` containers in main code/examples, `TEST_F`/
`TEST_P` for new tests, trait-example requirements), and
[docs/gdb-pretty-printers.md](docs/gdb-pretty-printers.md) for GDB pretty
printers covering `structo::sync` (source, auto-load, or embed into the
binary). A few highlights,
demonstrated in [examples/](examples):

- **[`boot_memory_map_demo.cpp`](examples/boot_memory_map_demo.cpp)** --
  decodes a DTB's `/memory`/`/reserved-memory` description into full/free
  `reloco::region_set`s with `structo::boot_memory_map::try_from_dtb()`,
  then finds the largest free region with `try_largest_free_region()` --
  the two queries a first-stage allocator needs before a heap exists.
- **[`device_tree_demo.cpp`](examples/device_tree_demo.cpp)** -- opens a
  DTB into a caller-owned-storage `structo::device_tree` and reads its
  `/chosen` node's `bootargs`/`stdout-path` properties, the first two
  things almost every kernel/hypervisor boot path looks for.
- **[`callout_scheduler_demo.cpp`](examples/callout_scheduler_demo.cpp)**
  -- a toy, single-core `callout` subsystem built on
  `reloco::c_tailq`/`structo::callout`'s `hook_type` customization point,
  driving a periodic and a one-shot `structo::callout` off a simulated
  software clock.
- **[`sdl3_gpu_accel_demo.cpp`](examples/sdl3_gpu_accel_demo.cpp)** --
  a windowed demo wiring `hypervisor::mmio_framebuffer_device` and
  `hypervisor::mmio_gpu_command_buffer_device` (dispatching through
  `hw::gpu_accel_ref`) straight to a real SDL3 window; only built when
  SDL3 development files are found, since it is the one example with a
  real external dependency.
- **[`sdl3_vt100_framebuffer_console_demo.cpp`](examples/sdl3_vt100_framebuffer_console_demo.cpp)**
  -- a windowed demo driving `hw::vt100_terminal`/`microfmt::format_to`
  through `hw::console_ref` onto `hw::framebuffer_console` (rendered
  with the real DejaVu Sans Mono bitmap font in
  [`examples/fonts/`](examples/fonts), not `block_font_8x8`'s
  solid-block placeholder), presented via the same SDL3 window
  pipeline; also only built when SDL3 is found.
- **[`sdl3_virtio_gpu_demo.cpp`](examples/sdl3_virtio_gpu_demo.cpp)** --
  a windowed demo of `virtio::virtio_gpu_function`: a tiny in-process
  "guest" driver talks to it over `virtio_mmio_device` and a split
  virtqueue (scatter-gather backing, partial `TRANSFER_TO_HOST_2D` +
  `RESOURCE_FLUSH`), and the flushed pixels reach an SDL3 window through
  `framebuffer_accel_display`; also only built when SDL3 is found
  (`--frames N` exits after N frames, e.g. with `SDL_VIDEODRIVER=dummy`).

All six build on this library's own building blocks (typed physical
addresses/pages and the Flattened Device Tree reader/writer/index moved
in from `jplcz_reloco`; `async_kernel_object`/`callout`/the hypervisor
MMIO emulation headers native to `structo` itself), each documented on
its own [docs/](docs) page.

## Using structo

structo is consumed exactly like `jplcz_reloco`/`jplcz_microfmt`:

```cmake
include(FetchContent)
FetchContent_Declare(
    jplcz_structo
    GIT_REPOSITORY https://github.com/jplcz/structo.git
    GIT_TAG master
)
FetchContent_MakeAvailable(jplcz_structo)

target_link_libraries(my_kernel PRIVATE jplcz_structo::structo)
```

structo's own `CMakeLists.txt` fetches `jplcz_reloco` and `jplcz_microfmt` the
same way (see `JPLCZ_STRUCTO_RELOCO_SOURCE_DIR`/`JPLCZ_STRUCTO_RELOCO_GIT_REPOSITORY`/
`JPLCZ_STRUCTO_RELOCO_GIT_TAG` and their `JPLCZ_STRUCTO_MICROFMT_*`
counterparts), so a consumer never has to declare either dependency itself
unless it wants to pin/vendor a specific reloco or microfmt checkout. See
[docs/package-managers.md](docs/package-managers.md) for Conan/vcpkg/CPM
alternatives.

### Copy-paste: the full 4-step resolution block

The snippet below is exactly the pattern this project's own `CMakeLists.txt`
uses internally to consume `jplcz_reloco` (and the pattern `microfmt`/
`microvisor` use for their own dependencies). Drop it into your own
`CMakeLists.txt`, rename every `MYPROJECT_` prefix to your own project's name,
and it resolves `jplcz_structo` in order: an already-provided target, a local
checkout, your own Git remote/tag, then the official repository -- giving
downstream users of *your* project the same override knobs this project
gives its own consumers.

```cmake
# Resolve jplcz_structo: existing target > local checkout > caller's git
# remote/tag > official repository. Skip entirely if a parent build already
# provided the jplcz_structo::structo target.
if(NOT TARGET jplcz_structo::structo)
    set(MYPROJECT_STRUCTO_SOURCE_DIR "" CACHE PATH
        "Path to a local jplcz_structo checkout to use instead of fetching it")
    # Optional: allow the local checkout path via an environment variable too.
    if(NOT MYPROJECT_STRUCTO_SOURCE_DIR AND DEFINED ENV{MYPROJECT_STRUCTO_SOURCE_DIR})
        set(MYPROJECT_STRUCTO_SOURCE_DIR "$ENV{MYPROJECT_STRUCTO_SOURCE_DIR}"
            CACHE PATH
            "Path to a local jplcz_structo checkout to use instead of fetching it"
            FORCE)
    endif()
    set(MYPROJECT_STRUCTO_GIT_REPOSITORY "https://github.com/jplcz/structo.git"
        CACHE STRING
        "Git repository to fetch jplcz_structo from when MYPROJECT_STRUCTO_SOURCE_DIR is unset")
    set(MYPROJECT_STRUCTO_GIT_TAG "master" CACHE STRING
        "Git tag or commit to fetch jplcz_structo from when MYPROJECT_STRUCTO_SOURCE_DIR is unset")

    include(FetchContent)
    if(MYPROJECT_STRUCTO_SOURCE_DIR)
        FetchContent_Declare(jplcz_structo
            SOURCE_DIR "${MYPROJECT_STRUCTO_SOURCE_DIR}")
    else()
        FetchContent_Declare(jplcz_structo
            GIT_REPOSITORY "${MYPROJECT_STRUCTO_GIT_REPOSITORY}"
            GIT_TAG "${MYPROJECT_STRUCTO_GIT_TAG}")
    endif()

    # Keep structo's own development targets out of the combined build.
    # CACHE ... FORCE is required: structo's own CMakeLists.txt declares
    # these same variables via an unforced `set(... CACHE BOOL ...)`, which
    # would silently clear a plain `set()` of the same name (see "Local
    # checkout and overriding cache variables" below).
    set(JPLCZ_STRUCTO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_STRUCTO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(JPLCZ_STRUCTO_BUILD_HEADER_CHECKS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_STRUCTO_ENABLE_STRICT_WARNINGS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_STRUCTO_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(jplcz_structo)
endif()

target_link_libraries(my_target PRIVATE jplcz_structo::structo)
```

Pulling in `jplcz_reloco` directly (instead of relying on structo's own
transitive fetch) uses the identical block with `reloco`/`RELOCO` names; see
["Add jplcz_reloco"](https://github.com/jplcz/reloco#copy-paste-the-full-4-step-resolution-block)
in reloco's own README for that copy-paste block.

### Local checkout and overriding cache variables

Point `FetchContent_Declare` at a local working copy instead of fetching from
GitHub with `SOURCE_DIR`, and preset any `JPLCZ_STRUCTO_*` or, transitively,
`JPLCZ_RELOCO_*` cache variable before the `FetchContent_Declare`/
`add_subdirectory` call that brings structo (and, transitively, reloco) in:

```cmake
set(JPLCZ_STRUCTO_RELOCO_SOURCE_DIR "/path/to/local/jplcz_reloco" CACHE PATH "" FORCE)
set(JPLCZ_STRUCTO_BUILD_TESTS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
    jplcz_structo
    SOURCE_DIR /path/to/local/jplcz_structo
)
FetchContent_MakeAvailable(jplcz_structo)
```

**Always preset a dependency-owned cache variable with
`CACHE <type> "" FORCE`, never a plain `set(VAR value)`.** CMake's
`set(<var> <value> CACHE <type> <docstring>)` (without `FORCE`) silently
discards any plain/normal variable of the same name already in scope the
first time it runs -- even though the cache entry did not previously exist --
so a parent project's unforced `set(JPLCZ_STRUCTO_RELOCO_SOURCE_DIR ...)`
executed *before* structo's own `CMakeLists.txt` declares that same variable
gets silently overwritten with the empty default as soon as that
`CMakeLists.txt` runs, with no warning or error -- the built-in default (an
unpinned GitHub fetch) just wins silently. structo's own forwarded
`JPLCZ_RELOCO_*` options (`JPLCZ_RELOCO_BUILD_TESTS OFF CACHE BOOL "" FORCE`,
etc. in `CMakeLists.txt`) already follow this rule; apply the same pattern in
any project that embeds structo (see `microvisor`'s `CMakeLists.txt` for a
real-world example of the bug this avoids).

### How the jplcz_reloco dependency is resolved

`CMakeLists.txt` resolves its own `jplcz_reloco` dependency in a fixed order,
each step only taken if the previous one did not already settle the question:

1. **Detect an existing target.** `if(NOT TARGET jplcz_reloco::reloco)` guards
   the whole block: if a parent build already provided the target (its own
   `add_subdirectory`/`FetchContent_MakeAvailable(jplcz_reloco)` ran first),
   structo reuses it as-is and skips every step below.
2. **A local checkout, named by variable.** `JPLCZ_STRUCTO_RELOCO_SOURCE_DIR`
   (a `CACHE PATH`, settable with `-D` or `set(... FORCE)`, or equivalently
   the `JPLCZ_STRUCTO_RELOCO_SOURCE_DIR` environment variable when the cache
   variable is left unset) points `FetchContent_Declare`'s `SOURCE_DIR` at a
   local working copy, bypassing Git entirely.
3. **A user-selected Git remote.** If no local checkout was named,
   `JPLCZ_STRUCTO_RELOCO_GIT_REPOSITORY`/`JPLCZ_STRUCTO_RELOCO_GIT_TAG` (also
   `CACHE STRING` variables) let a consumer point `FetchContent_Declare` at
   their own fork, mirror, or pinned tag/commit instead of upstream.
4. **The official repository, by default.** If neither of the above was set,
   `JPLCZ_STRUCTO_RELOCO_GIT_REPOSITORY`/`_GIT_TAG` default to
   `https://github.com/jplcz/reloco.git`/`master`, so a plain
   `FetchContent_MakeAvailable(jplcz_structo)` with no extra configuration
   still works out of the box.

`microfmt`'s and `microvisor`'s `CMakeLists.txt` resolve their own
dependencies the same way, under the matching
`JPLCZ_MICROFMT_RELOCO_*`/`MICROVISOR_MICROFMT_*`/`MICROVISOR_STRUCTO_*`
variable names.

### How the jplcz_microfmt dependency is resolved

`CMakeLists.txt` resolves `jplcz_microfmt` with the exact same four steps,
under the `JPLCZ_STRUCTO_MICROFMT_*` variable names instead:

1. **Detect an existing target.** `if(NOT TARGET jplcz_microfmt::microfmt)`
   guards the whole block, reusing a parent-provided target as-is.
2. **A local checkout, named by variable.** `JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR`
   (`CACHE PATH`, or the environment variable of the same name) points
   `FetchContent_Declare`'s `SOURCE_DIR` at a local working copy.
3. **A user-selected Git remote.** `JPLCZ_STRUCTO_MICROFMT_GIT_REPOSITORY`/
   `JPLCZ_STRUCTO_MICROFMT_GIT_TAG` let a consumer point at their own fork,
   mirror, or pinned tag/commit.
4. **The official repository, by default.** Otherwise these default to
   `https://github.com/jplcz/microfmt.git`/`master`.

structo links `jplcz_microfmt::microfmt` into the `jplcz_structo` interface
target alongside `jplcz_reloco::reloco`, and its own development targets
(tests, examples, benchmarks, header checks, manpages, strict warnings) are
forced off the same way `jplcz_reloco`'s are (`JPLCZ_MICROFMT_BUILD_TESTS OFF
CACHE BOOL "" FORCE`, etc.) -- see the "Local checkout and overriding cache
variables" section above for why `FORCE` is required there.

## Building

```sh
cmake -S . -B build -DJPLCZ_STRUCTO_BUILD_TESTS=ON -DJPLCZ_STRUCTO_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build
```

## License

BSD-2-Clause. See [LICENSE](LICENSE).
