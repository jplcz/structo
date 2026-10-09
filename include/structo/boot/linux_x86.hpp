// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file linux_x86.hpp
 * @brief The Linux/x86 boot protocol (Documentation/arch/x86/boot.rst), for
 * both i386 and x86_64 kernels: a decoder for the `setup_header` embedded
 * in a bzImage, a reader for the `boot_params` ("zero page") a loader
 * hands to the kernel, and a writer to build one when *structo is the
 * loader*.
 *
 * ## Two roles
 *
 * - **Loader role** (structo-based bootloader launching Linux):
 *   `try_parse_setup_header()` validates the bzImage, tells you where the
 *   protected-mode kernel starts, whether it is a 64-bit kernel, and which
 *   entry points exist; `boot_params_writer` then fills the zero page
 *   (command line, initrd, e820, EFI info, framebuffer). Entry:
 *   - 32-bit: jump to `code32_start` in flat protected mode with
 *     `%esi` = physical address of the zero page, `%ebp = %edi = %ebx = 0`.
 *   - 64-bit (boot protocol >= 2.12, `xloadflags` bit 0): jump to
 *     `code32_start + 0x200` in long mode (identity mapped) with
 *     `%rsi` = zero page.
 * - **Kernel role** (a structo kernel/hypervisor entered by a Linux-
 *   protocol loader, e.g. QEMU `-kernel`, kexec, GRUB `linux`):
 *   `boot_params_reader` decodes the zero page at `%esi`/`%rsi`.
 *
 * Every field is little-endian and every access is bounds-checked against
 * the caller-owned span; no pointers are formed anywhere.
 *
 * @code
 * // Loader role. `bzimage` is the kernel file image, `zero_page` a
 * // caller-owned, writable, 4096-byte, page-aligned buffer.
 * auto hdr = structo::boot::linux_x86::try_parse_setup_header(bzimage);
 * if (!hdr || !hdr->is_64bit())
 *   return;
 * structo::boot::linux_x86::boot_params_writer bp(zero_page);
 * (void)bp.init_from_setup_header(bzimage);              // copies the setup header in, as the protocol requires
 * (void)bp.set_loader(0xFF, 0);                          // type_of_loader 0xFF = "undefined loader"
 * (void)bp.set_cmdline(cmdline_phys_addr);               // physical address of the NUL-terminated cmdline
 * (void)bp.set_ramdisk(initrd_phys_addr, initrd_size);   // physical location of the initrd
 * (void)bp.add_e820(0x100000, 0x7ff00000,                // base, size
 *                   structo::boot::linux_x86::e820_type::ram);
 * // Then jump to code32_start + 0x200 with %rsi = physical address of zero_page.
 *
 * // Kernel role: decode what the loader gave us.
 * auto params = structo::boot::linux_x86::boot_params_reader::try_create(zero_page_bytes);
 * structo::boot_memory_map<64> map;
 * (void)params->try_fill_memory_map(map);
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <structo/boot/memory_kind.hpp>
#include <structo/detail/boot_bytes.hpp>

