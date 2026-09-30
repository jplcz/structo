# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

set(SOURCE_PATH "${CURRENT_PORT_DIR}/../../../..")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DJPLCZ_STRUCTO_INSTALL=ON
        -DJPLCZ_STRUCTO_BUILD_TESTS=OFF
        -DJPLCZ_STRUCTO_BUILD_EXAMPLES=OFF
        -DJPLCZ_STRUCTO_BUILD_HEADER_CHECKS=OFF
        -DJPLCZ_STRUCTO_ENABLE_STRICT_WARNINGS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME jplcz_structo
    CONFIG_PATH share/cmake/jplcz_structo
)

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug"
    "${CURRENT_PACKAGES_DIR}/lib"
)
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(TOUCH "${CURRENT_PACKAGES_DIR}/share/${PORT}/usage-accurate")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
