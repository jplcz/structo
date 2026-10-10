// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file uefi.hpp
 * @brief UEFI (UEFI Specification 2.x) data-structure support for a kernel
 * or bootloader entered from firmware (directly as a PE/COFF EFI
 * application, via an EFI stub, or via a loader such as Limine/GRUB that
 * forwards the EFI memory map and system table): GUIDs, status codes,
 * the `EFI_MEMORY_DESCRIPTOR` array, the `EFI_SYSTEM_TABLE` header and
 * `EFI_CONFIGURATION_TABLE` array, and the GOP mode information.
 *
 * Everything here is a pure, bounds-checked parser over caller-owned
 * `reloco::span<const std::byte>` regions -- no firmware calls and no
 * raw pointers. Calling Boot Services (`GetMemoryMap`, `ExitBootServices`,
 * `LocateProtocol`, ...) is inherently an ABI call through function
 * pointers the firmware owns, so it stays the application's job: it
 * obtains the memory-map buffer from `GetMemoryMap()` and passes the
 * resulting bytes plus the returned `DescriptorSize` here.
 *
 * ## Key detail: descriptor stride
 *
 * `EFI_MEMORY_DESCRIPTOR` entries are laid out `DescriptorSize` bytes
 * apart, and `DescriptorSize` may be *larger* than the 40 bytes this
 * header decodes (firmware may append fields; the spec forbids iterating
 * with `sizeof(EFI_MEMORY_DESCRIPTOR)`). `memory_map_reader` therefore
 * always steps by the `descriptor_size` it is given.
 *
 * @code
 * // Directly after GetMemoryMap() succeeded: `map_bytes` covers the
 * // `MemoryMapSize` bytes the firmware wrote, `desc_size` is the
 * // DescriptorSize it returned.
 * auto it = structo::boot::uefi::memory_map_reader::try_create(map_bytes, desc_size);
 * structo::boot_memory_map<64> map;
 * for (auto d : *it) {
 *   if (!d)
 *     break;
 *   // Fold each descriptor into the protocol-neutral map. Page count is
 *   // in 4 KiB UEFI pages, converted to bytes by size_bytes().
 *   (void)map.try_add(d->kind(), d->physical_start, d->size_bytes());
 * }
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/iterator.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <structo/boot/memory_kind.hpp>
#include <structo/detail/boot_bytes.hpp>

namespace structo::boot::uefi {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

// ============================================================================
// GUIDs
// ============================================================================

/** @brief `EFI_GUID`: 16 bytes, fields little-endian in memory. */
struct guid {
  uint32_t data1 = 0;
  uint16_t data2 = 0;
  uint16_t data3 = 0;
  array<uint8_t, 8> data4{};