namespace structo::boot::linux_x86 {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

/** @brief Size of the zero page (`struct boot_params`). */
inline constexpr std::size_t boot_params_size = 4096;
/** @brief Offset of `setup_header` inside the zero page and inside a bzImage. */
inline constexpr std::size_t setup_header_offset = 0x1F1;
/** @brief `boot_flag` value (`0xAA55`) at offset 0x1FE. */
inline constexpr uint16_t boot_flag_magic = 0xAA55;
/** @brief `header` field value ("HdrS") at offset 0x202. */
inline constexpr uint32_t header_magic = 0x53726448;
/** @brief Maximum number of entries in the zero page's embedded e820 table. */
inline constexpr std::size_t e820_max_entries = 128;
/** @brief Offset, within a 64-bit kernel's protected-mode image, of the 64-bit entry point. */
inline constexpr std::size_t entry64_offset = 0x200;

/** @brief Packs a boot protocol version as `major << 8 | minor`, e.g. `version(2, 15)`. */
[[nodiscard]] constexpr uint16_t version(unsigned major, unsigned minor) noexcept {
  return static_cast<uint16_t>((major << 8) | minor);
}

/** @brief `loadflags` bits. */
inline constexpr uint8_t loadflag_loaded_high = 1u << 0;
inline constexpr uint8_t loadflag_kaslr = 1u << 1;
inline constexpr uint8_t loadflag_quiet = 1u << 5;
inline constexpr uint8_t loadflag_keep_segments = 1u << 6;
inline constexpr uint8_t loadflag_can_use_heap = 1u << 7;

/** @brief `xloadflags` bits (boot protocol 2.12+). */
inline constexpr uint16_t xlf_kernel_64 = 1u << 0;
inline constexpr uint16_t xlf_can_be_loaded_above_4g = 1u << 1;
inline constexpr uint16_t xlf_efi_handover_32 = 1u << 2;
inline constexpr uint16_t xlf_efi_handover_64 = 1u << 3;
inline constexpr uint16_t xlf_efi_kexec = 1u << 4;
inline constexpr uint16_t xlf_5level = 1u << 5;
inline constexpr uint16_t xlf_5level_enabled = 1u << 6;
inline constexpr uint16_t xlf_mem_encryption = 1u << 7;

// ============================================================================
// setup_header
// ============================================================================

/** @brief Decoded `struct setup_header` (fields beyond the image's declared `version` read as 0). */
struct setup_header {
  uint8_t setup_sects = 0;
  uint16_t root_flags = 0;
  uint32_t syssize = 0;
  uint16_t boot_flag = 0;
  uint32_t header = 0;
  uint16_t version = 0;
  uint8_t type_of_loader = 0;
  uint8_t loadflags = 0;
  uint32_t code32_start = 0;
  uint32_t ramdisk_image = 0;
  uint32_t ramdisk_size = 0;
  uint32_t cmd_line_ptr = 0;
  uint32_t initrd_addr_max = 0;
  uint32_t kernel_alignment = 0;
  uint8_t relocatable_kernel = 0;
  uint8_t min_alignment = 0;
  uint16_t xloadflags = 0;
  uint32_t cmdline_size = 0;
  uint32_t hardware_subarch = 0;
  uint64_t hardware_subarch_data = 0;
  uint32_t payload_offset = 0;
  uint32_t payload_length = 0;
  uint64_t setup_data = 0;
  uint64_t pref_address = 0;
  uint32_t init_size = 0;
  uint32_t handover_offset = 0;
  uint32_t kernel_info_offset = 0;

