// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

/** @file multiboot_header.cpp
 * @brief Embeds `structo::arch::x86::make_basic_header()`'s result in a
 * dedicated `.multiboot` section (placed first by `linker.ld`, well
 * within the first 32 KiB of the image), so a Multiboot2-compliant
 * bootloader (including QEMU's own built-in loader, invoked via
 * `-kernel`) recognizes and loads this kernel.
 */

#include <structo/arch/x86/multiboot2.hpp>

namespace {

struct multiboot_image {
  structo::arch::x86::multiboot2_header header;
  structo::arch::x86::multiboot2_end_tag end_tag;
};

[[nodiscard]] constexpr multiboot_image make_image() noexcept {
  auto [header, end_tag] = structo::arch::x86::make_basic_header();
  return multiboot_image{header, end_tag};
}

} // namespace

extern "C" __attribute__((section(".multiboot"), used)) alignas(8) constexpr multiboot_image g_multiboot_image =
    make_image();