  [[nodiscard]] constexpr bool operator==(const guid &o) const noexcept {
    if (data1 != o.data1 || data2 != o.data2 || data3 != o.data3)
      return false;
    for (std::size_t i = 0; i < 8; ++i)
      if (data4[i] != o.data4[i])
        return false;
    return true;
  }
  [[nodiscard]] constexpr bool operator!=(const guid &o) const noexcept { return !(*this == o); }
};

/** @brief Decodes a `guid` from 16 little-endian bytes at @p offset. */
[[nodiscard]] inline result<guid> read_guid(span<const std::byte> region, std::size_t offset) noexcept {
  auto slice = region.try_subspan(offset, 16);
  if (!slice)
    return unexpected(error::out_of_bounds);
  guid g;
  g.data1 = boot_bytes::read_le_at<uint32_t>(*slice, 0).value_or(0);
  g.data2 = boot_bytes::read_le_at<uint16_t>(*slice, 4).value_or(0);
  g.data3 = boot_bytes::read_le_at<uint16_t>(*slice, 6).value_or(0);
  for (std::size_t i = 0; i < 8; ++i)
    g.data4[i] = boot_bytes::read_le_at<uint8_t>(*slice, 8 + i).value_or(0);
  return g;
}

inline constexpr guid acpi_20_table_guid = {
    0x8868e871, 0xe4f1, 0x11d3, {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
inline constexpr guid acpi_10_table_guid = {
    0xeb9d2d30, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
inline constexpr guid smbios_table_guid = {
    0xeb9d2d31, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
inline constexpr guid smbios3_table_guid = {
    0xf2fd1544, 0x9794, 0x4a2c, {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}};
/** @brief The flattened devicetree configuration table (`EFI_DTB_TABLE_GUID`), used on arm/arm64/riscv. */
inline constexpr guid dtb_table_guid = {0xb1b621d5, 0xf19c, 0x41a5, {0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0}};
inline constexpr guid memory_attributes_table_guid = {
    0xdcfa911d, 0x26eb, 0x469f, {0xa2, 0x20, 0x38, 0xb7, 0xdc, 0x46, 0x12, 0x20}};
inline constexpr guid rng_protocol_guid = {
    0x3152bca5, 0xeade, 0x433d, {0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44}};
/** @brief The Linux initrd media GUID (`LINUX_EFI_INITRD_MEDIA_GUID`), used by the EFI stub's initrd loading. */
inline constexpr guid linux_initrd_media_guid = {
    0x5568e427, 0x68fc, 0x4f3d, {0xac, 0x74, 0xca, 0x55, 0x52, 0x31, 0xcc, 0x68}};
inline constexpr guid graphics_output_protocol_guid = {
    0x9042a9de, 0x23dc, 0x4a38, {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
inline constexpr guid loaded_image_protocol_guid = {
    0x5b1b31a1, 0x9562, 0x11d2, {0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};

// ============================================================================
// Status codes
// ============================================================================

/** @brief `EFI_STATUS` is `UINTN` with the high bit marking errors. */
inline constexpr uint64_t status_error_bit_64 = 0x8000000000000000ull;
inline constexpr uint64_t status_error_bit_32 = 0x80000000ull;

/** @brief Common `EFI_STATUS` code numbers (without the error bit). */
enum class status_code : uint64_t {
  success = 0,
  load_error = 1,
  invalid_parameter = 2,
  unsupported = 3,
  bad_buffer_size = 4,
  buffer_too_small = 5,
  not_ready = 6,
  device_error = 7,
  write_protected = 8,
  out_of_resources = 9,
  volume_corrupted = 10,
  volume_full = 11,
  no_media = 12,
  media_changed = 13,
  not_found = 14,
  access_denied = 15,
  no_response = 16,
  timeout = 18,
  not_started = 19,
  already_started = 20,
  aborted = 21,
  security_violation = 26,
};

/** @brief Whether the raw `EFI_STATUS` @p status denotes an error (high bit set) on a firmware with
 * @p pointer_bits-bit `UINTN`. */
[[nodiscard]] constexpr bool status_is_error(uint64_t status, unsigned pointer_bits = 64) noexcept {
  return (status & (pointer_bits == 32 ? status_error_bit_32 : status_error_bit_64)) != 0;
}

/** @brief Maps a raw `EFI_STATUS` onto `reloco::error` (success -> success is not representable here:
 * returns `optional` empty on success, an error otherwise). */
[[nodiscard]] inline optional<error> status_to_error(uint64_t status, unsigned pointer_bits = 64) noexcept {
  if (!status_is_error(status, pointer_bits))
    return nullopt;
  const uint64_t code = status & ~(pointer_bits == 32 ? status_error_bit_32 : status_error_bit_64);
  switch (static_cast<status_code>(code)) {
  case status_code::invalid_parameter:
  case status_code::bad_buffer_size:
    return error::invalid_argument;
  case status_code::unsupported:
    return error::unsupported_operation;
  case status_code::buffer_too_small:
    return error::capacity_exceeded;
  case status_code::not_found:
    return error::not_found;
  case status_code::out_of_resources:
    return error::allocation_failed;
  case status_code::access_denied:
  case status_code::write_protected:
    return error::permission_denied;
  case status_code::security_violation:
    return error::security_violation;
  case status_code::timeout:
    return error::timed_out;
  case status_code::not_ready:
    return error::try_again;
  case status_code::aborted:
    return error::operation_canceled;
  default:
    return error::io_error;
  }
}

// ============================================================================
// Memory map
// ============================================================================

/** @brief `EFI_MEMORY_TYPE` values. */
enum class memory_type : uint32_t {
  reserved = 0,
  loader_code = 1,
  loader_data = 2,
  boot_services_code = 3,
  boot_services_data = 4,
  runtime_services_code = 5,
  runtime_services_data = 6,
  conventional = 7,
  unusable = 8,
  acpi_reclaim = 9,
  acpi_nvs = 10,
  memory_mapped_io = 11,
  memory_mapped_io_port_space = 12,
  pal_code = 13,
  persistent = 14,
  unaccepted = 15,
};

/** @brief `EFI_MEMORY_*` attribute bits. */
inline constexpr uint64_t memory_attr_uc = 0x1;
inline constexpr uint64_t memory_attr_wc = 0x2;
inline constexpr uint64_t memory_attr_wt = 0x4;
inline constexpr uint64_t memory_attr_wb = 0x8;
inline constexpr uint64_t memory_attr_wp = 0x1000;
inline constexpr uint64_t memory_attr_rp = 0x2000;
inline constexpr uint64_t memory_attr_xp = 0x4000;
inline constexpr uint64_t memory_attr_runtime = 0x8000000000000000ull;

/** @brief UEFI's page size is always 4 KiB regardless of the OS page size. */
inline constexpr uint64_t page_size = 4096;

/** @brief Maps a UEFI memory type onto the protocol-neutral `memory_kind`.
 *
 * Boot-services code/data and loader code/data are treated as RAM that is
 * *only* free after `ExitBootServices()` (`usable`); pass
 * @p boot_services_exited = false while firmware is still running so they
 * stay `bootloader_reclaimable`. */
[[nodiscard]] constexpr memory_kind to_memory_kind(memory_type type, bool boot_services_exited = true) noexcept {
  switch (type) {
  case memory_type::conventional:
    return memory_kind::usable;
  case memory_type::boot_services_code:
  case memory_type::boot_services_data:
    return boot_services_exited ? memory_kind::usable : memory_kind::bootloader_reclaimable;
  case memory_type::loader_code:
  case memory_type::loader_data:
    return memory_kind::bootloader_reclaimable;
  case memory_type::runtime_services_code:
  case memory_type::runtime_services_data:
    return memory_kind::firmware_runtime;
  case memory_type::acpi_reclaim:
    return memory_kind::acpi_reclaimable;
  case memory_type::acpi_nvs:
    return memory_kind::acpi_nvs;
  case memory_type::unusable:
    return memory_kind::bad;
  case memory_type::memory_mapped_io:
  case memory_type::memory_mapped_io_port_space:
    return memory_kind::mmio;
  case memory_type::persistent: // Not general-purpose RAM; keep it out of the allocator.
  case memory_type::reserved:
  case memory_type::pal_code:
  case memory_type::unaccepted: // Must be accepted (TDX/SEV-SNP) before use, so never free here.
    break;
  }
  return memory_kind::reserved;
}

/** @brief One decoded `EFI_MEMORY_DESCRIPTOR`. */
struct memory_descriptor {
  uint32_t type = 0;
  uint64_t physical_start = 0;
  uint64_t virtual_start = 0;
  uint64_t number_of_pages = 0;
  uint64_t attribute = 0;

  [[nodiscard]] constexpr uint64_t size_bytes() const noexcept { return number_of_pages * page_size; }
  [[nodiscard]] constexpr memory_kind kind(bool boot_services_exited = true) const noexcept {
    return to_memory_kind(static_cast<memory_type>(type), boot_services_exited);
  }
};

/** @brief Minimum bytes of one `EFI_MEMORY_DESCRIPTOR` this reader decodes (`Type`, padding, 4x `UINT64`). */
inline constexpr std::size_t memory_descriptor_min_size = 40;

/** @brief Iterates an `EFI_MEMORY_DESCRIPTOR` array, stepping by the firmware-reported descriptor size. */
class RELOCO_POINTER memory_map_reader : public iterator_adaptor<memory_map_reader, result<memory_descriptor>> {
public:
  using item_type = result<memory_descriptor>;

  /** @brief Validates @p descriptor_size (`>= 40`, and a multiple of 8 as the spec's 8-byte alignment implies)
   * and that @p map is a whole number of descriptors. */
  [[nodiscard]] static result<memory_map_reader> try_create(span<const std::byte> map,
                                                            std::size_t descriptor_size) noexcept {
    if (descriptor_size < memory_descriptor_min_size || descriptor_size % 8 != 0)
      return unexpected(error::invalid_argument);
    if (map.size() % descriptor_size != 0)
      return unexpected(error::invalid_argument);
    return memory_map_reader(map, descriptor_size);
  }

  [[nodiscard]] std::size_t count() const noexcept { return map_.size() / stride_; }

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (cursor_ >= map_.size())
      return nullopt;
    auto slice = map_.try_subspan(cursor_, stride_);
    cursor_ += stride_;
    if (!slice)
      return optional<item_type>(item_type(unexpected(error::out_of_bounds)));
    memory_descriptor d;
    auto type = boot_bytes::read_le_at<uint32_t>(*slice, 0);
    auto phys = boot_bytes::read_le_at<uint64_t>(*slice, 8);
    auto virt = boot_bytes::read_le_at<uint64_t>(*slice, 16);
    auto pages = boot_bytes::read_le_at<uint64_t>(*slice, 24);
    auto attr = boot_bytes::read_le_at<uint64_t>(*slice, 32);
    if (!type || !phys || !virt || !pages || !attr)
      return optional<item_type>(item_type(unexpected(error::out_of_bounds)));
    d.type = *type;
    d.physical_start = *phys;
    d.virtual_start = *virt;
    d.number_of_pages = *pages;
    d.attribute = *attr;
    return optional<item_type>(item_type(d));
  }

private:
  memory_map_reader(span<const std::byte> map, std::size_t stride) noexcept : map_(map), stride_(stride) {}
  span<const std::byte> map_;
  std::size_t stride_;
  std::size_t cursor_ = 0;
};

/** @brief Folds every descriptor of @p reader into @p map (`boot_memory_map`-shaped: needs
 * `try_add(memory_kind, base, size)`). */
template <typename Map>
[[nodiscard]] result<void> try_fill_memory_map(memory_map_reader reader, Map &map,
                                               bool boot_services_exited = true) noexcept {
  for (auto d : reader) {
    if (!d)
      return unexpected(d.error());
    if (auto r = map.try_add(d->kind(boot_services_exited), d->physical_start, d->size_bytes()); !r)
      return r;
  }
  return {};
}

// ============================================================================
// System table / configuration tables
// ============================================================================

/** @brief `EFI_SYSTEM_TABLE_SIGNATURE` ("IBI SYST"). */
inline constexpr uint64_t system_table_signature = 0x5453595320494249ull;

/** @brief Decoded `EFI_TABLE_HEADER` + the fields of `EFI_SYSTEM_TABLE` a kernel needs. */
struct system_table_info {
  uint32_t revision = 0;
  uint32_t header_size = 0;
  uint64_t number_of_table_entries = 0;
  /** @brief Address of the `EFI_CONFIGURATION_TABLE` array (resolve it yourself; see `config_table_reader`). */
  uint64_t configuration_table = 0;
  /** @brief Address of the `EFI_RUNTIME_SERVICES` table. */
  uint64_t runtime_services = 0;
  /** @brief Address of the `EFI_BOOT_SERVICES` table. */
  uint64_t boot_services = 0;
  [[nodiscard]] constexpr uint16_t major() const noexcept { return static_cast<uint16_t>(revision >> 16); }
  [[nodiscard]] constexpr uint16_t minor() const noexcept { return static_cast<uint16_t>(revision & 0xFFFF); }
};

/** @brief Decodes a 64-bit `EFI_SYSTEM_TABLE` held in @p table (at least 120 bytes), validating the signature. */
[[nodiscard]] inline result<system_table_info> try_decode_system_table64(span<const std::byte> table) noexcept {
  auto sig = boot_bytes::read_le_at<uint64_t>(table, 0);
  if (!sig)
    return unexpected(sig.error());
  if (*sig != system_table_signature)
    return unexpected(error::invalid_argument);
  auto revision = boot_bytes::read_le_at<uint32_t>(table, 8);
  auto header_size = boot_bytes::read_le_at<uint32_t>(table, 12);
  auto runtime = boot_bytes::read_le_at<uint64_t>(table, 88);
  auto boot = boot_bytes::read_le_at<uint64_t>(table, 96);
  auto entries = boot_bytes::read_le_at<uint64_t>(table, 104);
  auto config = boot_bytes::read_le_at<uint64_t>(table, 112);
  if (!revision || !header_size || !runtime || !boot || !entries || !config)
    return unexpected(error::out_of_bounds);
  system_table_info info;
  info.revision = *revision;
  info.header_size = *header_size;
  info.runtime_services = *runtime;
  info.boot_services = *boot;
  info.number_of_table_entries = *entries;
  info.configuration_table = *config;
  return info;
}

/** @brief One `EFI_CONFIGURATION_TABLE` entry: a GUID and the (physical) address of its vendor table. */
struct config_table_entry {
  guid vendor_guid{};
  uint64_t vendor_table = 0;
};

/** @brief Iterates an `EFI_CONFIGURATION_TABLE` array (24-byte entries for a 64-bit firmware, 20 for 32-bit). */
class RELOCO_POINTER config_table_reader : public iterator_adaptor<config_table_reader, result<config_table_entry>> {
public:
  using item_type = result<config_table_entry>;

  /** @brief @p pointer_bits is 32 or 64 (the firmware's `UINTN` width). */
  [[nodiscard]] static result<config_table_reader> try_create(span<const std::byte> table,
                                                              unsigned pointer_bits = 64) noexcept {
    if (pointer_bits != 32 && pointer_bits != 64)
      return unexpected(error::invalid_argument);
    const std::size_t stride = pointer_bits == 64 ? 24 : 20;
    if (table.size() % stride != 0)
      return unexpected(error::invalid_argument);
    return config_table_reader(table, stride);
  }

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (cursor_ >= table_.size())
      return nullopt;
    auto slice = table_.try_subspan(cursor_, stride_);
    cursor_ += stride_;
    if (!slice)
      return optional<item_type>(item_type(unexpected(error::out_of_bounds)));
    auto g = read_guid(*slice, 0);
    if (!g)
      return optional<item_type>(item_type(unexpected(g.error())));
    config_table_entry e;
    e.vendor_guid = *g;
    if (stride_ == 24) {
      auto p = boot_bytes::read_le_at<uint64_t>(*slice, 16);
      if (!p)
        return optional<item_type>(item_type(unexpected(p.error())));
      e.vendor_table = *p;
    } else {
      auto p = boot_bytes::read_le_at<uint32_t>(*slice, 16);
      if (!p)
        return optional<item_type>(item_type(unexpected(p.error())));
      e.vendor_table = *p;
    }
    return optional<item_type>(item_type(e));
  }

private:
  config_table_reader(span<const std::byte> table, std::size_t stride) noexcept : table_(table), stride_(stride) {}
  span<const std::byte> table_;
  std::size_t stride_;
  std::size_t cursor_ = 0;
};

/** @brief Finds @p wanted in @p table and returns its vendor-table address; `error::not_found` if absent. */
[[nodiscard]] inline result<uint64_t> try_find_config_table(config_table_reader reader, const guid &wanted) noexcept {
  for (auto e : reader) {
    if (!e)
      return unexpected(e.error());
    if (e->vendor_guid == wanted)
      return e->vendor_table;
  }
  return unexpected(error::not_found);
}

// ============================================================================
// GOP
// ============================================================================

/** @brief `EFI_GRAPHICS_PIXEL_FORMAT`. */
enum class gop_pixel_format : uint32_t {
  rgb_reserved_8bit = 0,
  bgr_reserved_8bit = 1,
  bit_mask = 2,
  blt_only = 3,
};

/** @brief Decoded `EFI_GRAPHICS_OUTPUT_MODE_INFORMATION` (36 bytes). */
struct gop_mode_info {
  uint32_t version = 0;
  uint32_t horizontal_resolution = 0;
  uint32_t vertical_resolution = 0;
  gop_pixel_format pixel_format = gop_pixel_format::blt_only;
  uint32_t red_mask = 0;
  uint32_t green_mask = 0;
  uint32_t blue_mask = 0;
  uint32_t reserved_mask = 0;
  uint32_t pixels_per_scan_line = 0;

  /** @brief Whether the mode exposes a linear framebuffer (`blt_only` modes do not). */
  [[nodiscard]] constexpr bool has_linear_framebuffer() const noexcept {
    return pixel_format != gop_pixel_format::blt_only;
  }
  /** @brief Bytes per scan line; every non-`blt_only` GOP format is 32 bits per pixel. */
  [[nodiscard]] constexpr uint64_t pitch_bytes() const noexcept {
    return static_cast<uint64_t>(pixels_per_scan_line) * 4;
  }
};

[[nodiscard]] inline result<gop_mode_info> try_decode_gop_mode_info(span<const std::byte> info) noexcept {
  gop_mode_info g;
  auto version = boot_bytes::read_le_at<uint32_t>(info, 0);
  auto h = boot_bytes::read_le_at<uint32_t>(info, 4);
  auto v = boot_bytes::read_le_at<uint32_t>(info, 8);
  auto fmt = boot_bytes::read_le_at<uint32_t>(info, 12);
  auto r = boot_bytes::read_le_at<uint32_t>(info, 16);
  auto gr = boot_bytes::read_le_at<uint32_t>(info, 20);
  auto b = boot_bytes::read_le_at<uint32_t>(info, 24);
  auto res = boot_bytes::read_le_at<uint32_t>(info, 28);
  auto ppsl = boot_bytes::read_le_at<uint32_t>(info, 32);
  if (!version || !h || !v || !fmt || !r || !gr || !b || !res || !ppsl)
    return unexpected(error::out_of_bounds);
  if (*fmt > static_cast<uint32_t>(gop_pixel_format::blt_only))
    return unexpected(error::invalid_argument);
  g.version = *version;
  g.horizontal_resolution = *h;
  g.vertical_resolution = *v;
  g.pixel_format = static_cast<gop_pixel_format>(*fmt);
  g.red_mask = *r;
  g.green_mask = *gr;
  g.blue_mask = *b;
  g.reserved_mask = *res;
  g.pixels_per_scan_line = *ppsl;
  return g;
}

} // namespace structo::boot::uefi