  /** @brief Number of 512-byte real-mode setup sectors; 0 means 4 per the spec. */
  [[nodiscard]] constexpr uint32_t effective_setup_sects() const noexcept { return setup_sects == 0 ? 4u : setup_sects; }
  /** @brief Byte offset in the bzImage file where the protected-mode kernel begins (boot sector + setup). */
  [[nodiscard]] constexpr uint64_t protected_mode_offset() const noexcept {
    return (static_cast<uint64_t>(effective_setup_sects()) + 1) * 512;
  }
  /** @brief Size in bytes of the protected-mode kernel (`syssize` is in 16-byte paragraphs). */
  [[nodiscard]] constexpr uint64_t protected_mode_size() const noexcept { return static_cast<uint64_t>(syssize) * 16; }
  /** @brief Whether the image is a 64-bit kernel with a 64-bit entry point (protocol >= 2.12). */
  [[nodiscard]] constexpr bool is_64bit() const noexcept {
    return version >= linux_x86::version(2, 12) && (xloadflags & xlf_kernel_64) != 0;
  }
  [[nodiscard]] constexpr bool is_relocatable() const noexcept {
    return version >= linux_x86::version(2, 5) && relocatable_kernel != 0;
  }
  [[nodiscard]] constexpr bool can_load_above_4g() const noexcept { return (xloadflags & xlf_can_be_loaded_above_4g) != 0; }
  /** @brief Physical address to jump to for a 64-bit entry, given where the protected-mode kernel was loaded. */
  [[nodiscard]] constexpr uint64_t entry64(uint64_t loaded_at) const noexcept { return loaded_at + entry64_offset; }
  /** @brief Maximum command line length the kernel accepts (protocol >= 2.06; 255 before). */
  [[nodiscard]] constexpr uint32_t max_cmdline_size() const noexcept {
    return version >= linux_x86::version(2, 6) ? cmdline_size : 255u;
  }
};

/** @brief Decodes the setup header from the start of a bzImage (or from a zero page), validating `boot_flag`
 * and the "HdrS" magic and requiring protocol >= 2.02 (the oldest one this decoder supports). */
[[nodiscard]] inline result<setup_header> try_parse_setup_header(span<const std::byte> image) noexcept {
  setup_header h;
  auto boot_flag = boot_bytes::read_le_at<uint16_t>(image, 0x1FE);
  auto magic = boot_bytes::read_le_at<uint32_t>(image, 0x202);
  auto ver = boot_bytes::read_le_at<uint16_t>(image, 0x206);
  if (!boot_flag || !magic || !ver)
    return unexpected(error::out_of_bounds);
  if (*boot_flag != boot_flag_magic || *magic != header_magic)
    return unexpected(error::invalid_argument);
  if (*ver < version(2, 2))
    return unexpected(error::unsupported_operation);
  h.boot_flag = *boot_flag;
  h.header = *magic;
  h.version = *ver;
  h.setup_sects = boot_bytes::read_le_at<uint8_t>(image, 0x1F1).value_or(0);
  h.root_flags = boot_bytes::read_le_at<uint16_t>(image, 0x1F2).value_or(0);
  h.syssize = boot_bytes::read_le_at<uint32_t>(image, 0x1F4).value_or(0);
  h.type_of_loader = boot_bytes::read_le_at<uint8_t>(image, 0x210).value_or(0);
  h.loadflags = boot_bytes::read_le_at<uint8_t>(image, 0x211).value_or(0);
  h.code32_start = boot_bytes::read_le_at<uint32_t>(image, 0x214).value_or(0);
  h.ramdisk_image = boot_bytes::read_le_at<uint32_t>(image, 0x218).value_or(0);
  h.ramdisk_size = boot_bytes::read_le_at<uint32_t>(image, 0x21C).value_or(0);
  h.cmd_line_ptr = boot_bytes::read_le_at<uint32_t>(image, 0x228).value_or(0);
  if (*ver >= version(2, 3))
    h.initrd_addr_max = boot_bytes::read_le_at<uint32_t>(image, 0x22C).value_or(0);
  if (*ver >= version(2, 5)) {
    h.kernel_alignment = boot_bytes::read_le_at<uint32_t>(image, 0x230).value_or(0);
    h.relocatable_kernel = boot_bytes::read_le_at<uint8_t>(image, 0x234).value_or(0);
  }
  if (*ver >= version(2, 10))
    h.min_alignment = boot_bytes::read_le_at<uint8_t>(image, 0x235).value_or(0);
  if (*ver >= version(2, 12))
    h.xloadflags = boot_bytes::read_le_at<uint16_t>(image, 0x236).value_or(0);
  if (*ver >= version(2, 6))
    h.cmdline_size = boot_bytes::read_le_at<uint32_t>(image, 0x238).value_or(0);
  if (*ver >= version(2, 7)) {
    h.hardware_subarch = boot_bytes::read_le_at<uint32_t>(image, 0x23C).value_or(0);
    h.hardware_subarch_data = boot_bytes::read_le_at<uint64_t>(image, 0x240).value_or(0);
  }
  if (*ver >= version(2, 8)) {
    h.payload_offset = boot_bytes::read_le_at<uint32_t>(image, 0x248).value_or(0);
    h.payload_length = boot_bytes::read_le_at<uint32_t>(image, 0x24C).value_or(0);
  }
  if (*ver >= version(2, 9))
    h.setup_data = boot_bytes::read_le_at<uint64_t>(image, 0x250).value_or(0);
  if (*ver >= version(2, 10))
    h.pref_address = boot_bytes::read_le_at<uint64_t>(image, 0x258).value_or(0);
  if (*ver >= version(2, 10))
    h.init_size = boot_bytes::read_le_at<uint32_t>(image, 0x260).value_or(0);
  if (*ver >= version(2, 11))
    h.handover_offset = boot_bytes::read_le_at<uint32_t>(image, 0x264).value_or(0);
  if (*ver >= version(2, 15))
    h.kernel_info_offset = boot_bytes::read_le_at<uint32_t>(image, 0x268).value_or(0);
  return h;
}

// ============================================================================
// e820
// ============================================================================

/** @brief `E820_*` range types. */
enum class e820_type : uint32_t {
  ram = 1,
  reserved = 2,
  acpi = 3,
  nvs = 4,
  unusable = 5,
  pmem = 7,
  pram = 12,
  soft_reserved = 0xefffffff,
};

/** @brief Maps an e820 type onto the protocol-neutral `memory_kind`. */
[[nodiscard]] constexpr memory_kind to_memory_kind(uint32_t type) noexcept {
  switch (static_cast<e820_type>(type)) {
  case e820_type::ram:
    return memory_kind::usable;
  case e820_type::acpi:
    return memory_kind::acpi_reclaimable;
  case e820_type::nvs:
    return memory_kind::acpi_nvs;
  case e820_type::unusable:
    return memory_kind::bad;
  case e820_type::reserved:
  case e820_type::pmem:
  case e820_type::pram:
  case e820_type::soft_reserved:
    break;
  }
  return memory_kind::reserved;
}

/** @brief One decoded e820 entry (20 bytes in the zero page). */
struct e820_entry {
  uint64_t addr = 0;
  uint64_t size = 0;
  uint32_t type = 0;
  [[nodiscard]] constexpr memory_kind kind() const noexcept { return to_memory_kind(type); }
};

// ============================================================================
// Zero page offsets
// ============================================================================

namespace zp {
inline constexpr std::size_t screen_info = 0x000;
inline constexpr std::size_t ext_ramdisk_image = 0x0C0;
inline constexpr std::size_t ext_ramdisk_size = 0x0C4;
inline constexpr std::size_t ext_cmd_line_ptr = 0x0C8;
inline constexpr std::size_t efi_info = 0x1C0;
inline constexpr std::size_t e820_entries = 0x1E8;
inline constexpr std::size_t secure_boot = 0x1EC;
inline constexpr std::size_t e820_table = 0x2D0;
inline constexpr std::size_t e820_entry_size = 20;
} // namespace zp

/** @brief `efi_loader_signature` values: "EL32" / "EL64". */
inline constexpr uint32_t efi_loader_signature_32 = 0x32334C45;
inline constexpr uint32_t efi_loader_signature_64 = 0x34364C45;

/** @brief Decoded `struct efi_info`: how the loader handed over UEFI state. */
struct efi_info {
  uint32_t loader_signature = 0;
  uint64_t systab = 0;
  uint32_t memdesc_size = 0;
  uint32_t memdesc_version = 0;
  uint64_t memmap = 0;
  uint32_t memmap_size = 0;
  [[nodiscard]] constexpr bool is_64bit() const noexcept { return loader_signature == efi_loader_signature_64; }
  [[nodiscard]] constexpr bool valid() const noexcept {
    return loader_signature == efi_loader_signature_32 || loader_signature == efi_loader_signature_64;
  }
};

/** @brief Decoded `struct screen_info` fields a framebuffer console needs. */
struct screen_info {
  uint8_t orig_video_mode = 0;
  uint8_t orig_video_cols = 0;
  uint8_t orig_video_lines = 0;
  uint8_t orig_video_is_vga = 0;
  uint16_t lfb_width = 0;
  uint16_t lfb_height = 0;
  uint16_t lfb_depth = 0;
  uint64_t lfb_base = 0;
  uint32_t lfb_size = 0;
  uint16_t lfb_linelength = 0;
  uint8_t red_size = 0, red_pos = 0;
  uint8_t green_size = 0, green_pos = 0;
  uint8_t blue_size = 0, blue_pos = 0;
  uint8_t rsvd_size = 0, rsvd_pos = 0;

