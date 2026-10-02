# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause
#
# A fully standalone CMake project -- deliberately *not* `add_subdirectory`'d
# from the main `structo` build (see `examples/CMakeLists.txt`'s hosted
# demos for that pattern instead). This demo cross-compiles a freestanding,
# non-relocatable x86 kernel image with its own compiler, its own linker
# script, and no dependency on the host's own C++ standard library being
# linkable -- none of which belongs in (or should ever gate) the main
# project's ordinary hosted build. Configure and build it on its own:
#
#   cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-i686.cmake
#   cmake --build build --target baremetal_x86_demo
#   cmake --build build --target run_baremetal_x86_demo
#
# See README.md for the full rationale, the exact QEMU/OVMF invocation,
# and why a Multiboot2 kernel needs GRUB (not QEMU's own `-kernel` loader,
# which only understands Multiboot *1*) to boot at all.

cmake_minimum_required(VERSION 3.20)
project(structo_baremetal_x86_demo LANGUAGES C CXX ASM)

if(NOT CMAKE_CROSSCOMPILING)
    message(WARNING
        "This demo is meant to be configured with "
        "-DCMAKE_TOOLCHAIN_FILE=toolchain-i686.cmake (see README.md); "
        "building with the host's own default compiler is untested and "
        "will very likely fail to link.")
endif()

# ============================================================================
# Dependency resolution
# ============================================================================
#
# A deliberately minimal, standalone equivalent of the main project's own
# FetchContent-based dependency resolution (see the top-level
# CMakeLists.txt/README.md): this tiny demo isn't worth pulling that whole
# machinery into, so it just expects sibling checkouts of `structo` itself,
# `reloco`, and `microfmt` next to each other on disk (exactly this
# repository's own development layout) -- override any of the three
# directly if that doesn't match your setup.

get_filename_component(JPLCZ_STRUCTO_BAREMETAL_DEMO_PROJECTS_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)

set(STRUCTO_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include" CACHE PATH
    "Path to structo's own include/ directory")
set(RELOCO_INCLUDE_DIR "${JPLCZ_STRUCTO_BAREMETAL_DEMO_PROJECTS_DIR}/reloco/include" CACHE PATH
    "Path to jplcz_reloco's include/ directory")
set(MICROFMT_INCLUDE_DIR "${JPLCZ_STRUCTO_BAREMETAL_DEMO_PROJECTS_DIR}/microfmt/include" CACHE PATH
    "Path to jplcz_microfmt's include/ directory")

foreach(dir STRUCTO_INCLUDE_DIR RELOCO_INCLUDE_DIR MICROFMT_INCLUDE_DIR)
    if(NOT EXISTS "${${dir}}")
        message(FATAL_ERROR
            "${dir} (${${dir}}) does not exist -- pass -D${dir}=/path/to/include "
            "if your checkouts of structo/reloco/microfmt aren't sibling "
            "directories of each other.")
    endif()
endforeach()

# ============================================================================
# The kernel image
# ============================================================================

add_executable(baremetal_x86_demo
    boot.s
    crt0.cpp
    multiboot_header.cpp
    multiboot1_header.cpp
    panic.cpp
    monotonic_allocator.cpp
    kmain.cpp
    thirdparty/freebsd_libc/strlen.c
    thirdparty/freebsd_libc/memchr.c
    thirdparty/freebsd_libc/memcmp.c
    thirdparty/freebsd_libc/memset.c
    thirdparty/freebsd_libc/memcpy.c
    thirdparty/freebsd_libc/memmove.c
)
set_target_properties(baremetal_x86_demo PROPERTIES
    OUTPUT_NAME "baremetal_x86_demo"
    SUFFIX ".elf"
)

target_include_directories(baremetal_x86_demo PRIVATE
    "${STRUCTO_INCLUDE_DIR}"
    "${RELOCO_INCLUDE_DIR}"
    "${MICROFMT_INCLUDE_DIR}"
)

# RELOCO_KERNEL (see panic.hpp/README.md's "Why RELOCO_KERNEL" section)
# routes reloco/structo's RELOCO_ASSERT failure path to baremetal_panic()
# instead of a hosted std::fprintf(stderr, ...) default handler. Every
# C++ translation unit gets `-include panic.hpp` force-included ahead of
# anything else, so that declaration (and RELOCO_KERNEL_PANIC's
# definition) is visible before the first reloco header is ever parsed,
# regardless of this file's own #include order.
#
# RELOCO_DEFAULT_ALLOCATOR_CUSTOM (see monotonic_allocator.hpp/.cpp)
# suppresses reloco's own built-in reloco::default_allocator() (the
# process heap, via std::malloc -- unavailable in this -nostdlib build
# anyway) so monotonic_allocator.cpp's single out-of-line definition is
# the only one that exists.
target_compile_definitions(baremetal_x86_demo PRIVATE
    RELOCO_KERNEL=1
    RELOCO_DEFAULT_ALLOCATOR_CUSTOM=1
    MICROFMT_DISABLE_ASSERT_STDIO=1
)
target_compile_options(baremetal_x86_demo PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++20>
    $<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/panic.hpp>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-stack-protector>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-pic>
    # No threads exist in this freestanding kernel, and -nostdlib has no
    # libsupc++ to supply the __cxa_guard_acquire/release pair GCC would
    # otherwise emit to make function-local statics (see
    # monotonic_allocator.cpp's heap_allocator()) thread-safe: without
    # this flag, the very first such static anywhere in the program
    # would fail to link.
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
    $<$<COMPILE_LANGUAGE:CXX>:-mno-sse>
    $<$<COMPILE_LANGUAGE:CXX>:-mno-mmx>
    $<$<COMPILE_LANGUAGE:CXX>:-mno-80387>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
    # thirdparty/freebsd_libc/*.c is vendored, unmodified-apart-from-porting
    # upstream code (see its own README.md) -- not held to this project's
    # own warning/style conventions, and -fno-builtin is required so the
    # compiler cannot recognize/inline these definitions as calls to
    # themselves (which would silently miscompile into infinite recursion).
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    $<$<COMPILE_LANGUAGE:C>:-fno-builtin>
    $<$<COMPILE_LANGUAGE:C>:-w>
)

