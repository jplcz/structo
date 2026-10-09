// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file linux_image_header.hpp
 * @brief The 64-byte `Image` header shared in shape by the AArch64
 * (Documentation/arch/arm64/booting.rst) and RISC-V
 * (Documentation/arch/riscv/boot-image-header.rst) Linux boot protocols:
 * `structo::boot::linux_arm64::try_parse_image_header()` and
 * `structo::boot::linux_riscv::try_parse_image_header()`.
 *
 * Both give a loader what it needs to place the kernel: the offset from a
 * 2 MiB (arm64) / platform-aligned (riscv) base at which to load, the
 * runtime size to reserve, and the flags.
 *
 * ## arm64 entry
 *
 * MMU off, D-cache off (clean the image to PoC), IRQ/FIQ/SError masked,
 * EL2 (recommended) or EL1, non-secure; `x0` = physical address of the DTB
 * (8-byte aligned, < 2 MiB), `x1 = x2 = x3 = 0`. Load the image at
 * `base + text_offset` where `base` is 2 MiB aligned.
 *
 * ## riscv entry
 *
 * S-mode (via SBI, see `arch/riscv/sbi.hpp`), MMU off (`satp = 0`); `a0` =
 * boot hart id, `a1` = physical address of the DTB. Load at
 * `base + text_offset` with `base` aligned to 2 MiB (RV64) / 4 MiB (RV32).
 *
 * @code
 * // Loader role. `kernel` is the uncompressed Image file contents.
 * auto hdr = structo::boot::linux_arm64::try_parse_image_header(kernel);
 * if (!hdr)
 *   return;                                  // not an arm64 Image
 * uint64_t load_addr = dram_base_2mib_aligned + hdr->effective_text_offset();
 * // Copy `kernel` to load_addr, reserve hdr->image_size (or more if 0), place the DTB elsewhere,
 * // then jump to load_addr with x0 = dtb_phys.
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>
#include <structo/detail/boot_bytes.hpp>

namespace structo::boot {

namespace linux_arm64 {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

/** @brief `"ARM\x64"` little-endian magic at offset 56. */
inline constexpr uint32_t image_magic = 0x644d5241;
/** @brief `text_offset` assumed when `image_size` is 0 (pre-v3.17 kernels). */
inline constexpr uint64_t legacy_text_offset = 0x80000;

inline constexpr uint64_t flag_big_endian = 1u << 0;
/** @brief Page-size field (bits 1-2): 0 unspecified, 1 = 4K, 2 = 16K, 3 = 64K. */
inline constexpr unsigned flag_page_size_shift = 1;
/** @brief Physical placement bit 3: 0 = base close to DRAM start, 1 = anywhere in the 48-bit range. */
inline constexpr uint64_t flag_phys_placement_any = 1u << 3;

/** @brief Decoded arm64 `Image` header. */
struct image_header {
  uint64_t text_offset = 0;
  uint64_t image_size = 0;
  uint64_t flags = 0;
  /** @brief Offset of the PE/COFF header (0 if the image is not an EFI application). */
  uint32_t pe_offset = 0;

  [[nodiscard]] constexpr bool big_endian() const noexcept { return (flags & flag_big_endian) != 0; }
  /** @brief Kernel page size in bytes, 0 if unspecified. */
  [[nodiscard]] constexpr uint64_t page_size() const noexcept {
    switch ((flags >> flag_page_size_shift) & 3) {
    case 1:
      return 4096;
    case 2:
      return 16384;
    case 3:
      return 65536;
    default:
      return 0;
    }
  }
  /** @brief The offset to load at: `text_offset`, or `0x80000` when `image_size == 0` (old kernels). */
  [[nodiscard]] constexpr uint64_t effective_text_offset() const noexcept {
    return image_size == 0 ? legacy_text_offset : text_offset;
  }
  [[nodiscard]] constexpr bool is_efi_application() const noexcept { return pe_offset != 0; }
};

[[nodiscard]] inline result<image_header> try_parse_image_header(span<const std::byte> image) noexcept {
  auto magic = boot_bytes::read_le_at<uint32_t>(image, 56);
  if (!magic)
    return unexpected(magic.error());
  if (*magic != image_magic)
    return unexpected(error::invalid_argument);
  auto text_offset = boot_bytes::read_le_at<uint64_t>(image, 8);
  auto image_size = boot_bytes::read_le_at<uint64_t>(image, 16);
  auto flags = boot_bytes::read_le_at<uint64_t>(image, 24);
  auto pe = boot_bytes::read_le_at<uint32_t>(image, 60);
  if (!text_offset || !image_size || !flags || !pe)
    return unexpected(error::out_of_bounds);
  return image_header{*text_offset, *image_size, *flags, *pe};
}

} // namespace linux_arm64

namespace linux_riscv {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

/** @brief `"RSC\x05"` little-endian `magic2` at offset 56 (replaces the deprecated 64-bit "RISCV" `magic`). */
inline constexpr uint32_t image_magic2 = 0x05435352;
/** @brief Deprecated 64-bit `"RISCV"` magic at offset 48. */
inline constexpr uint64_t image_magic_legacy = 0x5643534952;

inline constexpr uint64_t flag_big_endian = 1u << 0;

/** @brief Decoded RISC-V `Image` header. */
struct image_header {
  uint64_t text_offset = 0;
  uint64_t image_size = 0;
  uint64_t flags = 0;
  /** @brief Header version: major in bits 31:16, minor in bits 15:0 (current: 0.2). */
  uint32_t version = 0;
  uint32_t pe_offset = 0;

  [[nodiscard]] constexpr uint16_t version_major() const noexcept { return static_cast<uint16_t>(version >> 16); }
  [[nodiscard]] constexpr uint16_t version_minor() const noexcept { return static_cast<uint16_t>(version & 0xFFFF); }
  [[nodiscard]] constexpr bool big_endian() const noexcept { return (flags & flag_big_endian) != 0; }
  [[nodiscard]] constexpr bool is_efi_application() const noexcept { return pe_offset != 0; }
};

/** @brief Validates (via `magic2`, or the legacy `magic` for pre-0.2 headers) and decodes the header. The
 * spec says a zero `image_size` makes booting fail, so it is rejected as `error::invalid_argument`. */
[[nodiscard]] inline result<image_header> try_parse_image_header(span<const std::byte> image) noexcept {
  auto magic2 = boot_bytes::read_le_at<uint32_t>(image, 56);
  auto magic = boot_bytes::read_le_at<uint64_t>(image, 48);
  if (!magic2 || !magic)
    return unexpected(error::out_of_bounds);
  if (*magic2 != image_magic2 && *magic != image_magic_legacy)
    return unexpected(error::invalid_argument);
  auto text_offset = boot_bytes::read_le_at<uint64_t>(image, 8);
  auto image_size = boot_bytes::read_le_at<uint64_t>(image, 16);
  auto flags = boot_bytes::read_le_at<uint64_t>(image, 24);
  auto version = boot_bytes::read_le_at<uint32_t>(image, 32);
  auto pe = boot_bytes::read_le_at<uint32_t>(image, 60);
  if (!text_offset || !image_size || !flags || !version || !pe)
    return unexpected(error::out_of_bounds);
  if (*image_size == 0)
    return unexpected(error::invalid_argument);
  return image_header{*text_offset, *image_size, *flags, *version, *pe};
}

} // namespace linux_riscv

} // namespace structo::boot