  /** @brief `orig_video_isVGA` value for a linear UEFI GOP framebuffer. */
  static constexpr uint8_t video_type_efi = 0x70;
  /** @brief `orig_video_isVGA` value for a VESA linear framebuffer. */
  static constexpr uint8_t video_type_vlfb = 0x23;
  [[nodiscard]] constexpr bool has_linear_framebuffer() const noexcept {
    return (orig_video_is_vga == video_type_efi || orig_video_is_vga == video_type_vlfb) && lfb_base != 0;
  }
};

/** @brief A `[base, base + size)` physical range. */
struct memory_range {
  uint64_t base = 0;
  uint64_t size = 0;
};

// ============================================================================
// Kernel role: reader
// ============================================================================

/** @brief Bounds-checked, read-only view of a zero page. */
class boot_params_reader {
public:
  /** @brief @p page must cover at least the zero page's 4096 bytes. */
  [[nodiscard]] static result<boot_params_reader> try_create(span<const std::byte> page) noexcept {
    if (page.size() < boot_params_size)
      return unexpected(error::out_of_bounds);
    return boot_params_reader(page.first(boot_params_size));
  }

  [[nodiscard]] result<setup_header> try_header() const noexcept { return try_parse_setup_header(page_); }

  [[nodiscard]] uint8_t e820_count() const noexcept {
    return boot_bytes::read_le_at<uint8_t>(page_, zp::e820_entries).value_or(0);
  }

