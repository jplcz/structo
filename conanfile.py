# SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
#
# SPDX-License-Identifier: BSD-2-Clause

import os

from conan import ConanFile
from conan.tools.files import copy


class JplczStructoConan(ConanFile):
    name = "jplcz_structo"
    version = "0.2.1"
    package_type = "header-library"
    license = "BSD-2-Clause"
    url = "https://github.com/jplcz/structo"
    homepage = "https://github.com/jplcz/structo"
    description = (
        "Header-only OS/kernel/hypervisor/trusted-boundary building "
        "blocks (Flattened Device Tree decoding, physical memory region "
        "management) built on jplcz_reloco and jplcz_microfmt"
    )
    topics = ("kernel", "hypervisor", "embedded", "devicetree", "header-only")

    exports_sources = "include/**", "LICENSE"
    no_copy_source = True

    def requirements(self):
        self.requires("jplcz_reloco/0.1.0")
        self.requires("jplcz_microfmt/0.1.0")

    def package(self):
        copy(
            self,
            "*",
            src=os.path.join(self.source_folder, "include"),
            dst=os.path.join(self.package_folder, "include"),
        )
        copy(
            self,
            "LICENSE",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "licenses"),
        )

    def package_id(self):
        self.info.clear()

    def package_info(self):
        self.cpp_info.bindirs = []
        self.cpp_info.libdirs = []
        self.cpp_info.set_property("cmake_file_name", "jplcz_structo")
        self.cpp_info.set_property(
            "cmake_target_name", "jplcz_structo::structo"
        )
