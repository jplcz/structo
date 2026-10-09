// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file linux_arm.hpp
 * @brief The 32-bit ARM Linux boot protocol (Documentation/arch/arm/booting.rst):
 * a decoder for the `zImage` header, and an iterator/writer pair for the
 * legacy ATAGS tagged list (the pre-devicetree way to pass memory, command
 * line and initrd to the kernel).
 *
 * ## Entry convention
 *
 * Jump to the first instruction of the (decompressed-by-itself) `zImage`
 * in ARM state, MMU off, D-cache off, IRQ/FIQ masked, with:
 *
 * | register | value |
 * |---|---|
 * | `r0` | `0` |
 * | `r1` | machine type number, or `~0` for devicetree-only platforms |
 * | `r2` | physical address of the ATAGS list **or** of the DTB |
 *
 * The kernel tells the two apart by looking for the DTB magic
 * `0xd00dfeed` at `r2` (`kernel_arg_kind()`); a devicetree is the modern
 * choice, ATAGS exist for old boards. For devicetrees use
 * `structo/fdt_*.hpp`; this header covers only what is ARM-Linux-specific.
 *
 * @code
 * // Loader role: build an ATAGS list in a caller-owned buffer placed in
 * // the first 16 KiB of RAM (the recommended location).
 * structo::boot::linux_arm::atag_writer w(atag_buffer);
 * (void)w.add_core();                                  // mandatory first tag (empty form)
 * (void)w.add_mem(0x80000000, 0x20000000);              // start, size: 512 MiB of RAM at 0x80000000
 * (void)w.add_cmdline("console=ttyAMA0 root=/dev/vda");  // kernel command line
 * (void)w.add_initrd2(initrd_phys, initrd_size);        // physical start and size of the initramfs
 * (void)w.finish();                                      // terminating ATAG_NONE
 * // Jump with r0 = 0, r1 = machine type, r2 = physical address of atag_buffer.
 * @endcode
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/iterator.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>
#include <structo/boot/memory_kind.hpp>
#include <structo/detail/boot_bytes.hpp>

namespace structo::boot::linux_arm {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

// ============================================================================
// zImage header
// ============================================================================

/** @brief `zImage` magic at offset 0x24 (little-endian). */
inline constexpr uint32_t zimage_magic = 0x016F2818;
/** @brief Endianness marker at offset 0x30: `0x04030201` for a little-endian image. */
inline constexpr uint32_t zimage_endian_le = 0x04030201;
inline constexpr uint32_t zimage_endian_be = 0x01020304;
/** @brief Machine type to pass in `r1` for devicetree-only platforms. */
inline constexpr uint32_t machine_type_dt_only = 0xFFFFFFFF;
/** @brief Magic of a flattened devicetree (big-endian `0xd00dfeed`). */
inline constexpr uint32_t dtb_magic = 0xd00dfeed;

/** @brief Decoded `zImage` header: the addresses are relative to the image's own load address. */
struct zimage_header {
  /** @brief Offset (relative to image start) of the first instruction. Always 0. */
  uint32_t start = 0;
  /** @brief Offset one past the last byte of the image. Equals the file size. */
  uint32_t end = 0;
  bool little_endian = true;