  /** @brief Entry @p index of the zero page's embedded e820 table. */
  [[nodiscard]] result<e820_entry> try_e820(std::size_t index) const noexcept {
    if (index >= e820_count() || index >= e820_max_entries)
      return unexpected(error::out_of_bounds);
    const std::size_t off = zp::e820_table + index * zp::e820_entry_size;
    auto addr = boot_bytes::read_le_at<uint64_t>(page_, off);
    auto size = boot_bytes::read_le_at<uint64_t>(page_, off + 8);
    auto type = boot_bytes::read_le_at<uint32_t>(page_, off + 16);
    if (!addr || !size || !type)
      return unexpected(error::out_of_bounds);
    return e820_entry{*addr, *size, *type};
  }

  /** @brief Folds the e820 table into @p map (anything with `try_add(memory_kind, base, size)`, e.g.
   * `structo::boot_memory_map`). */
  template <typename Map> [[nodiscard]] result<void> try_fill_memory_map(Map &map) const noexcept {
    for (std::size_t i = 0; i < e820_count(); ++i) {
      auto e = try_e820(i);
      if (!e)
        return unexpected(e.error());
      if (auto r = map.try_add(e->kind(), e->addr, e->size); !r)
        return r;
    }
    return {};
  }

  /** @brief Command line address, combining `cmd_line_ptr` with `ext_cmd_line_ptr` (the high 32 bits). */
  [[nodiscard]] result<uint64_t> try_cmdline_address() const noexcept {
    auto h = try_header();
    if (!h)
      return unexpected(h.error());
    const uint64_t high = boot_bytes::read_le_at<uint32_t>(page_, zp::ext_cmd_line_ptr).value_or(0);
    return (high << 32) | h->cmd_line_ptr;
  }

  /** @brief Initrd physical address and size, combining the `ext_ramdisk_*` high halves. */
  [[nodiscard]] result<memory_range> try_ramdisk() const noexcept {
    auto h = try_header();
    if (!h)
      return unexpected(h.error());
    const uint64_t image_hi = boot_bytes::read_le_at<uint32_t>(page_, zp::ext_ramdisk_image).value_or(0);
    const uint64_t size_hi = boot_bytes::read_le_at<uint32_t>(page_, zp::ext_ramdisk_size).value_or(0);
    return memory_range{(image_hi << 32) | h->ramdisk_image, (size_hi << 32) | h->ramdisk_size};
  }

