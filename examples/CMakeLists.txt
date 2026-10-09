# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

function(add_jplcz_structo_example NAME)
    add_executable(${NAME} ${NAME}.cpp)
    target_link_libraries(${NAME} PRIVATE jplcz_structo jplcz_structo_warnings)
endfunction()

add_jplcz_structo_example(boot_memory_map_demo)
add_jplcz_structo_example(device_tree_demo)
add_jplcz_structo_example(callout_scheduler_demo)

set(JPLCZ_STRUCTO_EXAMPLE_TARGETS
    boot_memory_map_demo
    device_tree_demo
    callout_scheduler_demo
)

# Optional: a windowed demo rendering `gpu_accel_ref`/`mmio_gpu_command_buffer_device`-
# dispatched drawing commands onto an `mmio_framebuffer_device`-owned pixel buffer, presented
# via SDL3 -- only built when an SDL3 development package is actually available, since
# (unlike every other example above) it is the one example with a real external dependency.
find_package(SDL3 QUIET CONFIG)
if(SDL3_FOUND)
    add_executable(sdl3_gpu_accel_demo sdl3_gpu_accel_demo.cpp)
    target_link_libraries(sdl3_gpu_accel_demo PRIVATE jplcz_structo jplcz_structo_warnings SDL3::SDL3)
    list(APPEND JPLCZ_STRUCTO_EXAMPLE_TARGETS sdl3_gpu_accel_demo)

    # Optional: a windowed demo driving `hw::vt100_terminal` through `hw::console_ref` onto
    # `hw::framebuffer_console` (using the real `examples/fonts` DejaVu Sans Mono bitmap, not
    # `block_font_8x8`'s solid-block placeholder), presented via SDL3 the same way.
    add_executable(sdl3_vt100_framebuffer_console_demo sdl3_vt100_framebuffer_console_demo.cpp)
    target_link_libraries(sdl3_vt100_framebuffer_console_demo PRIVATE
        jplcz_structo jplcz_structo_warnings SDL3::SDL3)
    list(APPEND JPLCZ_STRUCTO_EXAMPLE_TARGETS sdl3_vt100_framebuffer_console_demo)

    # Optional: a windowed demo of the virtio-gpu device (`virtio_gpu_function`) driven by a tiny
    # in-process "guest" driver over a virtio-mmio transport, presented via SDL3.
    add_executable(sdl3_virtio_gpu_demo sdl3_virtio_gpu_demo.cpp)
    target_link_libraries(sdl3_virtio_gpu_demo PRIVATE jplcz_structo jplcz_structo_warnings SDL3::SDL3)
    list(APPEND JPLCZ_STRUCTO_EXAMPLE_TARGETS sdl3_virtio_gpu_demo)
else()
    message(STATUS "jplcz_structo: SDL3 not found, skipping sdl3_gpu_accel_demo/\
sdl3_vt100_framebuffer_console_demo/sdl3_virtio_gpu_demo examples")
endif()

add_custom_target(jplcz_structo_examples
    DEPENDS ${JPLCZ_STRUCTO_EXAMPLE_TARGETS}
)

# C++20-only Linux demo of bootldr::netstack over a pty/serial line (SLIP or PPP); POSIX only.
# cxx_std_20 means "at least C++20", so a newer global standard is kept.
if(UNIX)
    add_executable(netstack_linux_demo netstack_linux_demo.cpp)
    target_link_libraries(netstack_linux_demo PRIVATE jplcz_structo jplcz_structo_warnings)
    target_compile_features(netstack_linux_demo PRIVATE cxx_std_20)
    add_dependencies(jplcz_structo_examples netstack_linux_demo)

    # C++20-only terminal demo of bootldr::edit_text.
    add_executable(text_editor_linux_demo text_editor_linux_demo.cpp)
    target_link_libraries(text_editor_linux_demo PRIVATE jplcz_structo jplcz_structo_warnings)
    target_compile_features(text_editor_linux_demo PRIVATE cxx_std_20)
    add_dependencies(jplcz_structo_examples text_editor_linux_demo)

    # C++20-only terminal demo of bootldr::debug_menu.
    add_executable(debug_menu_linux_demo debug_menu_linux_demo.cpp)
    target_link_libraries(debug_menu_linux_demo PRIVATE jplcz_structo jplcz_structo_warnings)
    target_compile_features(debug_menu_linux_demo PRIVATE cxx_std_20)
    add_dependencies(jplcz_structo_examples debug_menu_linux_demo)

    # C++20-only terminal demo of the bootldr::shell with the generic command set.
    add_executable(shell_linux_demo shell_linux_demo.cpp)
    target_link_libraries(shell_linux_demo PRIVATE jplcz_structo jplcz_structo_warnings)
    target_compile_features(shell_linux_demo PRIVATE cxx_std_20)
    add_dependencies(jplcz_structo_examples shell_linux_demo)

    # Graphical terminal emulator spawning $SHELL on a pty, via input_device_ref + framebuffer_console +
    # console_uart; needs SDL3 like the other sdl3_* demos.
    if(SDL3_FOUND)
        add_executable(sdl3_shell_terminal_demo sdl3_shell_terminal_demo.cpp)
        target_link_libraries(sdl3_shell_terminal_demo PRIVATE jplcz_structo jplcz_structo_warnings SDL3::SDL3)
        find_library(JPLCZ_STRUCTO_UTIL_LIB util)
        if(JPLCZ_STRUCTO_UTIL_LIB)
            target_link_libraries(sdl3_shell_terminal_demo PRIVATE ${JPLCZ_STRUCTO_UTIL_LIB})
        endif()
        add_dependencies(jplcz_structo_examples sdl3_shell_terminal_demo)
    endif()
endif()
