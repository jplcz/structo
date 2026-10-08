# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required(VERSION 3.20)
project(jplcz_structo VERSION 0.2.1 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(JPLCZ_STRUCTO_IS_TOP_LEVEL ON)
else()
    set(JPLCZ_STRUCTO_IS_TOP_LEVEL OFF)
endif()

set(JPLCZ_STRUCTO_DEVELOPMENT_DEFAULT ${JPLCZ_STRUCTO_IS_TOP_LEVEL})

option(JPLCZ_STRUCTO_ENABLE_STRICT_WARNINGS
    "Enable comprehensive GCC and Clang warnings" ${JPLCZ_STRUCTO_DEVELOPMENT_DEFAULT})
option(JPLCZ_STRUCTO_ENABLE_UNSAFE_BUFFER_USAGE
    "Enable Clang unsafe buffer usage diagnostics" OFF)
option(JPLCZ_STRUCTO_ENABLE_WERROR
    "Enable build with -Werror" ${JPLCZ_STRUCTO_DEVELOPMENT_DEFAULT})
option(JPLCZ_STRUCTO_BUILD_TESTS
    "Build test suite" ${JPLCZ_STRUCTO_DEVELOPMENT_DEFAULT})
option(JPLCZ_STRUCTO_BUILD_EXAMPLES
    "Build example apps" ${JPLCZ_STRUCTO_DEVELOPMENT_DEFAULT})
option(JPLCZ_STRUCTO_BUILD_HEADER_CHECKS
    "Compile all public headers under each supported C++ standard"
    ${JPLCZ_STRUCTO_DEVELOPMENT_DEFAULT})
option(JPLCZ_STRUCTO_CHECK_NO_STD_TYPES
    "Hard build-time check that public headers avoid allocating/throwing/\
glibc-dependent std:: facilities outside explicitly marked interop points \
(see scripts/check-no-std-types.py)"
    ${JPLCZ_STRUCTO_DEVELOPMENT_DEFAULT})
option(JPLCZ_STRUCTO_INSTALL
    "Install jplcz_structo headers and CMake package metadata"
    ${JPLCZ_STRUCTO_IS_TOP_LEVEL})
option(JPLCZ_STRUCTO_INSTALL_HEADERS_ONLY
    "When JPLCZ_STRUCTO_INSTALL is enabled, install only the public \
headers and skip everything else this project would otherwise install: \
README/LICENSE/docs/ and the CMake package (install(TARGETS ... EXPORT \
...), install(EXPORT ...), Config/ConfigVersion files). Also forces its \
fetched jplcz_reloco/jplcz_microfmt dependencies' own \
JPLCZ_RELOCO_INSTALL_HEADERS_ONLY/JPLCZ_MICROFMT_INSTALL_HEADERS_ONLY to \
match. Needed when jplcz_structo is pulled in via FetchContent/\
add_subdirectory by a larger build (e.g. an embedded/kernel tree) that \
defines its own targets depending on jplcz_structo (and jplcz_reloco/\
jplcz_microfmt) and performs its own single, consolidated \
install(EXPORT) for all of them -- CMake forbids an exported target from \
depending on another target that is itself exported under more than one \
export set/namespace, so jplcz_structo must not also export itself (or \
its dependencies) on its own in that situation; the caller remains \
responsible for installing/exporting those targets (and their CMake \
packages) itself." OFF)

# Included unconditionally (not only under JPLCZ_STRUCTO_INSTALL), and
# harmlessly idempotent if a parent build already included it, purely to
# make GNUInstallDirs' CMAKE_INSTALL_* cache variables available as
# defaults for the JPLCZ_STRUCTO_INSTALL_* overrides immediately below --
# these are consulted by every install() destination further down,
# regardless of JPLCZ_STRUCTO_INSTALL's own value at this point in the
# file (option() processing order). Mirrors reloco's/microfmt's own
# JPLCZ_*_INSTALL_*DIR block.
include(GNUInstallDirs)

# Each defaults to the plain GNUInstallDirs value, but can be overridden
# independently of CMAKE_INSTALL_PREFIX/CMAKE_INSTALL_*, e.g. by a custom
# RTOS/hypervisor/SDK build tree that already repurposes the standard
# CMAKE_INSTALL_INCLUDEDIR/CMAKE_INSTALL_DATADIR cache variables for its
# own unrelated headers/CMake packages and needs structo installed under
# a distinct path instead.
#
# Cache type STRING, not PATH, deliberately -- see reloco/microfmt's own
# CMakeLists.txt for the full rationale (a PATH-typed cache entry set
# from a bare `-DVAR=value` collapses to a cwd-relative absolute path,
# breaking the relative $<INSTALL_INTERFACE:...> usage below).
set(JPLCZ_STRUCTO_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}" CACHE STRING
    "Install destination for structo's public headers (relative to \
CMAKE_INSTALL_PREFIX unless absolute)")
set(JPLCZ_STRUCTO_INSTALL_DOCDIR "${CMAKE_INSTALL_DOCDIR}" CACHE STRING
    "Install destination for structo's README/LICENSE/docs/ (relative \
to CMAKE_INSTALL_PREFIX unless absolute)")
set(JPLCZ_STRUCTO_INSTALL_CMAKEDIR "${CMAKE_INSTALL_DATADIR}/cmake/jplcz_structo" CACHE STRING
    "Install destination for jplcz_structo's CMake package config/\
targets/version files (relative to CMAKE_INSTALL_PREFIX unless \
absolute)")