  [[nodiscard]] result<efi_info> try_efi_info() const noexcept {
    efi_info e;
    auto sig = boot_bytes::read_le_at<uint32_t>(page_, zp::efi_info);
    auto systab = boot_bytes::read_le_at<uint32_t>(page_, zp::efi_info + 4);
    auto desc_size = boot_bytes::read_le_at<uint32_t>(page_, zp::efi_info + 8);
    auto desc_ver = boot_bytes::read_le_at<uint32_t>(page_, zp::efi_info + 12);
    auto memmap = boot_bytes::read_le_at<uint32_t>(page_, zp::efi_info + 16);
    auto memmap_size = boot_bytes::read_le_at<uint32_t>(page_, zp::efi_info + 20);
    auto systab_hi = boot_bytes::read_le_at<uint32_t>(page_, zp::efi_info + 24);
    auto memmap_hi = boot_bytes::read_le_at<uint32_t>(page_, zp::efi_info + 28);
    if (!sig || !systab || !desc_size || !desc_ver || !memmap || !memmap_size || !systab_hi || !memmap_hi)
      return unexpected(error::out_of_bounds);
    e.loader_signature = *sig;
    e.systab = (static_cast<uint64_t>(*systab_hi) << 32) | *systab;
    e.memdesc_size = *desc_size;
    e.memdesc_version = *desc_ver;
    e.memmap = (static_cast<uint64_t>(*memmap_hi) << 32) | *memmap;
    e.memmap_size = *memmap_size;
    return e;
  }

  [[nodiscard]] result<screen_info> try_screen_info() const noexcept {
    namespace b = boot_bytes;
    screen_info s;
    s.orig_video_mode = b::read_le_at<uint8_t>(page_, 6).value_or(0);
    s.orig_video_cols = b::read_le_at<uint8_t>(page_, 7).value_or(0);
    s.orig_video_lines = b::read_le_at<uint8_t>(page_, 14).value_or(0);
    s.orig_video_is_vga = b::read_le_at<uint8_t>(page_, 15).value_or(0);
    s.lfb_width = b::read_le_at<uint16_t>(page_, 18).value_or(0);
    s.lfb_height = b::read_le_at<uint16_t>(page_, 20).value_or(0);
    s.lfb_depth = b::read_le_at<uint16_t>(page_, 22).value_or(0);
    const uint64_t base_lo = b::read_le_at<uint32_t>(page_, 24).value_or(0);
    const uint64_t base_hi = b::read_le_at<uint32_t>(page_, 58).value_or(0);
    s.lfb_base = (base_hi << 32) | base_lo;
    s.lfb_size = b::read_le_at<uint32_t>(page_, 28).value_or(0);
    s.lfb_linelength = b::read_le_at<uint16_t>(page_, 36).value_or(0);
    s.red_size = b::read_le_at<uint8_t>(page_, 38).value_or(0);
    s.red_pos = b::read_le_at<uint8_t>(page_, 39).value_or(0);
    s.green_size = b::read_le_at<uint8_t>(page_, 40).value_or(0);
    s.green_pos = b::read_le_at<uint8_t>(page_, 41).value_or(0);
    s.blue_size = b::read_le_at<uint8_t>(page_, 42).value_or(0);
    s.blue_pos = b::read_le_at<uint8_t>(page_, 43).value_or(0);
    s.rsvd_size = b::read_le_at<uint8_t>(page_, 44).value_or(0);
    s.rsvd_pos = b::read_le_at<uint8_t>(page_, 45).value_or(0);
    return s;
  }

  /** @brief Physical address of the first `setup_data` node (0 if none); walk the list with your own mapping. */
  [[nodiscard]] result<uint64_t> try_setup_data_address() const noexcept {
    auto h = try_header();
    if (!h)
      return unexpected(h.error());
    return h->setup_data;
  }

private:
  explicit boot_params_reader(span<const std::byte> page) noexcept : page_(page) {}
  span<const std::byte> page_;
};

// ============================================================================
// Loader role: writer
// ============================================================================

/** @brief Builds a zero page in caller-owned, writable memory. */
class boot_params_writer {
public:
  /** @brief @p page must be exactly/at least 4096 bytes; it is zeroed by `init_from_setup_header()`. */
  explicit boot_params_writer(span<std::byte> page) noexcept : page_(page) {}

