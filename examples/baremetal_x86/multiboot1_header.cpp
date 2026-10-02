// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

/** @file multiboot1_header.cpp
 * @brief Embeds a classic Multiboot *1* header (magic `0x1BADB002`) in a
 * dedicated `.multiboot1` section, placed first by `linker.ld` -- and
 * therefore within the mandatory first-8192-byte search window Multiboot
 * 1 loaders require -- purely so `qemu-system-i386`'s own built-in
 * `-kernel` loader (which only ever implements Multiboot *1*, never *2*;
 * see `README.md`) can load this kernel directly, with no GRUB/ISO
 * detour needed.
 *
 * This is deliberately hand-rolled here rather than added to
 * `structo::arch::x86::multiboot2.hpp`: this library's Multiboot support
 * is intentionally Multiboot2-only (a materially different, tag-based
 * boot information format), and Multiboot 1 is legacy/QEMU-loader-only
 * at this point. `kmain.cpp` picks whichever boot information format
 * matches the magic value actually left in `eax` at entry.
 */

#include <cstdint>

namespace {

struct multiboot1_header {
  std::uint32_t magic;
  std::uint32_t flags;
  std::uint32_t checksum;
};

// bit 0: align all boot modules on page (4 KiB) boundaries.
// bit 1: request `mem_lower`/`mem_upper` (and, if the bootloader has it,
//        the richer BIOS memory map) in the boot information structure.
// Neither requires anything else from this kernel (no custom load
// addresses, no video mode request), so these two bits are sufficient.
constexpr std::uint32_t multiboot1_magic = 0x1BADB002;
constexpr std::uint32_t multiboot1_flags = 0x00000003;

// The spec requires magic + flags + checksum == 0 (mod 2^32); unsigned
// wraparound makes this plain negation exact, with no explicit modulo
// needed.
constexpr std::uint32_t multiboot1_checksum = static_cast<std::uint32_t>(0u - multiboot1_magic - multiboot1_flags);

} // namespace

extern "C" __attribute__((section(".multiboot1"), used)) alignas(4) constexpr multiboot1_header g_multiboot1_header = {
    multiboot1_magic, multiboot1_flags, multiboot1_checksum};
