// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file memory_kind.hpp
 * @brief `structo::boot::memory_kind`: the one protocol-neutral
 * classification every boot-protocol parser in `structo/boot/` (Limine,
 * UEFI, Linux x86 `e820`, ...) maps its own memory types onto, so a
 * kernel can fold any of them into a single `structo::boot_memory_map`
 * (via `boot_memory_map::try_add`) without caring which bootloader
 * produced the map.
 *
 * ## What counts as "installed RAM" vs "free"
 *
 * `boot_memory_map::full` is every byte of *RAM* the machine has, and
 * `boot_memory_map::free` is the subset a page allocator may hand out
 * *right now*. `is_ram(kind)` and `is_free(kind)` encode that split once
 * for every protocol:
 *
 * | kind | in `full` | in `free` | Notes |
 * |---|---|---|---|
 * | `usable` | yes | yes | ordinary free RAM |
 * | `bootloader_reclaimable` | yes | no | free once the kernel stops reading bootloader data (page tables,
 * `boot_params`, the memory map itself) | | `acpi_reclaimable` | yes | no | free once ACPI tables were parsed/copied |
 * | `acpi_nvs` | yes | no | firmware sleep state; never free |
 * | `kernel_and_modules` | yes | no | the loaded image, initrd and modules |
 * | `firmware_runtime` | yes | no | UEFI runtime services code/data |
 * | `mmio` | no | no | device/port space |
 * | `framebuffer` | no | no | scanout memory |
 * | `bad` | no | no | reported defective |
 * | `reserved` | no | no | anything else the firmware withholds |
 *
 * A kernel that wants the reclaimable kinds after early boot calls
 * `boot_memory_map::free.try_add()` for those ranges itself once it is done
 * with the bootloader's data; this header deliberately never frees them
 * implicitly.
 */

#include <cstdint>

namespace structo::boot {

/** @brief Protocol-neutral physical-memory classification; see the file
 * documentation for how each kind maps onto `boot_memory_map`. */
enum class memory_kind : uint8_t {
  usable,
  bootloader_reclaimable,
  acpi_reclaimable,
  acpi_nvs,
  kernel_and_modules,
  firmware_runtime,
  mmio,
  framebuffer,
  bad,
  reserved,
};

/** @brief Whether @p kind is installed RAM (belongs in `boot_memory_map::full`). */
[[nodiscard]] constexpr bool is_ram(memory_kind kind) noexcept {
  switch (kind) {
  case memory_kind::usable:
  case memory_kind::bootloader_reclaimable:
  case memory_kind::acpi_reclaimable:
  case memory_kind::acpi_nvs:
  case memory_kind::kernel_and_modules:
  case memory_kind::firmware_runtime:
    return true;
  case memory_kind::mmio:
  case memory_kind::framebuffer:
  case memory_kind::bad:
  case memory_kind::reserved:
    return false;
  }
  return false;
}

/** @brief Whether @p kind is immediately allocatable (belongs in `boot_memory_map::free`). */
[[nodiscard]] constexpr bool is_free(memory_kind kind) noexcept { return kind == memory_kind::usable; }

/** @brief Whether @p kind becomes allocatable once the kernel is done with
 * the firmware/bootloader data stored there. */
[[nodiscard]] constexpr bool is_reclaimable(memory_kind kind) noexcept {
  return kind == memory_kind::bootloader_reclaimable || kind == memory_kind::acpi_reclaimable;
}

} // namespace structo::boot