  /** @brief Zeroes the page and copies the bzImage's setup header (offsets 0x1F1..0x268) into it, as the
   * protocol requires, after validating the image. */
  [[nodiscard]] result<void> init_from_setup_header(span<const std::byte> bzimage) noexcept {
    if (page_.size() < boot_params_size)
      return unexpected(error::out_of_bounds);
    auto hdr = try_parse_setup_header(bzimage);
    if (!hdr)
      return unexpected(hdr.error());
    for (std::size_t i = 0; i < boot_params_size; ++i)
      page_[i] = std::byte{0};
    // Copy through the last field this decoder knows (`kernel_info_offset`) so newer optional fields survive.
    constexpr std::size_t header_end = 0x26C;
    auto src = bzimage.try_subspan(setup_header_offset, header_end - setup_header_offset);
    if (!src)
      return unexpected(error::out_of_bounds);
    for (std::size_t i = 0; i < src->size(); ++i)
      page_[setup_header_offset + i] = (*src)[i];
    return {};
  }

  [[nodiscard]] result<void> set_loader(uint8_t type_of_loader, uint8_t ext_loader_version) noexcept {
    if (auto r = boot_bytes::write_le_at<uint8_t>(page_, 0x210, type_of_loader); !r)
      return r;
    return boot_bytes::write_le_at<uint8_t>(page_, 0x226, ext_loader_version);
  }

  [[nodiscard]] result<void> set_loadflags(uint8_t flags) noexcept {
    return boot_bytes::write_le_at<uint8_t>(page_, 0x211, flags);
  }

  /** @brief Sets the physical address of the NUL-terminated command line (low 32 bits in `cmd_line_ptr`,
   * high 32 bits in `ext_cmd_line_ptr`). */
  [[nodiscard]] result<void> set_cmdline(uint64_t phys) noexcept {
    if (auto r = boot_bytes::write_le_at<uint32_t>(page_, 0x228, static_cast<uint32_t>(phys)); !r)
      return r;
    return boot_bytes::write_le_at<uint32_t>(page_, zp::ext_cmd_line_ptr, static_cast<uint32_t>(phys >> 32));
  }

  [[nodiscard]] result<void> set_ramdisk(uint64_t phys, uint64_t size) noexcept {
    if (auto r = boot_bytes::write_le_at<uint32_t>(page_, 0x218, static_cast<uint32_t>(phys)); !r)
      return r;
    if (auto r = boot_bytes::write_le_at<uint32_t>(page_, 0x21C, static_cast<uint32_t>(size)); !r)
      return r;
    if (auto r = boot_bytes::write_le_at<uint32_t>(page_, zp::ext_ramdisk_image, static_cast<uint32_t>(phys >> 32)); !r)
      return r;
    return boot_bytes::write_le_at<uint32_t>(page_, zp::ext_ramdisk_size, static_cast<uint32_t>(size >> 32));
  }

  /** @brief Points the kernel at the first `setup_data` node (physical address). */
  [[nodiscard]] result<void> set_setup_data(uint64_t phys) noexcept {
    return boot_bytes::write_le_at<uint64_t>(page_, 0x250, phys);
  }

  /** @brief Appends an e820 entry; `error::capacity_exceeded` after 128 entries. */
  [[nodiscard]] result<void> add_e820(uint64_t addr, uint64_t size, e820_type type) noexcept {
    auto count = boot_bytes::read_le_at<uint8_t>(page_, zp::e820_entries);
    if (!count)
      return unexpected(count.error());
    if (*count >= e820_max_entries)
      return unexpected(error::capacity_exceeded);
    const std::size_t off = zp::e820_table + static_cast<std::size_t>(*count) * zp::e820_entry_size;
    if (auto r = boot_bytes::write_le_at<uint64_t>(page_, off, addr); !r)
      return r;
    if (auto r = boot_bytes::write_le_at<uint64_t>(page_, off + 8, size); !r)
      return r;
    if (auto r = boot_bytes::write_le_at<uint32_t>(page_, off + 16, static_cast<uint32_t>(type)); !r)
      return r;
    return boot_bytes::write_le_at<uint8_t>(page_, zp::e820_entries, static_cast<uint8_t>(*count + 1));
  }

