// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file uboot.hpp
 * @brief U-Boot interoperability: the legacy `uImage` header (parse, verify,
 * build), format detection (legacy vs. FIT), and a reader for the U-Boot
 * environment blob.
 *
 * - **Legacy uImage**: a 64-byte big-endian header prepended to a payload
 *   (kernel, ramdisk, ...). Both the header and the payload are CRC-32
 *   protected.
 * - **FIT** (Flattened Image Tree): a devicetree blob whose nodes hold the
 *   images; detect it with `detect_format()` and walk it with
 *   `structo/fdt_reader.hpp`.
 * - **Environment**: `crc32 (LE u32) [flags u8, redundant only] key=value\0 ... \0\0`.
 *
 * U-Boot hands off to Linux via `bootm`: arm `r0=0, r1=machine/~0, r2=DTB`;
 * arm64 `x0=DTB`; riscv `a0=hartid, a1=DTB` - see `linux_arm.hpp` and
 * `linux_image_header.hpp`.
 *
 * @code
 * // Read side: why - validate an image U-Boot (or mkimage) produced before trusting its fields.
 * auto img = structo::boot::uboot::try_parse_image(blob);   // blob: whole file as span<const std::byte>
 * if (!img)
 *   return;                                                  // bad magic, header CRC or truncated payload
 * // img->header.load / entry: addresses the image expects; img->payload: the bytes (CRC already verified).
 *
 * // Environment: env_blob is the raw environment partition contents.
 * auto env = structo::boot::uboot::env_reader::try_create(env_blob, false);  // false = non-redundant layout
 * auto bootargs = env->find("bootargs");                     // optional<string_view>
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>
#include <structo/detail/boot_bytes.hpp>

