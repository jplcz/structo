# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

function(add_jplcz_structo_example NAME)
    add_executable(${NAME} ${NAME}.cpp)
    target_link_libraries(${NAME} PRIVATE jplcz_structo jplcz_structo_warnings)
endfunction()

add_jplcz_structo_example(boot_memory_map_demo)
add_jplcz_structo_example(device_tree_demo)

add_custom_target(jplcz_structo_examples
    DEPENDS
        boot_memory_map_demo
        device_tree_demo
)