  /** @brief Fills `efi_info` so the kernel's EFI support finds the firmware tables. */
  [[nodiscard]] result<void> set_efi_info(const efi_info &e) noexcept {
    namespace b = boot_bytes;
    if (auto r = b::write_le_at<uint32_t>(page_, zp::efi_info, e.loader_signature); !r)
      return r;
    if (auto r = b::write_le_at<uint32_t>(page_, zp::efi_info + 4, static_cast<uint32_t>(e.systab)); !r)
      return r;
    if (auto r = b::write_le_at<uint32_t>(page_, zp::efi_info + 8, e.memdesc_size); !r)
      return r;
    if (auto r = b::write_le_at<uint32_t>(page_, zp::efi_info + 12, e.memdesc_version); !r)
      return r;
    if (auto r = b::write_le_at<uint32_t>(page_, zp::efi_info + 16, static_cast<uint32_t>(e.memmap)); !r)
      return r;
    if (auto r = b::write_le_at<uint32_t>(page_, zp::efi_info + 20, e.memmap_size); !r)
      return r;
    if (auto r = b::write_le_at<uint32_t>(page_, zp::efi_info + 24, static_cast<uint32_t>(e.systab >> 32)); !r)
      return r;
    return b::write_le_at<uint32_t>(page_, zp::efi_info + 28, static_cast<uint32_t>(e.memmap >> 32));
  }

  /** @brief Fills `screen_info` for a linear framebuffer (`video_type_efi`). */
  [[nodiscard]] result<void> set_screen_info(const screen_info &s) noexcept {
    namespace b = boot_bytes;
    if (auto r = b::write_le_at<uint8_t>(page_, 15, s.orig_video_is_vga); !r)
      return r;
    if (auto r = b::write_le_at<uint16_t>(page_, 18, s.lfb_width); !r)
      return r;
    if (auto r = b::write_le_at<uint16_t>(page_, 20, s.lfb_height); !r)
      return r;
    if (auto r = b::write_le_at<uint16_t>(page_, 22, s.lfb_depth); !r)
      return r;
    if (auto r = b::write_le_at<uint32_t>(page_, 24, static_cast<uint32_t>(s.lfb_base)); !r)
      return r;
    if (auto r = b::write_le_at<uint32_t>(page_, 28, s.lfb_size); !r)
      return r;
    if (auto r = b::write_le_at<uint16_t>(page_, 36, s.lfb_linelength); !r)
      return r;
    if (auto r = b::write_le_at<uint8_t>(page_, 38, s.red_size); !r)
      return r;
    if (auto r = b::write_le_at<uint8_t>(page_, 39, s.red_pos); !r)
      return r;
    if (auto r = b::write_le_at<uint8_t>(page_, 40, s.green_size); !r)
      return r;
    if (auto r = b::write_le_at<uint8_t>(page_, 41, s.green_pos); !r)
      return r;
    if (auto r = b::write_le_at<uint8_t>(page_, 42, s.blue_size); !r)
      return r;
    if (auto r = b::write_le_at<uint8_t>(page_, 43, s.blue_pos); !r)
      return r;
    if (auto r = b::write_le_at<uint8_t>(page_, 44, s.rsvd_size); !r)
      return r;
    if (auto r = b::write_le_at<uint8_t>(page_, 45, s.rsvd_pos); !r)
      return r;
    return b::write_le_at<uint32_t>(page_, 58, static_cast<uint32_t>(s.lfb_base >> 32));
  }

private:
  span<std::byte> page_;
};

} // namespace structo::boot::linux_x86
