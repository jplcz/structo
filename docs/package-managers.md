<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Package-manager integration

`jplcz_structo` provides native metadata for Conan 2, a vcpkg overlay port,
and direct CPM.cmake/FetchContent integration, mirroring `jplcz_reloco`'s
own setup exactly (see reloco's
[docs/package-managers.md](https://github.com/jplcz/reloco/blob/master/docs/package-managers.md)).
Every integration exposes the same CMake target:

```cmake
find_package(jplcz_structo CONFIG REQUIRED)
target_link_libraries(my_target PRIVATE jplcz_structo::structo)
```

The package is header-only, requires C++17 or later, and depends on
`jplcz_reloco` (resolved transitively by every integration below).

## Conan 2

The repository root contains a Conan 2 recipe:

```sh
conan profile detect --force
conan create . --build=missing
```

The resulting reference is `jplcz_structo/0.1.0`, depending on
`jplcz_reloco/0.1.0`. A consuming `conanfile.txt`:

```ini
[requires]
jplcz_structo/0.1.0

[generators]
CMakeDeps
CMakeToolchain
```

## vcpkg

`packaging/vcpkg/ports/jplcz-structo` is a vcpkg overlay port. Add the
`packaging/vcpkg` directory as an overlay port path:

```sh
vcpkg install jplcz-structo --overlay-ports=packaging/vcpkg/ports
```

or register `packaging/vcpkg/ports` via `vcpkg-configuration.json`'s
`overlay-ports` for a manifest-mode build. The port depends on
`jplcz-reloco` (itself available the same way from reloco's own
`packaging/vcpkg/ports`).

## CPM.cmake / FetchContent

structo's own `CMakeLists.txt` is a valid `add_subdirectory`/
`FetchContent`/CPM target directly:

```cmake
include(FetchContent) # or CPMAddPackage
FetchContent_Declare(
    jplcz_structo
    GIT_REPOSITORY https://github.com/jplcz/structo.git
    GIT_TAG master
)
set(JPLCZ_STRUCTO_BUILD_TESTS OFF CACHE BOOL "")
set(JPLCZ_STRUCTO_BUILD_EXAMPLES OFF CACHE BOOL "")
FetchContent_MakeAvailable(jplcz_structo)

target_link_libraries(my_target PRIVATE jplcz_structo::structo)
```

This transitively fetches `jplcz_reloco` the same way (see
`JPLCZ_STRUCTO_RELOCO_SOURCE_DIR`/`JPLCZ_STRUCTO_RELOCO_GIT_REPOSITORY`/
`JPLCZ_STRUCTO_RELOCO_GIT_TAG` in structo's `CMakeLists.txt`) unless a
`jplcz_reloco::reloco` target already exists in the combined build (e.g.
a parent project's own earlier `add_subdirectory`/`FetchContent` of
reloco), in which case that target is reused as-is.

## `add_subdirectory`

Vendoring structo directly (e.g. as a git submodule) works the same way:

```cmake
set(JPLCZ_STRUCTO_BUILD_TESTS OFF CACHE BOOL "")
set(JPLCZ_STRUCTO_BUILD_EXAMPLES OFF CACHE BOOL "")
add_subdirectory(third_party/structo)

target_link_libraries(my_target PRIVATE jplcz_structo::structo)
```