if(JPLCZ_STRUCTO_ENABLE_STRICT_WARNINGS OR JPLCZ_STRUCTO_ENABLE_UNSAFE_BUFFER_USAGE OR
   JPLCZ_STRUCTO_BUILD_HEADER_CHECKS OR JPLCZ_STRUCTO_BUILD_TESTS)
    include(CheckCXXCompilerFlag)

    if(MSVC)
        check_cxx_compiler_flag("/std:c++latest"
            JPLCZ_STRUCTO_COMPILER_SUPPORTS_CXX23)
    else()
        check_cxx_compiler_flag("-std=c++23"
            JPLCZ_STRUCTO_COMPILER_SUPPORTS_CXX23)
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        check_cxx_compiler_flag("-Wunsafe-buffer-usage"
            JPLCZ_STRUCTO_COMPILER_SUPPORTS_WUNSAFE_BUFFER_USAGE)
    endif()
endif()

# jplcz_reloco dependency
#
# jplcz_reloco provides the hardened container / lifetime-safety /
# OS-development primitives (fdt_reader/fdt_index/fdt_memory,
# region_set/phys_addr, checked containers, ...) that jplcz_structo's
# kernel/hypervisor/trusted-boundary building blocks are composed from.
# Callers that already vendor or pin a specific reloco checkout can point
# JPLCZ_STRUCTO_RELOCO_SOURCE_DIR at it; otherwise it is fetched from its
# GitHub repository at a pinned commit. Mirrors microfmt's own
# JPLCZ_MICROFMT_RELOCO_* block (microfmt/CMakeLists.txt) exactly.
#
# If a parent project already brought in jplcz_reloco itself (e.g. its own
# `add_subdirectory(path/to/reloco)` before `add_subdirectory(path/to/structo)`,
# or its own earlier FetchContent_MakeAvailable(jplcz_reloco)), the
# jplcz_reloco::reloco target already exists by the time this file runs:
# skip fetching/declaring it a second time and reuse the parent's target
# as-is instead of silently overriding it.
if(NOT TARGET jplcz_reloco::reloco)
    set(JPLCZ_STRUCTO_RELOCO_SOURCE_DIR "" CACHE PATH
        "Path to a local jplcz_reloco checkout to use instead of fetching it")
    # Allow the local checkout path to be supplied via an environment variable
    # (e.g. for CI or shell profiles) without having to pass -D on every configure.
    # The explicit CMake cache variable/-D option always takes precedence.
    if(NOT JPLCZ_STRUCTO_RELOCO_SOURCE_DIR AND DEFINED ENV{JPLCZ_STRUCTO_RELOCO_SOURCE_DIR})
        set(JPLCZ_STRUCTO_RELOCO_SOURCE_DIR "$ENV{JPLCZ_STRUCTO_RELOCO_SOURCE_DIR}" CACHE PATH
            "Path to a local jplcz_reloco checkout to use instead of fetching it" FORCE)
    endif()
    set(JPLCZ_STRUCTO_RELOCO_GIT_REPOSITORY "https://github.com/jplcz/reloco.git" CACHE STRING
        "Git repository to fetch jplcz_reloco from when JPLCZ_STRUCTO_RELOCO_SOURCE_DIR is unset")
    set(JPLCZ_STRUCTO_RELOCO_GIT_TAG "master" CACHE STRING
        "Git tag or commit to fetch jplcz_reloco from when JPLCZ_STRUCTO_RELOCO_SOURCE_DIR is unset")

    include(FetchContent)
    if(JPLCZ_STRUCTO_RELOCO_SOURCE_DIR)
        FetchContent_Declare(
            jplcz_reloco
            SOURCE_DIR "${JPLCZ_STRUCTO_RELOCO_SOURCE_DIR}"
        )
    else()
        FetchContent_Declare(
            jplcz_reloco
            GIT_REPOSITORY "${JPLCZ_STRUCTO_RELOCO_GIT_REPOSITORY}"
            GIT_TAG "${JPLCZ_STRUCTO_RELOCO_GIT_TAG}"
        )
    endif()

    # jplcz_reloco is consumed as a header-only build dependency: keep its own
    # development targets (tests, header checks, strict warnings) out of the
    # combined build, and mirror JPLCZ_STRUCTO_INSTALL so an installed
    # jplcz_structo package also installs and exports the jplcz_reloco
    # package it depends on.
    set(JPLCZ_RELOCO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_RELOCO_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_RELOCO_BUILD_HEADER_CHECKS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_RELOCO_ENABLE_STRICT_WARNINGS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_RELOCO_INSTALL ${JPLCZ_STRUCTO_INSTALL} CACHE BOOL "" FORCE)
    set(JPLCZ_RELOCO_INSTALL_HEADERS_ONLY ${JPLCZ_STRUCTO_INSTALL_HEADERS_ONLY} CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(jplcz_reloco)
endif()

# jplcz_microfmt dependency
#
# jplcz_microfmt provides the allocation-free, no-libc-growth formatting
# primitives (format/parse, cbor encode/decode) that jplcz_structo's
# diagnostic/tracing paths are built on (e.g. serializing boot state for
# early-console logging without pulling in iostream/printf). Callers that
# already vendor or pin a specific microfmt checkout can point
# JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR at it; otherwise it is fetched from
# its GitHub repository at a pinned commit. Mirrors the jplcz_reloco
# block immediately above exactly.
#
# If a parent project already brought in jplcz_microfmt itself, the
# jplcz_microfmt::microfmt target already exists by the time this file
# runs: skip fetching/declaring it a second time and reuse the parent's
# target as-is instead of silently overriding it.
if(NOT TARGET jplcz_microfmt::microfmt)
    set(JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR "" CACHE PATH
        "Path to a local jplcz_microfmt checkout to use instead of fetching it")
    # Allow the local checkout path to be supplied via an environment variable
    # (e.g. for CI or shell profiles) without having to pass -D on every configure.
    # The explicit CMake cache variable/-D option always takes precedence.
    if(NOT JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR AND DEFINED ENV{JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR})
        set(JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR "$ENV{JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR}" CACHE PATH
            "Path to a local jplcz_microfmt checkout to use instead of fetching it" FORCE)
    endif()
    set(JPLCZ_STRUCTO_MICROFMT_GIT_REPOSITORY "https://github.com/jplcz/microfmt.git" CACHE STRING
        "Git repository to fetch jplcz_microfmt from when JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR is unset")
    set(JPLCZ_STRUCTO_MICROFMT_GIT_TAG "master" CACHE STRING
        "Git tag or commit to fetch jplcz_microfmt from when JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR is unset")

    include(FetchContent)
    if(JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR)
        FetchContent_Declare(
            jplcz_microfmt
            SOURCE_DIR "${JPLCZ_STRUCTO_MICROFMT_SOURCE_DIR}"
        )
    else()
        FetchContent_Declare(
            jplcz_microfmt
            GIT_REPOSITORY "${JPLCZ_STRUCTO_MICROFMT_GIT_REPOSITORY}"
            GIT_TAG "${JPLCZ_STRUCTO_MICROFMT_GIT_TAG}"
        )
    endif()

    # jplcz_microfmt is consumed as a header-only build dependency: keep its
    # own development targets (tests, examples, benchmarks, header checks,
    # manpages, strict warnings) out of the combined build, and mirror
    # JPLCZ_STRUCTO_INSTALL so an installed jplcz_structo package also
    # installs and exports the jplcz_microfmt package it depends on.
    set(JPLCZ_MICROFMT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_MICROFMT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(JPLCZ_MICROFMT_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_MICROFMT_BUILD_HEADER_CHECKS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_MICROFMT_BUILD_MANPAGES OFF CACHE BOOL "" FORCE)
    set(JPLCZ_MICROFMT_ENABLE_STRICT_WARNINGS OFF CACHE BOOL "" FORCE)
    set(JPLCZ_MICROFMT_INSTALL ${JPLCZ_STRUCTO_INSTALL} CACHE BOOL "" FORCE)
    set(JPLCZ_MICROFMT_INSTALL_HEADERS_ONLY ${JPLCZ_STRUCTO_INSTALL_HEADERS_ONLY} CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(jplcz_microfmt)
endif()

# Header-only interface target
add_library(jplcz_structo INTERFACE)
target_compile_features(jplcz_structo INTERFACE cxx_std_17)
target_compile_options(jplcz_structo INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)
set_target_properties(jplcz_structo PROPERTIES EXPORT_NAME structo)

target_include_directories(jplcz_structo INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${JPLCZ_STRUCTO_INSTALL_INCLUDEDIR}>
)

target_link_libraries(jplcz_structo INTERFACE jplcz_reloco::reloco jplcz_microfmt::microfmt)

add_library(jplcz_structo::structo ALIAS jplcz_structo)

add_library(jplcz_structo_warnings INTERFACE)
if(JPLCZ_STRUCTO_ENABLE_UNSAFE_BUFFER_USAGE AND
   NOT JPLCZ_STRUCTO_COMPILER_SUPPORTS_WUNSAFE_BUFFER_USAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR
            "JPLCZ_STRUCTO_ENABLE_UNSAFE_BUFFER_USAGE requires a compiler that supports "
            "-Wunsafe-buffer-usage")
    else()
        set(JPLCZ_STRUCTO_ENABLE_UNSAFE_BUFFER_USAGE OFF)
    endif()
endif()

if(JPLCZ_STRUCTO_ENABLE_STRICT_WARNINGS)
    if(MSVC)
        target_compile_options(jplcz_structo_warnings INTERFACE /W4)
    else()
        target_compile_options(jplcz_structo_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion)
    endif()
endif()

if(JPLCZ_STRUCTO_ENABLE_UNSAFE_BUFFER_USAGE AND JPLCZ_STRUCTO_COMPILER_SUPPORTS_WUNSAFE_BUFFER_USAGE)
    target_compile_options(jplcz_structo_warnings INTERFACE -Wunsafe-buffer-usage)
endif()

if(JPLCZ_STRUCTO_ENABLE_WERROR)
    if(MSVC)
        target_compile_options(jplcz_structo_warnings INTERFACE /WX)
    else()
        target_compile_options(jplcz_structo_warnings INTERFACE -Werror)
    endif()
endif()

if(JPLCZ_STRUCTO_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.zip
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    include(GoogleTest)

    add_executable(jplcz_structo_tests
        tests/test_boot_memory_map.cpp
        tests/test_device_tree.cpp
        tests/test_phys_addr.cpp
        tests/test_phys_page.cpp
        tests/test_slot_map_ptr.cpp
        tests/test_bitmap_utils.cpp
        tests/test_bitmap_types.cpp
        tests/test_target_ptr.cpp
        tests/test_io_address.cpp
        tests/test_io_space_ref.cpp
        tests/test_mmio_space.cpp
        tests/test_multiboot2.cpp
        tests/test_uart_ref.cpp
        tests/test_xmodem.cpp
        tests/test_timer_ref.cpp
        tests/test_irqc_ref.cpp
        tests/test_framebuffer.cpp
        tests/test_console_ref.cpp
        tests/test_vga_text_console.cpp
        tests/test_framebuffer_console.cpp
        tests/test_vt100.cpp
        tests/test_dejavu_font.cpp
        tests/test_clock_cycles.cpp
        tests/test_time_source_ref.cpp
        tests/test_vdso_clock_page.cpp
        tests/test_vdso_clock_writer.cpp
        tests/test_vdso_clock_reader.cpp
        tests/test_time_manager.cpp
        tests/test_instant_clock_traits.cpp
        tests/test_vm_time_manager.cpp
        tests/test_async_kernel_object.cpp
        tests/test_callout.cpp
        tests/test_load_average.cpp
        tests/test_cpu_load.cpp
        tests/test_runqueue.cpp
        tests/test_sched.cpp
        tests/test_sched_world_handoff.cpp
        tests/test_phys_translator.cpp
        tests/test_region_set.cpp
        tests/test_early_region_allocator.cpp
        tests/test_sg_list.cpp
        tests/test_sg_translation.cpp
        tests/test_fdt_reader.cpp
        tests/test_fdt_writer.cpp
        tests/test_fdt_index.cpp
        tests/test_fdt_memory.cpp
        tests/test_fdt_real_world.cpp
        tests/test_buddy_allocator.cpp
        tests/test_ipc_buffer.cpp
        tests/test_lazy_context.cpp
        tests/test_cpu_index.cpp
        tests/test_hw_id_map.cpp
        tests/test_fdt_cpu_map.cpp
        tests/test_cpu_topology.cpp
        tests/test_fdt_cpu_topology.cpp
        tests/test_cpu_sibling_map.cpp
        tests/test_work_steal.cpp
        tests/test_irq_guard.cpp
        tests/test_preemption_guard.cpp
        tests/test_spinlock_entry_guard.cpp
        tests/test_core_pin_guard.cpp
        tests/test_core_rendezvous_barrier.cpp
        tests/test_early_rendezvous_barrier.cpp
        tests/test_cpu_online_dispatcher.cpp
        tests/test_ipi_dispatcher.cpp
        tests/test_backoff.cpp
        tests/test_softlock_detector.cpp
        tests/test_kernel_spin_lock.cpp
        tests/test_ticket_spin_lock.cpp
        tests/test_queue_spin_lock.cpp
        tests/test_rw_spin_lock.cpp
        tests/test_queue_rw_spin_lock.cpp
        tests/test_irq_spin_lock.cpp
        tests/test_irq_rw_spin_lock.cpp
        tests/test_guarded_spin_mutex.cpp
        tests/test_guarded_rw_spin_mutex.cpp
        tests/test_lock_striping.cpp
        tests/test_per_cpu_ptr.cpp
        tests/test_per_thread_ptr.cpp
        tests/test_per_domain_ptr.cpp
        tests/test_asid_allocator.cpp
        tests/test_fixed_asid_allocator.cpp
        tests/test_tlb_flush.cpp
        tests/test_cpu_mask.cpp
        tests/test_mm_asid_context.cpp
        tests/test_page_table_traits.cpp
        tests/test_page_table_range.cpp
        tests/test_page_table_occupancy.cpp
        tests/test_arch_page_table_traits.cpp
        tests/test_arch_pte_fields.cpp
        tests/test_mmu_regs.cpp
        tests/test_hw_rng.cpp
        tests/test_hw_rng_combinator.cpp
        tests/test_otp_storage.cpp
        tests/test_block_device_ref.cpp
        tests/test_ram_disk.cpp
        tests/test_fdt_initrd.cpp
        tests/test_prng.cpp
        tests/test_debug_symtab.cpp
        tests/test_world_switch_guard.cpp
        tests/test_backref_ptr.cpp
        tests/test_backref_owner.cpp
        tests/test_vcpu_state_guard.cpp
        tests/test_vm_exit_dispatcher.cpp
        tests/test_hypercall_dispatcher.cpp
        tests/test_arm_smccc.cpp
        tests/test_arm_psci.cpp
        tests/test_mmio_device_ref.cpp
        tests/test_mmio_print_device.cpp
        tests/test_mmio_text_console.cpp
        tests/test_mmio_framebuffer_device.cpp
        tests/test_gpu_accel_ref.cpp
        tests/test_mmio_gpu_command_buffer_device.cpp
        tests/test_virtq_types.cpp
        tests/test_virtq_layout.cpp
        tests/test_virtq_memory.cpp
        tests/test_split_ring.cpp
        tests/test_split_ring_features.cpp
        tests/test_packed_ring.cpp
        tests/test_virtq_ref.cpp
        tests/test_virtio_blk.cpp
        tests/test_virtio_mmio.cpp
        tests/test_virtio_rpmsg.cpp
        tests/test_virtio_rng.cpp
        tests/test_virtio_console.cpp
        tests/test_virtio_balloon.cpp
        tests/test_virtio_gpu.cpp
    )
    target_link_libraries(jplcz_structo_tests PRIVATE
        jplcz_structo jplcz_structo_warnings GTest::gtest_main)
    target_include_directories(jplcz_structo_tests PRIVATE tests examples)
    gtest_discover_tests(jplcz_structo_tests)

    # Coroutine-based devices need C++20; own target so the main suite stays C++17.
    add_executable(jplcz_structo_coroutine_tests tests/test_polled_net_device.cpp)
    target_link_libraries(jplcz_structo_coroutine_tests PRIVATE
        jplcz_structo jplcz_structo_warnings GTest::gtest_main)
    set_target_properties(jplcz_structo_coroutine_tests PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    gtest_discover_tests(jplcz_structo_coroutine_tests TEST_PREFIX "Coroutine.")

    # tests/test_ipc_ring_fault_injection.cpp defines
    # RELOCO_ENABLE_FAULT_INJECTION, which changes the *body* of
    # reloco_ipc_ring.hpp's implicitly-inline (external/vague linkage)
    # ipc_producer::write_slices()/ipc_consumer::read_slices() member
    # functions. Linking that translation unit into the same binary as
    # any other TU that includes reloco_ipc_ring.hpp *without* the macro
    # (e.g. test_ipc_buffer.cpp, in jplcz_structo_tests) would be an ODR
    # violation, so it gets its own, fully separate binary.
    add_executable(jplcz_structo_ipc_ring_fault_injection_tests tests/test_ipc_ring_fault_injection.cpp)
    target_link_libraries(jplcz_structo_ipc_ring_fault_injection_tests PRIVATE
        jplcz_structo jplcz_structo_warnings GTest::gtest_main)
    gtest_discover_tests(jplcz_structo_ipc_ring_fault_injection_tests)
endif()

if(JPLCZ_STRUCTO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(JPLCZ_STRUCTO_BUILD_HEADER_CHECKS)
    # Recurse into every public subdirectory (arch/, sync/, hw/, hypervisor/, virtio/, ...)
    # alongside the top-level headers; include/structo/detail/ is
    # deliberately excluded -- it holds private implementation headers,
    # never included directly by consumers, and is not part of the public
    # API surface this target verifies.
    #
    # arch/ uses GLOB_RECURSE (rather than a flat "arch/*.hpp" pattern
    # like the other subdirectories) because it itself has per-
    # architecture subdirectories (arch/arm64/, arch/arm/, arch/riscv/,
    # arch/x86/, ...) that would otherwise need a new explicit pattern
    # added here every time one is introduced.
    file(GLOB_RECURSE JPLCZ_STRUCTO_ARCH_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/structo/arch/*.hpp"
    )
    file(GLOB JPLCZ_STRUCTO_PUBLIC_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/structo/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/structo/sync/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/structo/hw/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/structo/hypervisor/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/structo/virtio/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/structo/bootldr/*.hpp"
    )
    list(APPEND JPLCZ_STRUCTO_PUBLIC_HEADERS ${JPLCZ_STRUCTO_ARCH_HEADERS})

    function(add_jplcz_structo_header_check std)
        set(check_dir "${CMAKE_CURRENT_BINARY_DIR}/header_check_cxx${std}")
        file(MAKE_DIRECTORY "${check_dir}")
        set(sources "")
        foreach(header ${JPLCZ_STRUCTO_PUBLIC_HEADERS})
            # Use the path relative to include/structo/ (e.g. "arch/cpu_index")
            # both for the #include directive below and to build a unique
            # generated source file name -- a flat NAME_WE would collide if
            # two subdirectories ever ship a header with the same base name.
            file(RELATIVE_PATH rel_header "${CMAKE_CURRENT_SOURCE_DIR}/include/structo" "${header}")
            string(REGEX REPLACE "\\.hpp$" "" rel_name "${rel_header}")
            string(REPLACE "/" "_" flat_name "${rel_name}")
            set(src "${check_dir}/${flat_name}.cpp")
            file(WRITE "${src}" "#include <structo/${rel_header}>\n")
            list(APPEND sources "${src}")
        endforeach()
        add_library(jplcz_structo_headers_cxx${std} OBJECT ${sources})
        target_link_libraries(jplcz_structo_headers_cxx${std} PRIVATE jplcz_structo)
        target_compile_features(jplcz_structo_headers_cxx${std} PRIVATE cxx_std_${std})
    endfunction()

    set(JPLCZ_STRUCTO_HEADER_CHECK_TARGETS
        jplcz_structo_headers_cxx17
        jplcz_structo_headers_cxx20
    )
    add_jplcz_structo_header_check(17)
    add_jplcz_structo_header_check(20)
    if(JPLCZ_STRUCTO_COMPILER_SUPPORTS_CXX23)
        add_jplcz_structo_header_check(23)
        list(APPEND JPLCZ_STRUCTO_HEADER_CHECK_TARGETS jplcz_structo_headers_cxx23)
    endif()

    add_custom_target(jplcz_structo_check_public_headers
        DEPENDS ${JPLCZ_STRUCTO_HEADER_CHECK_TARGETS}
    )
endif()

if(JPLCZ_STRUCTO_CHECK_NO_STD_TYPES)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    file(GLOB_RECURSE JPLCZ_STRUCTO_PUBLIC_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/*.ipp"
    )

    # Hard build-time gate (part of the default `all` target, not an opt-in
    # target like jplcz_structo_check_public_headers above): fails the build
    # the moment a header introduces disallowed std:: usage, so regressions
    # are caught on every ordinary `cmake --build` rather than only in CI.
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/jplcz_structo_check_no_std_types.stamp"
        COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-no-std-types.py"
            --include-dir "${CMAKE_CURRENT_SOURCE_DIR}/include"
        COMMAND "${CMAKE_COMMAND}" -E touch
            "${CMAKE_CURRENT_BINARY_DIR}/jplcz_structo_check_no_std_types.stamp"
        DEPENDS ${JPLCZ_STRUCTO_PUBLIC_HEADERS}
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-no-std-types.py"
        COMMENT "Checking include/ for disallowed std:: usage (scripts/check-no-std-types.py)"
        VERBATIM
    )
    add_custom_target(jplcz_structo_check_no_std_types ALL
        DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/jplcz_structo_check_no_std_types.stamp"
    )
endif()

if(JPLCZ_STRUCTO_INSTALL)
    # GNUInstallDirs was already include()d unconditionally near the
    # JPLCZ_STRUCTO_INSTALL_*DIR option() block above; this second
    # include() is harmless (idempotent) and kept so this block remains
    # self-contained if ever reordered.
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    if(NOT JPLCZ_STRUCTO_INSTALL_HEADERS_ONLY)
        install(
            TARGETS jplcz_structo
            EXPORT jplcz_structoTargets
            INCLUDES DESTINATION "${JPLCZ_STRUCTO_INSTALL_INCLUDEDIR}"
        )
    endif()

    install(
        DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
        DESTINATION "${JPLCZ_STRUCTO_INSTALL_INCLUDEDIR}"
        COMPONENT Development
    )

    if(NOT JPLCZ_STRUCTO_INSTALL_HEADERS_ONLY)
        install(
            FILES
                "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
                "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
            DESTINATION "${JPLCZ_STRUCTO_INSTALL_DOCDIR}"
            COMPONENT Documentation
        )

        install(
            DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/docs/"
            DESTINATION "${JPLCZ_STRUCTO_INSTALL_DOCDIR}/docs"
            COMPONENT Documentation
        )

        configure_package_config_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/jplcz_structoConfig.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/jplcz_structoConfig.cmake"
            INSTALL_DESTINATION "${JPLCZ_STRUCTO_INSTALL_CMAKEDIR}"
            NO_SET_AND_CHECK_MACRO
            NO_CHECK_REQUIRED_COMPONENTS_MACRO
        )

        # Header-only INTERFACE library: the package config is architecture
        # independent (no compiled binaries to match ABI/bitness against), so
        # skip the platform check and install under share/, not lib/.
        write_basic_package_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/jplcz_structoConfigVersion.cmake"
            VERSION "${PROJECT_VERSION}"
            COMPATIBILITY SameMajorVersion
            ARCH_INDEPENDENT
        )

        install(
            EXPORT jplcz_structoTargets
            FILE jplcz_structoTargets.cmake
            NAMESPACE jplcz_structo::
            DESTINATION "${JPLCZ_STRUCTO_INSTALL_CMAKEDIR}"
            COMPONENT Development
        )

        install(
            FILES
                "${CMAKE_CURRENT_BINARY_DIR}/jplcz_structoConfig.cmake"
                "${CMAKE_CURRENT_BINARY_DIR}/jplcz_structoConfigVersion.cmake"
            DESTINATION "${JPLCZ_STRUCTO_INSTALL_CMAKEDIR}"
            COMPONENT Development
        )
    endif()
endif()

if(JPLCZ_STRUCTO_IS_TOP_LEVEL AND JPLCZ_STRUCTO_INSTALL)
    set(CPACK_PACKAGE_NAME "jplcz-structo")
    set(CPACK_PACKAGE_VENDOR "Jaroslaw Pelczar")
    set(CPACK_PACKAGE_CONTACT "Jaroslaw Pelczar <jarek@jpelczar.com>")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
        "Header-only OS/kernel/hypervisor building blocks (device-tree decoding, physical memory region management) built on jplcz_reloco and jplcz_microfmt")
    set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/jplcz/structo")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_VERSION_MAJOR "${PROJECT_VERSION_MAJOR}")
    set(CPACK_PACKAGE_VERSION_MINOR "${PROJECT_VERSION_MINOR}")
    set(CPACK_PACKAGE_VERSION_PATCH "${PROJECT_VERSION_PATCH}")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "jplcz_structo")
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
    set(CPACK_VERBATIM_VARIABLES ON)
    set(CPACK_THREADS 0)

    set(CPACK_SOURCE_GENERATOR "TGZ;ZIP")
    set(CPACK_SOURCE_IGNORE_FILES
        "/\\.git/"
        "/\\.idea/"
        "/\\.vscode/"
        "/build[^/]*/"
        "/cmake-build-[^/]*/"
        "/_deps/"
    )

    # Header-only package: archives always work everywhere; DEB/RPM are added
    # only when their packaging tools are available on the host.
    set(CPACK_GENERATOR "TGZ;ZIP")

    find_program(JPLCZ_STRUCTO_DPKG_DEB_EXECUTABLE dpkg-deb)
    if(JPLCZ_STRUCTO_DPKG_DEB_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "DEB")
    endif()

    find_program(JPLCZ_STRUCTO_RPMBUILD_EXECUTABLE rpmbuild)
    if(JPLCZ_STRUCTO_RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
    endif()

    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "libdevel")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "all")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

    set(CPACK_RPM_PACKAGE_LICENSE "BSD-2-Clause")
    set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_RPM_PACKAGE_ARCHITECTURE "noarch")
    set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
    set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION_SUMMARY}")

    include(CPack)
endif()