namespace structo::boot::uboot {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

inline constexpr uint32_t image_magic = 0x27051956;
inline constexpr std::size_t image_header_size = 64;
inline constexpr std::size_t image_name_size = 32;

/** @brief `ih_os` values. */
enum class os : uint8_t {
  invalid = 0,
  openbsd = 1,
  netbsd = 2,
  freebsd = 3,
  linux_ = 5,
  u_boot = 17,
  rtems = 18,
  efi = 23
};
/** @brief `ih_arch` values. */
enum class arch : uint8_t {
  invalid = 0,
  alpha = 1,
  arm = 2,
  x86 = 3,
  ia64 = 4,
  mips = 5,
  mips64 = 6,
  powerpc = 7,
  s390 = 8,
  sh = 9,
  sparc = 10,
  sparc64 = 11,
  m68k = 12,
  microblaze = 14,
  nios2 = 15,
  blackfin = 16,
  avr32 = 17,
  st200 = 18,
  sandbox = 19,
  nds32 = 20,
  openrisc = 21,
  arm64 = 22,
  arc = 24,
  x86_64 = 25,
  xtensa = 26,
  riscv = 28,
};
/** @brief `ih_type` values. */
enum class image_type : uint8_t {
  invalid = 0,
  standalone = 1,
  kernel = 2,
  ramdisk = 3,
  multi = 4,
  firmware = 5,
  script = 6,
  filesystem = 7,
  flat_dt = 8,
  kernel_noload = 14,
};
/** @brief `ih_comp` values. */
enum class compression : uint8_t { none = 0, gzip = 1, bzip2 = 2, lzma = 3, lzo = 4, lz4 = 5, zstd = 6 };

/** @brief Decoded uImage header. Multi-byte fields are host-order. */
struct image_header {
  uint32_t header_crc = 0;
  uint32_t timestamp = 0;
  uint32_t data_size = 0;
  uint32_t load = 0;
  uint32_t entry = 0;
  uint32_t data_crc = 0;
  os os_ = os::invalid;
  arch arch_ = arch::invalid;
  image_type type = image_type::invalid;
  compression comp = compression::none;
  /** @brief Image name: the bytes up to the first NUL (or all 32). */
  array<char, image_name_size + 1> name = {};
};

/** @brief A verified image: its header and the payload that follows it. */
struct image_view {
  image_header header;
  span<const std::byte> payload;
};

/** @brief Container formats recognised by `detect_format()`. */
enum class image_format { legacy, fit, unknown };

[[nodiscard]] inline image_format detect_format(span<const std::byte> blob) noexcept {
  if (auto m = boot_bytes::read_be_at<uint32_t>(blob, 0)) {
    if (*m == image_magic)
      return image_format::legacy;
    if (*m == 0xd00dfeed)
      return image_format::fit;
  }
  return image_format::unknown;
}

/** @brief Decodes the header without verifying CRCs. */
[[nodiscard]] inline result<image_header> try_parse_header(span<const std::byte> blob) noexcept {
  auto magic = boot_bytes::read_be_at<uint32_t>(blob, 0);
  if (!magic)
    return unexpected(error::out_of_bounds);
  if (*magic != image_magic)
    return unexpected(error::invalid_argument);
  if (blob.size() < image_header_size)
    return unexpected(error::out_of_bounds);
  auto hcrc = boot_bytes::read_be_at<uint32_t>(blob, 4);
  auto time = boot_bytes::read_be_at<uint32_t>(blob, 8);
  auto size = boot_bytes::read_be_at<uint32_t>(blob, 12);
  auto load = boot_bytes::read_be_at<uint32_t>(blob, 16);
  auto ep = boot_bytes::read_be_at<uint32_t>(blob, 20);
  auto dcrc = boot_bytes::read_be_at<uint32_t>(blob, 24);
  if (!hcrc || !time || !size || !load || !ep || !dcrc)
    return unexpected(error::out_of_bounds);
  image_header h;
  h.header_crc = *hcrc;
  h.timestamp = *time;
  h.data_size = *size;
  h.load = *load;
  h.entry = *ep;
  h.data_crc = *dcrc;
  h.os_ = static_cast<os>(blob[28]);
  h.arch_ = static_cast<arch>(blob[29]);
  h.type = static_cast<image_type>(blob[30]);
  h.comp = static_cast<compression>(blob[31]);
  for (std::size_t i = 0; i < image_name_size && blob[32 + i] != std::byte{0}; ++i)
    h.name[i] = static_cast<char>(blob[32 + i]);
  return h;
}

/** @brief CRC-32 of the 64-byte header with its `ih_hcrc` field treated as zero (as `mkimage` computes it). */
[[nodiscard]] inline result<uint32_t> try_compute_header_crc(span<const std::byte> blob) noexcept {
  if (blob.size() < image_header_size)
    return unexpected(error::out_of_bounds);
  const std::byte zeros[4] = {};
  uint32_t crc = boot_bytes::crc32(blob.first(4));
  crc = boot_bytes::crc32_update(crc, span<const std::byte>(zeros, 4));
  return boot_bytes::crc32_update(crc, blob.subspan(8, image_header_size - 8));
}

/** @brief Parses and fully verifies a legacy image: magic, header CRC, payload bounds and payload CRC. */
[[nodiscard]] inline result<image_view> try_parse_image(span<const std::byte> blob) noexcept {
  auto h = try_parse_header(blob);
  if (!h)
    return unexpected(h.error());
  auto hcrc = try_compute_header_crc(blob);
  if (!hcrc)
    return unexpected(hcrc.error());
  if (*hcrc != h->header_crc)
    return unexpected(error::security_violation);
  auto payload = blob.try_subspan(image_header_size, h->data_size);
  if (!payload)
    return unexpected(error::out_of_bounds);
  if (boot_bytes::crc32(*payload) != h->data_crc)
    return unexpected(error::security_violation);
  return image_view{*h, *payload};
}

/** @brief Writes a header for @p payload into the first 64 bytes of @p out (payload is not copied). */
[[nodiscard]] inline result<void> try_write_header(span<std::byte> out, span<const std::byte> payload,
                                                   const image_header &fields) noexcept {
  if (out.size() < image_header_size)
    return unexpected(error::capacity_exceeded);
  if (payload.size() > 0xFFFFFFFFu)
    return unexpected(error::out_of_range);
  for (std::size_t i = 0; i < image_header_size; ++i)
    out[i] = std::byte{0};
  const result<void> steps[] = {
      boot_bytes::write_be_at<uint32_t>(out, 0, image_magic),
      boot_bytes::write_be_at<uint32_t>(out, 8, fields.timestamp),
      boot_bytes::write_be_at<uint32_t>(out, 12, static_cast<uint32_t>(payload.size())),
      boot_bytes::write_be_at<uint32_t>(out, 16, fields.load),
      boot_bytes::write_be_at<uint32_t>(out, 20, fields.entry),
      boot_bytes::write_be_at<uint32_t>(out, 24, boot_bytes::crc32(payload)),
  };
  for (const auto &s : steps)
    if (!s)
      return unexpected(error::capacity_exceeded);
  out[28] = static_cast<std::byte>(fields.os_);
  out[29] = static_cast<std::byte>(fields.arch_);
  out[30] = static_cast<std::byte>(fields.type);
  out[31] = static_cast<std::byte>(fields.comp);
  for (std::size_t i = 0; i < image_name_size && fields.name[i] != '\0'; ++i)
    out[32 + i] = static_cast<std::byte>(fields.name[i]);
  auto hcrc = try_compute_header_crc(span<const std::byte>(out.data(), out.size()));
  if (!hcrc)
    return unexpected(hcrc.error());
  return boot_bytes::write_be_at<uint32_t>(out, 4, *hcrc);
}

/** @brief Reader over a U-Boot environment blob: CRC-verified, `key=value` lookups. */
class env_reader {
public:
  /** @brief Verifies the CRC-32 (covering everything after the CRC and, if @p redundant, the flags byte).
   * @param redundant `true` for the redundant-environment layout that has a 1-byte flags field. */
  [[nodiscard]] static result<env_reader> try_create(span<const std::byte> blob, bool redundant) noexcept {
    const std::size_t head = redundant ? 5 : 4;
    auto crc = boot_bytes::read_le_at<uint32_t>(blob, 0);
    if (!crc || blob.size() < head)
      return unexpected(error::out_of_bounds);
    if (boot_bytes::crc32(blob.subspan(head)) != *crc)
      return unexpected(error::security_violation);
    return env_reader(blob.subspan(head));
  }

  /** @brief The value of @p key, or `nullopt` if absent. */
  [[nodiscard]] optional<string_view> find(string_view key) const noexcept {
    std::size_t pos = 0;
    while (pos < data_.size() && data_[pos] != std::byte{0}) {
      std::size_t end = pos;
      while (end < data_.size() && data_[end] != std::byte{0})
        ++end;
      if (end == data_.size())
        break; // Unterminated entry.
      const auto raw = data_.subspan(pos, end - pos);
      const string_view entry(reinterpret_cast<const char *>(raw.data()), raw.size());
      if (entry.size() > key.size() && entry[key.size()] == '=' && entry.substr(0, key.size()) == key)
        return optional<string_view>(entry.substr(key.size() + 1));
      pos = end + 1;
    }
    return nullopt;
  }

private:
  explicit env_reader(span<const std::byte> data) noexcept : data_(data) {}
  span<const std::byte> data_;
};

} // namespace structo::boot::uboot