  [[nodiscard]] constexpr uint32_t image_size() const noexcept { return end - start; }
};

/** @brief Validates and decodes a `zImage` header from the start of @p image. */
[[nodiscard]] inline result<zimage_header> try_parse_zimage_header(span<const std::byte> image) noexcept {
  auto magic = boot_bytes::read_le_at<uint32_t>(image, 0x24);
  auto start = boot_bytes::read_le_at<uint32_t>(image, 0x28);
  auto end = boot_bytes::read_le_at<uint32_t>(image, 0x2C);
  auto endian = boot_bytes::read_le_at<uint32_t>(image, 0x30);
  if (!magic || !start || !end || !endian)
    return unexpected(error::out_of_bounds);
  if (*magic != zimage_magic)
    return unexpected(error::invalid_argument);
  if (*endian != zimage_endian_le && *endian != zimage_endian_be)
    return unexpected(error::invalid_argument);
  if (*end < *start)
    return unexpected(error::invalid_argument);
  zimage_header h;
  h.start = *start;
  h.end = *end;
  h.little_endian = *endian == zimage_endian_le;
  return h;
}

/** @brief Whatever `r2` points at, as the kernel distinguishes it. */
enum class kernel_arg_kind { devicetree, atags, unknown };

/** @brief Classifies the bytes at `r2`: a DTB (big-endian magic) or an ATAGS list (first tag `ATAG_CORE`). */
[[nodiscard]] inline kernel_arg_kind classify_kernel_arg(span<const std::byte> r2_bytes) noexcept {
  if (auto be = boot_bytes::read_be_at<uint32_t>(r2_bytes, 0); be && *be == dtb_magic)
    return kernel_arg_kind::devicetree;
  if (auto tag = boot_bytes::read_le_at<uint32_t>(r2_bytes, 4); tag && *tag == 0x54410001)
    return kernel_arg_kind::atags;
  return kernel_arg_kind::unknown;
}

// ============================================================================
// ATAGS
// ============================================================================

/** @brief ATAG tag values. */
enum class atag_tag : uint32_t {
  none = 0x00000000,
  core = 0x54410001,
  mem = 0x54410002,
  videotext = 0x54410003,
  ramdisk = 0x54410004,
  initrd2 = 0x54420005,
  serial = 0x54410006,
  revision = 0x54410007,
  videolfb = 0x54410008,
  cmdline = 0x54410009,
};

/** @brief One ATAG: its tag value and raw payload (the bytes after the 8-byte header). */
struct atag {
  atag_tag tag{};
  span<const std::byte> payload{};
};

/** @brief Decoded `ATAG_MEM`. */
struct atag_mem {
  uint32_t start = 0;
  uint32_t size = 0;
};

/** @brief Decoded `ATAG_INITRD2`. */
struct atag_initrd2 {
  uint32_t start = 0;
  uint32_t size = 0;
};

/** @brief Iterates an ATAGS list: requires `ATAG_CORE` first, ends at `ATAG_NONE` (yielded, then exhausted).
 * A malformed size surfaces as a single error item, after which iteration stops. */
class RELOCO_POINTER atag_reader : public iterator_adaptor<atag_reader, result<atag>> {
public:
  using item_type = result<atag>;

  [[nodiscard]] static result<atag_reader> try_create(span<const std::byte> list) noexcept {
    auto size = boot_bytes::read_le_at<uint32_t>(list, 0);
    auto tag = boot_bytes::read_le_at<uint32_t>(list, 4);
    if (!size || !tag)
      return unexpected(error::out_of_bounds);
    if (*tag != static_cast<uint32_t>(atag_tag::core))
      return unexpected(error::invalid_argument);
    return atag_reader(list);
  }

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (done_)
      return nullopt;
    auto size_words = boot_bytes::read_le_at<uint32_t>(list_, cursor_);
    auto tag = boot_bytes::read_le_at<uint32_t>(list_, cursor_ + 4);
    if (!size_words || !tag)
      return fail(error::out_of_bounds);
    if (*tag == static_cast<uint32_t>(atag_tag::none)) {
      done_ = true;
      return optional<item_type>(item_type(atag{atag_tag::none, {}}));
    }
    if (*size_words < 2) // Size counts the 2-word header.
      return fail(error::invalid_argument);
    const std::size_t bytes = static_cast<std::size_t>(*size_words) * 4;
    auto payload = list_.try_subspan(cursor_ + 8, bytes - 8);
    if (!payload)
      return fail(error::out_of_bounds);
    cursor_ += bytes;
    return optional<item_type>(item_type(atag{static_cast<atag_tag>(*tag), *payload}));
  }

private:
  explicit atag_reader(span<const std::byte> list) noexcept : list_(list) {}
  [[nodiscard]] optional<item_type> fail(error e) noexcept {
    done_ = true;
    return optional<item_type>(item_type(unexpected(e)));
  }
  span<const std::byte> list_;
  std::size_t cursor_ = 0;
  bool done_ = false;
};

[[nodiscard]] inline result<atag_mem> try_decode_mem(const atag &t) noexcept {
  if (t.tag != atag_tag::mem)
    return unexpected(error::invalid_argument);
  auto size = boot_bytes::read_le_at<uint32_t>(t.payload, 0);
  auto start = boot_bytes::read_le_at<uint32_t>(t.payload, 4);
  if (!size || !start)
    return unexpected(error::out_of_bounds);
  return atag_mem{*start, *size};
}

[[nodiscard]] inline result<atag_initrd2> try_decode_initrd2(const atag &t) noexcept {
  if (t.tag != atag_tag::initrd2)
    return unexpected(error::invalid_argument);
  auto start = boot_bytes::read_le_at<uint32_t>(t.payload, 0);
  auto size = boot_bytes::read_le_at<uint32_t>(t.payload, 4);
  if (!start || !size)
    return unexpected(error::out_of_bounds);
  return atag_initrd2{*start, *size};
}