target_link_options(baremetal_x86_demo PRIVATE
    -nostdlib
    -static
    -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/linker.ld
)
# -lgcc must come after every object file on the link line (it's a static
# archive resolving leftover 64-bit-arithmetic helpers like __udivdi3/
# __umoddi3 that 32-bit x86 has no native instructions for) -- expressing
# it via target_link_libraries (rather than target_link_options) is what
# guarantees CMake places it at the very end of the generated command.
target_link_libraries(baremetal_x86_demo PRIVATE gcc)
set_target_properties(baremetal_x86_demo PROPERTIES
    LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/linker.ld"
)

# ============================================================================
# Bootable ISO (GRUB2, Multiboot2) + QEMU/OVMF convenience targets
# ============================================================================
#
# QEMU's own `-kernel` Multiboot loader only ever understood Multiboot
# *1* (see README.md) -- actually booting this Multiboot *2* image needs a
# real Multiboot2-aware bootloader, i.e. GRUB2, packaged into a bootable
# ISO via `grub-mkrescue`. Both targets below are omitted (not errors) if
# the required tool isn't installed.

find_program(GRUB_MKRESCUE grub-mkrescue)
find_program(QEMU_SYSTEM_X86_64 qemu-system-x86_64)
find_file(OVMF_FIRMWARE
    NAMES OVMF.fd
    PATHS /usr/share/qemu /usr/share/ovmf /usr/share/OVMF
)

if(GRUB_MKRESCUE)
    set(iso_root "${CMAKE_CURRENT_BINARY_DIR}/isoroot")
    set(iso_path "${CMAKE_CURRENT_BINARY_DIR}/baremetal_x86_demo.iso")

    add_custom_command(
        OUTPUT "${iso_path}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${iso_root}/boot/grub"
        COMMAND ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:baremetal_x86_demo>" "${iso_root}/boot/kernel.elf"
        COMMAND ${CMAKE_COMMAND} -E echo "set timeout=0" > "${iso_root}/boot/grub/grub.cfg"
        COMMAND ${CMAKE_COMMAND} -E echo "set default=0" >> "${iso_root}/boot/grub/grub.cfg"
        COMMAND ${CMAKE_COMMAND} -E echo "menuentry \"structo baremetal x86 demo\" {" >> "${iso_root}/boot/grub/grub.cfg"
        COMMAND ${CMAKE_COMMAND} -E echo "  multiboot2 /boot/kernel.elf" >> "${iso_root}/boot/grub/grub.cfg"
        COMMAND ${CMAKE_COMMAND} -E echo "  boot" >> "${iso_root}/boot/grub/grub.cfg"
        COMMAND ${CMAKE_COMMAND} -E echo "}" >> "${iso_root}/boot/grub/grub.cfg"
        COMMAND ${GRUB_MKRESCUE} -o "${iso_path}" "${iso_root}"
        DEPENDS baremetal_x86_demo
        COMMENT "Building Multiboot2 GRUB2 ISO (baremetal_x86_demo.iso)"
        VERBATIM
    )
    add_custom_target(baremetal_x86_demo_iso DEPENDS "${iso_path}")

    if(QEMU_SYSTEM_X86_64 AND OVMF_FIRMWARE)
        add_custom_target(run_baremetal_x86_demo
            COMMAND ${QEMU_SYSTEM_X86_64}
                -bios "${OVMF_FIRMWARE}"
                -cdrom "${iso_path}"
                -m 256
                -nographic
                -no-reboot
            DEPENDS baremetal_x86_demo_iso
            COMMENT "Booting baremetal_x86_demo.iso under qemu-system-x86_64 + OVMF"
            USES_TERMINAL
        )
    endif()
endif()

# A second, much simpler convenience target: QEMU's own `-kernel` loader
# understands the Multiboot *1* header `multiboot1_header.cpp` embeds, so
# `qemu-system-i386` can load this exact same ELF image directly -- no
# GRUB/ISO/OVMF detour at all. See README.md for why the ISO/OVMF path
# above exists in the first place (Multiboot 2 has no such shortcut).
find_program(QEMU_SYSTEM_I386 qemu-system-i386)
if(QEMU_SYSTEM_I386)
    add_custom_target(run_baremetal_x86_demo_mb1
        COMMAND ${QEMU_SYSTEM_I386}
            -kernel "$<TARGET_FILE:baremetal_x86_demo>"
            -m 256
            -serial stdio
            -no-reboot
        DEPENDS baremetal_x86_demo
        COMMENT "Booting baremetal_x86_demo.elf directly under qemu-system-i386 (Multiboot 1)"
        USES_TERMINAL
    )
endif()

