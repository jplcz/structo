// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file multiboot2.hpp
 * @brief Basic support for the Multiboot2 Specification (the protocol
 * GRUB2 and most other modern x86/x86-64 bootloaders use to load a
 * kernel): `structo::arch::x86::multiboot2_header`, a fixed, embeddable
 * header a kernel places near the start of its image so the bootloader
 * recognizes it, and `multiboot2_boot_info_reader`, a Rust-style,
 * bounds-checked iterable view over the tag-based boot information
 * structure the bootloader hands back at kernel entry.
 *
 * ## The two directions of the protocol
 *
 * Multiboot2 is two independent, one-way exchanges:
 *
 * - **Kernel to bootloader** (`multiboot2_header`): a fixed 16-byte
 *   record -- magic, architecture, header length, checksum -- the
 *   bootloader scans for in the first 32 KiB of the kernel image, 8-byte
 *   aligned, to recognize it as Multiboot2-compliant before loading it
 *   at all. This basic driver covers only the minimal header (no
 *   optional request tags, e.g. framebuffer/module alignment requests)
 *   terminated by the mandatory end tag -- see `make_basic_header()`.
 * - **Bootloader to kernel** (`multiboot2_boot_info_reader`): at kernel
 *   entry (32-bit protected mode, per the spec), register `eax` holds
 *   `multiboot2_bootloader_magic` and `ebx` holds the *physical* address
 *   of a tag-based boot information structure this reader walks --
 *   command line, memory map, modules, framebuffer, ELF section headers,
 *   and more, exactly the FDT-on-ARM equivalent for x86.
 *
 * Like `fdt_reader.hpp` (the devicetree counterpart this header
 * deliberately mirrors in shape), the boot information structure is
 * firmware/bootloader-supplied, untrusted input: every tag length/offset
 * is bounds-checked against the caller-owned span on every step, and a
 * malformed tag surfaces as a `reloco::error` item rather than undefined
 * behavior -- exactly once, then the iterator is permanently exhausted,
 * matching `reloco::iterator_adaptor`'s contract. All multi-byte fields
 * are little-endian (x86's native order), unlike the DTB's big-endian
 * fields -- the one structural difference from `fdt_reader.hpp`.
 *
 * @code
 * // Embedded near the start of the kernel image (e.g. in a dedicated
 * // linker-script section placed within the first 32 KiB):
 * alignas(8) constexpr auto multiboot_header = structo::arch::x86::make_basic_header();
 *
 * // At kernel entry, with ebx's value already captured into `info_phys_addr`
 * // and mapped/identity-accessible as `info_bytes`:
 * auto reader = structo::arch::x86::multiboot2_boot_info_reader::try_create(info_bytes);
 * for (auto tag : *reader) {
 *   if (!tag)
 *     break;
 *   if (tag->type == structo::arch::x86::multiboot2_tag_type::memory_map) {
 *     for (auto entry : structo::arch::x86::multiboot2_mmap_entries(*tag)) {
 *       if (entry && entry->is_available())
 *         handle_free_ram(entry->base_addr, entry->length);
 *     }
 *   }
 * }
 * @endcode
 *
 * ## Validation
 *
 * The boot-info reader and tag/entry iterators are pure bounds-checked
 * bookkeeping over caller-owned bytes -- no port I/O, no privileged
 * instructions -- so, unlike this directory's other hardware drivers,
 * they *are* covered by `tests/test_multiboot2.cpp` against synthetic,
 * hand-built boot information blobs, the same way `fdt_reader.hpp` is
 * tested against hand-built DTB blobs. `multiboot2_header`/
 * `make_basic_header()` are compile-time constant data with nothing to
 * runtime-test beyond the header-check target.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/iterator.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <utility>

namespace structo::arch::x86 {

using namespace reloco;

// This file walks the Multiboot2 boot information blob through raw
// std::byte* pointer arithmetic derived from checked span offsets
// throughout; treated as a single checked boundary like
// `fdt_reader.hpp`.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace detail {

/** @brief Reads a little-endian `uint32_t` at `offset` within `region`,
 * bounds-checked via `span::try_subspan`. Multiboot2's native byte order
 * (unlike the DTB's big-endian fields `detail/fdt_format.hpp` reads). */
[[nodiscard]] inline result<uint32_t> read_u32_at(span<const std::byte> region, std::size_t offset) noexcept {
  auto slice = region.try_subspan(offset, 4);
  if (!slice)
    return unexpected(error::out_of_bounds);
  const std::byte *p = slice->data();
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

/** @brief Reads a little-endian `uint64_t` at `offset` within `region`,
 * bounds-checked via `span::try_subspan`. */
[[nodiscard]] inline result<uint64_t> read_u64_at(span<const std::byte> region, std::size_t offset) noexcept {
  auto lo = read_u32_at(region, offset);
  if (!lo)
    return unexpected(lo.error());
  auto hi = read_u32_at(region, offset + 4);
  if (!hi)
    return unexpected(hi.error());
  return (static_cast<uint64_t>(*hi) << 32) | static_cast<uint64_t>(*lo);
}

/** @brief Rounds `value` up to the next multiple of 8, matching every
 * Multiboot2 tag's mandatory 8-byte alignment padding. */
[[nodiscard]] constexpr std::size_t align8(std::size_t value) noexcept { return (value + 7) & ~std::size_t{7}; }

} // namespace detail

// ============================================================================
// Kernel to bootloader: the embeddable Multiboot2 header
// ============================================================================

/** @brief Magic value the bootloader scans for, 8-byte aligned, within
 * the first 32 KiB of the kernel image, to recognize it as a Multiboot2
 * header. */
inline constexpr uint32_t multiboot2_header_magic = 0xE85250D6;

/** @brief Magic value the bootloader leaves in `eax` at kernel entry,
 * proving it loaded the kernel via the Multiboot2 protocol. */
inline constexpr uint32_t multiboot2_bootloader_magic = 0x36D76289;

/** @brief The only architecture value this header supports: 32-bit
 * (protected-mode) i386, the architecture Multiboot2 itself always
 * hands control over in, even for a kernel that immediately switches to
 * long mode. */
inline constexpr uint32_t multiboot2_architecture_i386 = 0;

/** @brief The mandatory end tag every Multiboot2 header (and boot
 * information structure) is terminated by: `type = 0`, `flags = 0`,
 * `size = 8`. */
struct multiboot2_end_tag {
  uint16_t type = 0;
  uint16_t flags = 0;
  uint32_t size = 8;
};

/**
 * @brief The fixed 16-byte record a Multiboot2-compliant kernel embeds
 * near the start of its image: magic, architecture, this header's own
 * length, and a checksum such that `magic + architecture + header_length
 * + checksum == 0` (mod 2^32), per the spec.
 *
 * This basic driver covers only the minimal header -- no optional
 * request tags (e.g. a framebuffer or module-alignment request); a
 * kernel needing those appends its own tag structs (each 8-byte
 * aligned) between this header and `multiboot2_end_tag`, and recomputes
 * `header_length`/`checksum` accordingly, which is outside this basic
 * header's scope.
 */
struct multiboot2_header {
  uint32_t magic = multiboot2_header_magic;
  uint32_t architecture = multiboot2_architecture_i386;
  uint32_t header_length = 0;
  uint32_t checksum = 0;
};

/**
 * @brief Builds the minimal valid `multiboot2_header` + mandatory
 * `multiboot2_end_tag` pair, with `header_length`/`checksum` computed
 * for exactly this combination's size -- no optional request tags.
 * Place the result, `alignas(8)`, within the first 32 KiB of the kernel
 * image (typically via a dedicated linker-script section).
 */
[[nodiscard]] constexpr std::pair<multiboot2_header, multiboot2_end_tag> make_basic_header() noexcept {
  multiboot2_header hdr{};
  hdr.header_length = static_cast<uint32_t>(sizeof(multiboot2_header) + sizeof(multiboot2_end_tag));
  // mod-2^32 wraparound is exactly the spec's own checksum arithmetic; a
  // plain unsigned subtraction already does the right thing here.
  hdr.checksum = static_cast<uint32_t>(0u - hdr.magic - hdr.architecture - hdr.header_length);
  return {hdr, multiboot2_end_tag{}};
}

// ============================================================================
// Bootloader to kernel: the tag-based boot information structure
// ============================================================================

/** @brief The tag types this basic driver recognizes; a tag of any other
 * type is still yielded (with its raw `payload`) but not otherwise
 * interpreted. See the Multiboot2 spec for the full list and every
 * type's payload layout beyond `memory_map` (handled here via
 * `multiboot2_mmap_entries`). */
enum class multiboot2_tag_type : uint32_t {
  end = 0,
  cmdline = 1,
  boot_loader_name = 2,
  module = 3,
  basic_memory_info = 4,
  bios_boot_device = 5,
  memory_map = 6,
  vbe_info = 7,
  framebuffer_info = 8,
  elf_sections = 9,
  apm_table = 10,
  efi32_system_table = 11,
  efi64_system_table = 12,
  smbios_tables = 13,
  acpi_old_rsdp = 14,
  acpi_new_rsdp = 15,
  networking_info = 16,
  efi_memory_map = 17,
  efi_boot_services_not_terminated = 18,
  efi32_image_handle = 19,
  efi64_image_handle = 20,
  image_load_base_physical_address = 21,
};

/** @brief One tag's header fields plus its raw, unpadded payload bytes
 * (immediately following the 8-byte `type`/`size` header), borrowed
 * directly from whatever span the owning `multiboot2_boot_info_reader`
 * was bound to. */
struct multiboot2_tag {
  multiboot2_tag_type type{};
  /** @brief This tag's total size in bytes, header included, *not*
   * rounded up to the mandatory 8-byte inter-tag padding -- exactly the
   * `size` field as the spec defines it. */
  uint32_t size{};
  /** @brief The `size - 8` payload bytes following the tag header. */
  span<const std::byte> payload{};
};

/**
 * @brief Rust-style, bounds-checked iterable view over a Multiboot2 boot
 * information structure held in a caller-owned `span<const std::byte>`.
 *
 * Mirrors `structo::fdt::fdt_reader`'s shape: `try_create` validates the
 * fixed 8-byte prefix (`total_size`/`reserved`) once; iterating
 * (`next()`/range-for) bounds-checks every tag header and payload length
 * against the span on every step, surfacing a malformed tag as a
 * `reloco::error` item exactly once, then the iterator is permanently
 * exhausted.
 */
class RELOCO_POINTER multiboot2_boot_info_reader
    : public iterator_adaptor<multiboot2_boot_info_reader, result<multiboot2_tag>> {
public:
  using item_type = result<multiboot2_tag>;

  /**
   * @brief Fallible factory: validates the fixed 8-byte `total_size`/
   * `reserved` prefix (and that `total_size` fits within @p info) before
   * any tag is parsed. @p info must outlive the returned reader and
   * every `multiboot2_tag` it yields.
   * @return `error::out_of_bounds` if @p info is shorter than 8 bytes or
   * shorter than its own declared `total_size`.
   */
  [[nodiscard]] static result<multiboot2_boot_info_reader> try_create(span<const std::byte> info) noexcept {
    if (info.size() < 8) // Fixed `total_size`/`reserved` prefix, always present.
      return unexpected(error::out_of_bounds);
    auto total_size_r = detail::read_u32_at(info, 0);
    if (!total_size_r)
      return unexpected(total_size_r.error());
    if (*total_size_r < 8)
      return unexpected(error::out_of_bounds);
    auto blob_r = info.try_subspan(0, static_cast<std::size_t>(*total_size_r));
    if (!blob_r)
      return unexpected(error::out_of_bounds);
    return multiboot2_boot_info_reader(*blob_r);
  }

  /** @brief The validated prefix of the span passed to `try_create`
   * (i.e. `info.subspan(0, total_size)`). */
  [[nodiscard]] span<const std::byte> blob() const noexcept RELOCO_LIFETIMEBOUND { return blob_; }

  /** @brief `Derived::next_impl()` primitive required by
   * `reloco::iterator_adaptor`; use `next()`/range-for instead of
   * calling this directly. */
  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (done_)
      return nullopt;

    auto type_r = detail::read_u32_at(blob_, cursor_);
    if (!type_r)
      return fail(type_r.error());
    auto size_r = detail::read_u32_at(blob_, cursor_ + 4);
    if (!size_r)
      return fail(size_r.error());
    const uint32_t size = *size_r;
    if (size < 8)
      return fail(error::invalid_argument); // Every tag's size includes its own 8-byte header.

    auto payload_r = blob_.try_subspan(cursor_ + 8, static_cast<std::size_t>(size) - 8);
    if (!payload_r)
      return fail(error::out_of_bounds);

    const auto type = static_cast<multiboot2_tag_type>(*type_r);
    // `payload_r`'s success already proves `cursor_ + size` fits within
    // `blob_`, so padding up to the next 8-byte boundary cannot overflow.
    cursor_ = detail::align8(cursor_ + size);

    if (type == multiboot2_tag_type::end) {
      done_ = true;
      // The end tag is a real, 8-byte tag (not silently swallowed) --
      // yielded once, like `fdt_reader` yields its last real event
      // before reporting exhausted on the following `next()` call.
    }

    multiboot2_tag tag{};
    tag.type = type;
    tag.size = size;
    tag.payload = *payload_r;
    return optional<item_type>(item_type(tag));
  }

private:
  explicit multiboot2_boot_info_reader(span<const std::byte> blob) noexcept : blob_(blob) {}

  [[nodiscard]] optional<item_type> fail(error e) noexcept {
    done_ = true;
    return optional<item_type>(item_type(unexpected(e)));
  }

  span<const std::byte> blob_;
  std::size_t cursor_ = 8;
  bool done_ = false;
};

// ============================================================================
// The memory map tag (type 6), the one tag this basic driver interprets
// beyond its raw header/payload.
// ============================================================================

/**
 * @brief One BIOS-`e820`-style memory map entry, decoded from a
 * `memory_map`-type tag's payload by `multiboot2_mmap_entries`.
 */
struct multiboot2_mmap_entry {
  static constexpr uint32_t type_available = 1;
  static constexpr uint32_t type_reserved = 2;
  static constexpr uint32_t type_acpi_reclaimable = 3;
  static constexpr uint32_t type_nvs = 4;
  static constexpr uint32_t type_bad_ram = 5;

  uint64_t base_addr{};
  uint64_t length{};
  uint32_t type{};

  /** @brief Whether this range is ordinary, immediately usable RAM
   * (`type_available`) -- everything else (ACPI-reclaimable, NVS, bad,
   * or a vendor-specific reserved type) is not free to hand to a page
   * allocator without further handling. */
  [[nodiscard]] constexpr bool is_available() const noexcept { return type == type_available; }
};

/**
 * @brief Rust-style, bounds-checked iterable view over a `memory_map`
 * (type 6) tag's entries.
 *
 * Each entry is `entry_size` bytes wide (the spec reserves room for this
 * to grow beyond the fixed 24 bytes `multiboot2_mmap_entry` decodes;
 * any trailing bytes of a wider entry are simply skipped), and the
 * payload's first 8 bytes are the tag-local `entry_size`/`entry_version`
 * header this iterator validates once in `multiboot2_mmap_entries`
 * before iterating.
 */
class RELOCO_POINTER multiboot2_mmap_iterator
    : public iterator_adaptor<multiboot2_mmap_iterator, result<multiboot2_mmap_entry>> {
public:
  using item_type = result<multiboot2_mmap_entry>;

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (done_)
      return nullopt;
    if (cursor_ + entry_size_ > entries_.size()) {
      done_ = true;
      return nullopt;
    }

    auto base_r = detail::read_u64_at(entries_, cursor_);
    if (!base_r)
      return fail(base_r.error());
    auto length_r = detail::read_u64_at(entries_, cursor_ + 8);
    if (!length_r)
      return fail(length_r.error());
    auto type_r = detail::read_u32_at(entries_, cursor_ + 16);
    if (!type_r)
      return fail(type_r.error());

    cursor_ += entry_size_;

    multiboot2_mmap_entry entry{};
    entry.base_addr = *base_r;
    entry.length = *length_r;
    entry.type = *type_r;
    return optional<item_type>(item_type(entry));
  }

private:
  friend result<multiboot2_mmap_iterator> multiboot2_mmap_entries(const multiboot2_tag &tag) noexcept;

  multiboot2_mmap_iterator(span<const std::byte> entries, std::size_t entry_size) noexcept
      : entries_(entries), entry_size_(entry_size) {}

  [[nodiscard]] optional<item_type> fail(error e) noexcept {
    done_ = true;
    return optional<item_type>(item_type(unexpected(e)));
  }

  span<const std::byte> entries_;
  std::size_t entry_size_;
  std::size_t cursor_ = 0;
  bool done_ = false;
};

/**
 * @brief Builds an iterator over @p tag's entries.
 * @param tag Must be `multiboot2_tag_type::memory_map`.
 * @return `error::invalid_argument` if @p tag is not a `memory_map` tag,
 * if its payload is shorter than the 8-byte `entry_size`/`entry_version`
 * header, or if `entry_size` is smaller than the 24 bytes a
 * `multiboot2_mmap_entry` needs (`base_addr`/`length`/`type`).
 */
[[nodiscard]] inline result<multiboot2_mmap_iterator> multiboot2_mmap_entries(const multiboot2_tag &tag) noexcept {
  if (tag.type != multiboot2_tag_type::memory_map)
    return unexpected(error::invalid_argument);
  auto entry_size_r = detail::read_u32_at(tag.payload, 0);
  if (!entry_size_r)
    return unexpected(entry_size_r.error());
  if (*entry_size_r < 24)
    return unexpected(error::invalid_argument);
  auto entries_r = tag.payload.try_subspan(8, tag.payload.size() - 8);
  if (!entries_r)
    return unexpected(error::out_of_bounds);
  return multiboot2_mmap_iterator(*entries_r, static_cast<std::size_t>(*entry_size_r));
}

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::arch::x86