/** @brief The command line of an `ATAG_CMDLINE` (without the trailing NUL). */
[[nodiscard]] inline result<string_view> try_decode_cmdline(const atag &t) noexcept {
  if (t.tag != atag_tag::cmdline)
    return unexpected(error::invalid_argument);
  std::size_t length = 0;
  while (length < t.payload.size() && t.payload[length] != std::byte{0})
    ++length;
  if (length == t.payload.size())
    return unexpected(error::invalid_argument); // No terminating NUL.
  auto text = t.payload.first(length);
  return string_view(reinterpret_cast<const char *>(text.data()), length);
}

/** @brief Folds every `ATAG_MEM` of @p list into @p map as `memory_kind::usable`. */
template <typename Map> [[nodiscard]] result<void> try_fill_memory_map(atag_reader list, Map &map) noexcept {
  for (auto t : list) {
    if (!t)
      return unexpected(t.error());
    if (t->tag != atag_tag::mem)
      continue;
    auto m = try_decode_mem(*t);
    if (!m)
      return unexpected(m.error());
    if (auto r = map.try_add(memory_kind::usable, m->start, m->size); !r)
      return r;
  }
  return {};
}

/** @brief Builds an ATAGS list in caller-owned, writable memory. Tags must be added in the order they
 * should appear; `add_core()` first and `finish()` last. */
class atag_writer {
public:
  explicit atag_writer(span<std::byte> buffer) noexcept : buf_(buffer) {}

  /** @brief `ATAG_CORE` in its empty form (size field 2). Must be first. */
  [[nodiscard]] result<void> add_core() noexcept { return put_header(2, atag_tag::core); }

  [[nodiscard]] result<void> add_mem(uint32_t start, uint32_t size) noexcept {
    if (auto r = put_header(4, atag_tag::mem); !r)
      return r;
    if (auto r = put_word(size); !r)
      return r;
    return put_word(start);
  }

  [[nodiscard]] result<void> add_initrd2(uint32_t start, uint32_t size) noexcept {
    if (auto r = put_header(4, atag_tag::initrd2); !r)
      return r;
    if (auto r = put_word(start); !r)
      return r;
    return put_word(size);
  }

  [[nodiscard]] result<void> add_serial(uint32_t low, uint32_t high) noexcept {
    if (auto r = put_header(4, atag_tag::serial); !r)
      return r;
    if (auto r = put_word(low); !r)
      return r;
    return put_word(high);
  }

  [[nodiscard]] result<void> add_revision(uint32_t rev) noexcept {
    if (auto r = put_header(3, atag_tag::revision); !r)
      return r;
    return put_word(rev);
  }

  /** @brief `ATAG_CMDLINE`: @p text plus a terminating NUL, padded to a whole number of words. */
  [[nodiscard]] result<void> add_cmdline(string_view text) noexcept {
    const std::size_t padded = (text.size() + 1 + 3) / 4 * 4;
    if (auto r = put_header(static_cast<uint32_t>(2 + padded / 4), atag_tag::cmdline); !r)
      return r;
    auto dst = buf_.try_subspan(cursor_, padded);
    if (!dst)
      return unexpected(error::capacity_exceeded);
    for (std::size_t i = 0; i < padded; ++i)
      (*dst)[i] = i < text.size() ? static_cast<std::byte>(text[i]) : std::byte{0};
    cursor_ += padded;
    return {};
  }

  /** @brief Writes the terminating `ATAG_NONE` (size 0). */
  [[nodiscard]] result<void> finish() noexcept {
    if (auto r = put_word(0); !r)
      return r;
    return put_word(static_cast<uint32_t>(atag_tag::none));
  }

  /** @brief Bytes written so far. */
  [[nodiscard]] std::size_t size() const noexcept { return cursor_; }

private:
  [[nodiscard]] result<void> put_word(uint32_t v) noexcept {
    if (auto r = boot_bytes::write_le_at<uint32_t>(buf_, cursor_, v); !r)
      return unexpected(error::capacity_exceeded);
    cursor_ += 4;
    return {};
  }
  [[nodiscard]] result<void> put_header(uint32_t size_words, atag_tag tag) noexcept {
    if (auto r = put_word(size_words); !r)
      return r;
    return put_word(static_cast<uint32_t>(tag));
  }
  span<std::byte> buf_;
  std::size_t cursor_ = 0;
};

} // namespace structo::boot::linux_arm
