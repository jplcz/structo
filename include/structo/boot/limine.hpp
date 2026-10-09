// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file limine.hpp
 * @brief Limine boot protocol (https://github.com/limine-bootloader/limine-protocol)
 * support: the kernel-embedded *request* records, and a bounds-checked,
 * pointer-free reader (`structo::boot::limine::reader`) that decodes the
 * bootloader's *responses* into plain value types.
 *
 * ## How the protocol works
 *
 * Unlike Multiboot2 (one info blob handed over in a register), Limine is
 * request/response: the kernel embeds *request* records in its image
 * (each starts with a 4x`uint64_t` magic id, a `revision` and a
 * `response` slot), the bootloader scans the image for them, and for every
 * request it understands fills `response` with the (virtual) address of a
 * response record. The kernel then reads `response` -- 0 means "not
 * answered".
 *
 * ## No raw pointers
 *
 * Every address field in this header (`response`, `address`, `entries`,
 * ...) is a plain `uint64_t`, i.e. the `LIMINE_NO_POINTERS` form of
 * `limine.h`: the on-wire layout is identical but nothing here ever
 * dereferences one. Turning an address into readable bytes is the
 * caller's job, expressed as a *resolver*:
 *
 * @code
 * // A resolver maps a (virtual) address range the bootloader published to
 * // a span over it, or fails. `address` is a value found in a response
 * // record (HHDM-relative on x86_64/aarch64/riscv64), `size` is how many
 * // bytes the reader is about to read. Return an error for anything you
 * // did not map -- the reader then reports it instead of faulting.
 * struct my_resolver {
 *   reloco::result<reloco::span<const std::byte>> operator()(uint64_t address, std::size_t size) const noexcept {
 *     // In a kernel with a direct map: translate and bounds-check here,
 *     // then build the span from your own mapping primitive.
 *     return my_direct_map.try_view(address, size);
 *   }
 * };
 * @endcode
 *
 * ## Requests
 *
 * Requests must be `volatile` (the bootloader writes `response` behind the
 * compiler's back) and kept in the kernel image between
 * `requests_start_marker`/`requests_end_marker`:
 *
 * @code
 * // Base revision tag: tells the bootloader which protocol revision this
 * // kernel speaks (3 = current). The bootloader zeroes word [2] if it
 * // supports it; check with base_revision_supported().
 * alignas(8) inline volatile auto base_rev = structo::boot::limine::make_base_revision(3);
 *
 * // Ask for the physical memory map. Revision 0 is the only one defined.
 * alignas(8) inline volatile auto memmap_req = structo::boot::limine::make_request<structo::boot::limine::memmap_request>(0);
 *
 * // After entry: decode. `base_rev` proves the bootloader understood us.
 * structo::boot::limine::reader<my_resolver> rd{my_resolver{}};
 * if (structo::boot::limine::base_revision_supported(base_rev)) {
 *   auto entries = rd.try_memmap(memmap_req); // result<memmap_iterator>
 * }
 * @endcode
 *
 * Request revisions follow the protocol document; this header sets the
 * `revision` the caller passes and never raises it implicitly.
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
#include <reloco/string_view.hpp>
#include <structo/boot/memory_kind.hpp>
#include <structo/detail/boot_bytes.hpp>
#include <type_traits>

namespace structo::boot::limine {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

// ============================================================================
// Request records (kernel -> bootloader)
// ============================================================================

inline constexpr uint64_t common_magic_0 = 0xc7b1dd30df4c8b88;
inline constexpr uint64_t common_magic_1 = 0x0a82e883a194f07b;

/** @brief 32-byte request identifier: the common magic plus two words unique to each request type. */
using request_id = array<uint64_t, 4>;

/** @brief Marks the start of the request region (optional, speeds up the bootloader's scan). */
inline constexpr array<uint64_t, 4> requests_start_marker = {0xf6b8f4b39de7d1ae, 0xfab91a6940fcb9cf,
                                                             0x785c6ed015d3e316, 0x181e920a7852b9d9};
/** @brief Marks the end of the request region. */
inline constexpr array<uint64_t, 2> requests_end_marker = {0xadc0e0531bb10d03, 0x9572709f31764c62};

/** @brief The 3-word base-revision tag a kernel embeds: `{magic0, magic1, revision}`. */
struct base_revision_tag {
  uint64_t magic0;
  uint64_t magic1;
  uint64_t revision;
};

/** @brief Builds the 3-word base-revision tag: `{magic0, magic1, N}`. The bootloader sets word 2 to 0 when it
 * supports revision @p revision. */
[[nodiscard]] constexpr base_revision_tag make_base_revision(uint64_t revision) noexcept {
  return {0xf9562b2d5c95a6c8, 0x6a7b384944536bdc, revision};
}

/** @brief Whether the bootloader accepted the requested base revision (word 2 cleared). */
[[nodiscard]] inline bool base_revision_supported(const volatile base_revision_tag &tag) noexcept {
  return tag.revision == 0;
}

/** @brief The common 6-word prefix of every request record. */
struct request_header {
  request_id id{};
  uint64_t revision = 0;
  /** @brief Address of the response record; 0 until the bootloader answers. */
  uint64_t response = 0;
};

#define STRUCTO_LIMINE_REQUEST_(NAME, W2, W3)                                                                          \
  struct NAME##_request : request_header {                                                                             \
    static constexpr request_id request_type_id = {common_magic_0, common_magic_1, W2, W3};                            \
  }

STRUCTO_LIMINE_REQUEST_(bootloader_info, 0xf55038d8e2a1202f, 0x279426fcf5f59740);
STRUCTO_LIMINE_REQUEST_(executable_cmdline, 0x4b161536e598651e, 0xb390ad4a2f1f303a);
STRUCTO_LIMINE_REQUEST_(firmware_type, 0x8c2f75d90bef28a8, 0x7045a4688eac00c3);
STRUCTO_LIMINE_REQUEST_(hhdm, 0x48dcf1cb8ad2b852, 0x63984e959a98244b);
STRUCTO_LIMINE_REQUEST_(framebuffer, 0x9d5827dcd881dd75, 0xa3148604f6fab11b);
STRUCTO_LIMINE_REQUEST_(memmap, 0x67cf3d9d378a806f, 0xe304acdfc50c3c62);
STRUCTO_LIMINE_REQUEST_(executable_file, 0xad97e90e83f1ed67, 0x31eb5d1c5ff23b69);
STRUCTO_LIMINE_REQUEST_(rsdp, 0xc5e77b6b397e7b43, 0x27637845accdcf3c);
STRUCTO_LIMINE_REQUEST_(smbios, 0x9e9046f11e095391, 0xaa4a520fefbde5ee);
STRUCTO_LIMINE_REQUEST_(efi_system_table, 0x5ceba5163eaaf6d6, 0x0a6981610cf65fcc);
STRUCTO_LIMINE_REQUEST_(efi_memmap, 0x7df62a431d6872d5, 0xa4fcdfb3e57306c8);
STRUCTO_LIMINE_REQUEST_(date_at_boot, 0x502746e184c088aa, 0xfbc5ec83e6327893);
STRUCTO_LIMINE_REQUEST_(executable_address, 0x71ba76863cc55f63, 0xb2644a48c516a487);
STRUCTO_LIMINE_REQUEST_(dtb, 0xb40ddb48fb54bac7, 0x545081493f81ffb7);
STRUCTO_LIMINE_REQUEST_(riscv_bsp_hartid, 0x1369359f025525f9, 0x2ff2a56178391bb6);
STRUCTO_LIMINE_REQUEST_(tpm_event_log, 0x98e094fc7e76e979, 0xee8d8775c54e1d1f);
STRUCTO_LIMINE_REQUEST_(tsc_frequency, 0x10f2ee1d87d195e4, 0xf747a2b78f6ddb31);
STRUCTO_LIMINE_REQUEST_(bootloader_performance, 0x6b50ad9bf36d13ad, 0xdc4c7e88fc759e17);

#undef STRUCTO_LIMINE_REQUEST_

/** @brief Stack-size request: the extra `stack_size` field asks for a kernel stack of that many bytes. */
struct stack_size_request : request_header {
  static constexpr request_id request_type_id = {common_magic_0, common_magic_1, 0x224ef0460a8e8926,
                                                 0xe1cb0fc25f46ea3d};
  uint64_t stack_size = 0;
};

/** @brief Paging-mode request: preferred/maximum/minimum mode (values are architecture specific, see `paging_*`). */
struct paging_mode_request : request_header {
  static constexpr request_id request_type_id = {common_magic_0, common_magic_1, 0x95c1a0edab0944cb,
                                                 0xa4e5cb3842f7488a};
  uint64_t mode = 0;
  uint64_t max_mode = 0;
  uint64_t min_mode = 0;
};

/** @brief MP (SMP) request: `flags` bit 0 (`mp_request_x2apic`) asks for x2APIC on x86_64. */
struct mp_request : request_header {
  static constexpr request_id request_type_id = {common_magic_0, common_magic_1, 0x95a67b819a1b857e,
                                                 0xa0b61b723b6a73e0};
  uint64_t flags = 0;
};
inline constexpr uint64_t mp_request_x2apic = 1u << 0;

/** @brief Entry-point request: `entry` is the address the bootloader should jump to instead of the ELF entry. */
struct entry_point_request : request_header {
  static constexpr request_id request_type_id = {common_magic_0, common_magic_1, 0x13d86c035a1cd3e1,
                                                 0x2b0caa89d8f3026a};
  uint64_t entry = 0;
};

/** @brief Module request (revision 1 adds internal modules: an array of `internal_module` records). */
struct module_request : request_header {
  static constexpr request_id request_type_id = {common_magic_0, common_magic_1, 0x3e7e279702be32af,
                                                 0xca1c4f3bd1280cee};
  uint64_t internal_module_count = 0;
  /** @brief Address of an array of addresses of `internal_module` records. */
  uint64_t internal_modules = 0;
};

/** @brief Internal module record (referenced by `module_request`). */
struct internal_module {
  uint64_t path = 0;   ///< Address of a NUL-terminated path.
  uint64_t string = 0; ///< Address of a NUL-terminated string passed through to the module.
  uint64_t flags = 0;
};
inline constexpr uint64_t internal_module_required = 1u << 0;
inline constexpr uint64_t internal_module_compressed = 1u << 1;

/** @brief Builds a request of type @p Request with request id and `revision` filled in. Any extra
 * fields are value-initialized; set them on the returned object before placing it in the image
 * (e.g. `auto r = make_request<stack_size_request>(0); r.stack_size = 0x10000;`). */
template <typename Request> [[nodiscard]] constexpr Request make_request(uint64_t revision) noexcept {
  Request r{};
  r.id = Request::request_type_id;
  r.revision = revision;
  return r;
}

/** @brief Address of the response record @p req received, read through `volatile` because the
 * bootloader writes it behind the compiler's back. 0 means unanswered. */
template <typename Request> [[nodiscard]] inline uint64_t response_address(const volatile Request &req) noexcept {
  return req.response;
}

// ============================================================================
// Decoded value types
// ============================================================================

/** @brief `limine_firmware_type_response::firmware_type` values. */
enum class firmware_type : uint64_t { x86_bios = 0, efi32 = 1, efi64 = 2, sbi = 3 };

/** @brief `limine_memmap_entry::type` values. */
enum class memmap_type : uint64_t {
  usable = 0,
  reserved = 1,
  acpi_reclaimable = 2,
  acpi_nvs = 3,
  bad_memory = 4,
  bootloader_reclaimable = 5,
  executable_and_modules = 6,
  framebuffer = 7,
  reserved_mapped = 8,
};

/** @brief Maps a Limine memmap type onto the protocol-neutral `memory_kind`. Unknown values map to `reserved`. */
[[nodiscard]] constexpr memory_kind to_memory_kind(uint64_t type) noexcept {
  switch (static_cast<memmap_type>(type)) {
  case memmap_type::usable:
    return memory_kind::usable;
  case memmap_type::acpi_reclaimable:
    return memory_kind::acpi_reclaimable;
  case memmap_type::acpi_nvs:
    return memory_kind::acpi_nvs;
  case memmap_type::bad_memory:
    return memory_kind::bad;
  case memmap_type::bootloader_reclaimable:
    return memory_kind::bootloader_reclaimable;
  case memmap_type::executable_and_modules:
    return memory_kind::kernel_and_modules;
  case memmap_type::framebuffer:
    return memory_kind::framebuffer;
  case memmap_type::reserved:
  case memmap_type::reserved_mapped:
    break;
  }
  return memory_kind::reserved;
}

/** @brief One decoded `limine_memmap_entry`. */
struct memmap_entry {
  uint64_t base = 0;
  uint64_t length = 0;
  uint64_t type = 0;

  [[nodiscard]] constexpr memory_kind kind() const noexcept { return to_memory_kind(type); }
};

/** @brief Decoded `limine_hhdm_response`: the virtual offset of the higher-half direct map. */
struct hhdm_info {
  uint64_t offset = 0;
};

/** @brief Decoded `limine_executable_address_response`. */
struct executable_address_info {
  uint64_t physical_base = 0;
  uint64_t virtual_base = 0;
};

/** @brief Decoded `limine_framebuffer` (RGB, memory_model 1). `address` is a virtual address. */
struct framebuffer_info {
  uint64_t address = 0;
  uint64_t width = 0;
  uint64_t height = 0;
  uint64_t pitch = 0;
  uint16_t bpp = 0;
  uint8_t memory_model = 0;
  uint8_t red_mask_size = 0;
  uint8_t red_mask_shift = 0;
  uint8_t green_mask_size = 0;
  uint8_t green_mask_shift = 0;
  uint8_t blue_mask_size = 0;
  uint8_t blue_mask_shift = 0;
  uint64_t edid_size = 0;
  uint64_t edid = 0; ///< Address of the EDID blob (0 if none).
};

/** @brief `limine_uuid`-formatted GUID (little-endian first three fields, as in UEFI). */
struct uuid {
  uint32_t a = 0;
  uint16_t b = 0;
  uint16_t c = 0;
  array<uint8_t, 8> d{};
};

/** @brief Decoded `limine_file` (module or executable file). */
struct file_info {
  uint64_t revision = 0;
  uint64_t address = 0; ///< Virtual address of the file contents.
  uint64_t size = 0;
  uint64_t path = 0;   ///< Address of NUL-terminated path.
  uint64_t string = 0; ///< Address of NUL-terminated cmdline string.
  uint32_t media_type = 0;
  uint32_t partition_index = 0;
  uint32_t mbr_disk_id = 0;
  uuid gpt_disk_uuid{};
  uuid gpt_part_uuid{};
  uuid part_uuid{};
};

/** @brief Decoded SMBIOS entry points (0 when the corresponding table is absent). */
struct smbios_info {
  uint64_t entry_32 = 0;
  uint64_t entry_64 = 0;
};

/** @brief Decoded EFI memory map response: the raw descriptor array's address and geometry. Feed it to
 * `structo::boot::uefi::memory_map_reader` after resolving the bytes. */
struct efi_memmap_info {
  uint64_t memmap = 0;
  uint64_t memmap_size = 0;
  uint64_t desc_size = 0;
  uint64_t desc_version = 0;
};

/** @brief Decoded TPM event log response. */
struct tpm_event_log_info {
  uint64_t format = 0; ///< 1 = TCG 1.2, 2 = TCG 2.
  uint64_t size = 0;
  uint64_t address = 0;
};

/** @brief Decoded one-CPU record of an MP response, normalized across architectures. */
struct mp_cpu {
  uint64_t processor_id = 0;
  /** @brief LAPIC id (x86_64), MPIDR (aarch64) or hartid (riscv64). */
  uint64_t hw_id = 0;
  /** @brief Address of the `goto_address` field: write the entry function's address *there* to start this CPU. */
  uint64_t goto_address_field = 0;
  /** @brief Address of the `extra_argument` field, passed to the started CPU. */
  uint64_t extra_argument_field = 0;
};

/** @brief CPU architecture selecting the MP record layout (the protocol defines a different struct per arch). */
enum class mp_arch { x86_64, aarch64, riscv64 };

// ============================================================================
// Reader
// ============================================================================

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace detail {

[[nodiscard]] inline uuid decode_uuid(span<const std::byte> record, std::size_t offset) noexcept {
  uuid u;
  u.a = boot_bytes::read_le_at<uint32_t>(record, offset).value_or(0);
  u.b = boot_bytes::read_le_at<uint16_t>(record, offset + 4).value_or(0);
  u.c = boot_bytes::read_le_at<uint16_t>(record, offset + 6).value_or(0);
  for (std::size_t i = 0; i < 8; ++i)
    u.d[i] = boot_bytes::read_le_at<uint8_t>(record, offset + 8 + i).value_or(0);
  return u;
}

/** @brief `limine_memmap_entry` (24 bytes) decoder. */
struct memmap_decoder {
  using value_type = memmap_entry;
  static constexpr std::size_t record_size = 24;
  [[nodiscard]] static result<memmap_entry> decode(uint64_t, span<const std::byte> r) noexcept {
    auto base = boot_bytes::read_le_at<uint64_t>(r, 0);
    auto length = boot_bytes::read_le_at<uint64_t>(r, 8);
    auto type = boot_bytes::read_le_at<uint64_t>(r, 16);
    if (!base || !length || !type)
      return unexpected(error::out_of_bounds);
    return memmap_entry{*base, *length, *type};
  }
};

/** @brief `limine_framebuffer` decoder (the fixed 64-byte head; revision-1 video-mode list is not decoded). */
struct framebuffer_decoder {
  using value_type = framebuffer_info;
  static constexpr std::size_t record_size = 64;
  [[nodiscard]] static result<framebuffer_info> decode(uint64_t, span<const std::byte> r) noexcept {
    namespace d = structo::boot::detail;
    framebuffer_info f;
    auto address = d::read_le_at<uint64_t>(r, 0);
    auto width = d::read_le_at<uint64_t>(r, 8);
    auto height = d::read_le_at<uint64_t>(r, 16);
    auto pitch = d::read_le_at<uint64_t>(r, 24);
    auto bpp = d::read_le_at<uint16_t>(r, 32);
    auto edid_size = d::read_le_at<uint64_t>(r, 48);
    auto edid = d::read_le_at<uint64_t>(r, 56);
    if (!address || !width || !height || !pitch || !bpp || !edid_size || !edid)
      return unexpected(error::out_of_bounds);
    f.address = *address;
    f.width = *width;
    f.height = *height;
    f.pitch = *pitch;
    f.bpp = *bpp;
    f.memory_model = d::read_le_at<uint8_t>(r, 34).value_or(0);
    f.red_mask_size = d::read_le_at<uint8_t>(r, 35).value_or(0);
    f.red_mask_shift = d::read_le_at<uint8_t>(r, 36).value_or(0);
    f.green_mask_size = d::read_le_at<uint8_t>(r, 37).value_or(0);
    f.green_mask_shift = d::read_le_at<uint8_t>(r, 38).value_or(0);
    f.blue_mask_size = d::read_le_at<uint8_t>(r, 39).value_or(0);
    f.blue_mask_shift = d::read_le_at<uint8_t>(r, 40).value_or(0);
    f.edid_size = *edid_size;
    f.edid = *edid;
    return f;
  }
};

/** @brief `limine_file` (112 bytes) decoder. */
struct file_decoder {
  using value_type = file_info;
  static constexpr std::size_t record_size = 112;
  [[nodiscard]] static result<file_info> decode(uint64_t, span<const std::byte> r) noexcept {
    namespace d = structo::boot::detail;
    file_info f;
    auto revision = d::read_le_at<uint64_t>(r, 0);
    auto address = d::read_le_at<uint64_t>(r, 8);
    auto size = d::read_le_at<uint64_t>(r, 16);
    auto path = d::read_le_at<uint64_t>(r, 24);
    auto string = d::read_le_at<uint64_t>(r, 32);
    auto media_type = d::read_le_at<uint32_t>(r, 40);
    auto part_index = d::read_le_at<uint32_t>(r, 56);
    auto mbr = d::read_le_at<uint32_t>(r, 60);
    if (!revision || !address || !size || !path || !string || !media_type || !part_index || !mbr)
      return unexpected(error::out_of_bounds);
    f.revision = *revision;
    f.address = *address;
    f.size = *size;
    f.path = *path;
    f.string = *string;
    f.media_type = *media_type;
    f.partition_index = *part_index;
    f.mbr_disk_id = *mbr;
    f.gpt_disk_uuid = decode_uuid(r, 64);
    f.gpt_part_uuid = decode_uuid(r, 80);
    f.part_uuid = decode_uuid(r, 96);
    return f;
  }
};

/** @brief Per-architecture `limine_mp_info` decoder. */
template <mp_arch Arch> struct mp_decoder {
  using value_type = mp_cpu;
  static constexpr std::size_t record_size = Arch == mp_arch::x86_64 ? 32 : 40;
  [[nodiscard]] static result<mp_cpu> decode(uint64_t address, span<const std::byte> r) noexcept {
    namespace d = structo::boot::detail;
    mp_cpu c;
    if constexpr (Arch == mp_arch::x86_64) {
      auto id = d::read_le_at<uint32_t>(r, 0);
      auto lapic = d::read_le_at<uint32_t>(r, 4);
      if (!id || !lapic)
        return unexpected(error::out_of_bounds);
      c.processor_id = *id;
      c.hw_id = *lapic;
      c.goto_address_field = address + 16;
      c.extra_argument_field = address + 24;
    } else if constexpr (Arch == mp_arch::aarch64) {
      auto id = d::read_le_at<uint32_t>(r, 0);
      auto mpidr = d::read_le_at<uint64_t>(r, 8);
      if (!id || !mpidr)
        return unexpected(error::out_of_bounds);
      c.processor_id = *id;
      c.hw_id = *mpidr;
      c.goto_address_field = address + 24;
      c.extra_argument_field = address + 32;
    } else {
      auto id = d::read_le_at<uint64_t>(r, 0);
      auto hart = d::read_le_at<uint64_t>(r, 8);
      if (!id || !hart)
        return unexpected(error::out_of_bounds);
      c.processor_id = *id;
      c.hw_id = *hart;
      c.goto_address_field = address + 24;
      c.extra_argument_field = address + 32;
    }
    return c;
  }
};

} // namespace detail

/** @brief Resolver contract: `result<span<const std::byte>>(uint64_t address, std::size_t size)`. */
template <typename R>
inline constexpr bool is_resolver_v =
    std::is_invocable_r_v<result<span<const std::byte>>, const R &, uint64_t, std::size_t>;

/**
 * @brief Bounds-checked, pointer-free decoder for Limine response records.
 * @tparam Resolver See the file documentation: maps `(address, size)` to a readable span.
 */
template <typename Resolver> class reader {
  static_assert(is_resolver_v<Resolver>, "Resolver must be callable as result<span<const std::byte>>(uint64_t, size_t)");

public:
  explicit reader(Resolver resolver) noexcept : resolve_(static_cast<Resolver &&>(resolver)) {}

  /** @brief Whether @p req was answered (non-zero response address). */
  template <typename Request> [[nodiscard]] static bool answered(const volatile Request &req) noexcept {
    return response_address(req) != 0;
  }

  /** @brief Reads a NUL-terminated string at @p address, at most @p max_length bytes (default 4 KiB).
   * The returned view borrows from whatever the resolver mapped. */
  [[nodiscard]] result<string_view> try_cstring(uint64_t address, std::size_t max_length = 4096) const noexcept {
    if (address == 0)
      return unexpected(error::not_found);
    std::size_t length = 0;
    for (;; ++length) {
      if (length >= max_length)
        return unexpected(error::out_of_range);
      auto byte = resolve_(address + length, 1);
      if (!byte)
        return unexpected(byte.error());
      if ((*byte)[0] == std::byte{0})
        break;
    }
    auto whole = resolve_(address, length);
    if (!whole)
      return unexpected(whole.error());
    return string_view(reinterpret_cast<const char *>(whole->data()), length);
  }

  /** @brief `limine_bootloader_info_response`: bootloader `name` and `version`. */
  struct bootloader_info {
    string_view name;
    string_view version;
  };
  [[nodiscard]] result<bootloader_info> try_bootloader_info(const volatile bootloader_info_request &req) const noexcept {
    auto r = response(req, 24);
    if (!r)
      return unexpected(r.error());
    auto name = word(*r, 8);
    auto version = word(*r, 16);
    if (!name || !version)
      return unexpected(error::out_of_bounds);
    auto n = try_cstring(*name);
    if (!n)
      return unexpected(n.error());
    auto v = try_cstring(*version);
    if (!v)
      return unexpected(v.error());
    return bootloader_info{*n, *v};
  }

  /** @brief The kernel command line (`executable_cmdline` request). */
  [[nodiscard]] result<string_view> try_cmdline(const volatile executable_cmdline_request &req) const noexcept {
    auto r = response(req, 16);
    if (!r)
      return unexpected(r.error());
    auto p = word(*r, 8);
    if (!p)
      return unexpected(p.error());
    return try_cstring(*p);
  }

  [[nodiscard]] result<firmware_type> try_firmware_type(const volatile firmware_type_request &req) const noexcept {
    auto r = response(req, 16);
    if (!r)
      return unexpected(r.error());
    auto t = word(*r, 8);
    if (!t)
      return unexpected(t.error());
    return static_cast<firmware_type>(*t);
  }

  [[nodiscard]] result<hhdm_info> try_hhdm(const volatile hhdm_request &req) const noexcept {
    auto r = response(req, 16);
    if (!r)
      return unexpected(r.error());
    auto offset = word(*r, 8);
    if (!offset)
      return unexpected(offset.error());
    return hhdm_info{*offset};
  }

  [[nodiscard]] result<executable_address_info>
  try_executable_address(const volatile executable_address_request &req) const noexcept {
    auto r = response(req, 24);
    if (!r)
      return unexpected(r.error());
    auto p = word(*r, 8);
    auto v = word(*r, 16);
    if (!p || !v)
      return unexpected(error::out_of_bounds);
    return executable_address_info{*p, *v};
  }

  /** @brief RSDP address (virtual on revision >= 3, physical before; per the protocol revision). */
  [[nodiscard]] result<uint64_t> try_rsdp(const volatile rsdp_request &req) const noexcept {
    return single_address(req);
  }
  [[nodiscard]] result<uint64_t> try_efi_system_table(const volatile efi_system_table_request &req) const noexcept {
    return single_address(req);
  }
  [[nodiscard]] result<uint64_t> try_dtb(const volatile dtb_request &req) const noexcept { return single_address(req); }

  [[nodiscard]] result<smbios_info> try_smbios(const volatile smbios_request &req) const noexcept {
    auto r = response(req, 24);
    if (!r)
      return unexpected(r.error());
    auto e32 = word(*r, 8);
    auto e64 = word(*r, 16);
    if (!e32 || !e64)
      return unexpected(error::out_of_bounds);
    return smbios_info{*e32, *e64};
  }

  [[nodiscard]] result<efi_memmap_info> try_efi_memmap(const volatile efi_memmap_request &req) const noexcept {
    auto r = response(req, 40);
    if (!r)
      return unexpected(r.error());
    efi_memmap_info out;
    auto a = word(*r, 8);
    auto b = word(*r, 16);
    auto c = word(*r, 24);
    auto d = word(*r, 32);
    if (!a || !b || !c || !d)
      return unexpected(error::out_of_bounds);
    out.memmap = *a;
    out.memmap_size = *b;
    out.desc_size = *c;
    out.desc_version = *d;
    return out;
  }

  /** @brief Seconds since the Unix epoch at boot (`date_at_boot` request). */
  [[nodiscard]] result<int64_t> try_date_at_boot(const volatile date_at_boot_request &req) const noexcept {
    auto r = response(req, 16);
    if (!r)
      return unexpected(r.error());
    auto t = word(*r, 8);
    if (!t)
      return unexpected(t.error());
    return static_cast<int64_t>(*t);
  }

  [[nodiscard]] result<uint64_t> try_riscv_bsp_hartid(const volatile riscv_bsp_hartid_request &req) const noexcept {
    auto r = response(req, 16);
    if (!r)
      return unexpected(r.error());
    return word(*r, 8);
  }

  [[nodiscard]] result<uint64_t> try_tsc_frequency(const volatile tsc_frequency_request &req) const noexcept {
    auto r = response(req, 16);
    if (!r)
      return unexpected(r.error());
    return word(*r, 8);
  }

  [[nodiscard]] result<tpm_event_log_info> try_tpm_event_log(const volatile tpm_event_log_request &req) const noexcept {
    auto r = response(req, 32);
    if (!r)
      return unexpected(r.error());
    auto f = word(*r, 8);
    auto s = word(*r, 16);
    auto a = word(*r, 24);
    if (!f || !s || !a)
      return unexpected(error::out_of_bounds);
    return tpm_event_log_info{*f, *s, *a};
  }

  /** @brief Iterates a pointer-array response (`memmap`, `framebuffer`, `module`, `mp`): element `i` is
   * read by fetching the record address stored at `array[i]`, resolving that record and decoding it with
   * `Decoder`. Holds a copy of the resolver, never a pointer to it. */
  template <typename Decoder> class record_iterator : public iterator_adaptor<record_iterator<Decoder>, result<typename Decoder::value_type>> {
  public:
    using value_type = typename Decoder::value_type;
    using item_type = result<value_type>;

    [[nodiscard]] optional<item_type> next_impl() noexcept {
      if (index_ >= count_)
        return nullopt;
      const uint64_t slot_address = array_ + index_ * 8;
      ++index_;
      auto slot = resolve_(slot_address, 8);
      if (!slot)
        return optional<item_type>(item_type(unexpected(slot.error())));
      auto record_address = boot_bytes::read_le_at<uint64_t>(*slot, 0);
      if (!record_address)
        return optional<item_type>(item_type(unexpected(record_address.error())));
      auto record = resolve_(*record_address, Decoder::record_size);
      if (!record)
        return optional<item_type>(item_type(unexpected(record.error())));
      return optional<item_type>(Decoder::decode(*record_address, *record));
    }

    /** @brief Number of records the response declares. */
    [[nodiscard]] uint64_t count() const noexcept { return count_; }

  private:
    friend class reader;
    record_iterator(Resolver resolve, uint64_t array, uint64_t count) noexcept
        : resolve_(static_cast<Resolver &&>(resolve)), array_(array), count_(count) {}
    Resolver resolve_;
    uint64_t array_;
    uint64_t count_;
    uint64_t index_ = 0;
  };

  /** @brief The physical memory map as an iterator of `memmap_entry` (`limine_memmap_response`: count at +8, entries array at +16). */
  [[nodiscard]] result<record_iterator<detail::memmap_decoder>> try_memmap(const volatile memmap_request &req) const noexcept {
    return array_response<detail::memmap_decoder>(req);
  }

  /** @brief Framebuffers as an iterator of `framebuffer_info`. */
  [[nodiscard]] result<record_iterator<detail::framebuffer_decoder>> try_framebuffers(const volatile framebuffer_request &req) const noexcept {
    return array_response<detail::framebuffer_decoder>(req);
  }

  /** @brief Modules as an iterator of `file_info`. */
  [[nodiscard]] result<record_iterator<detail::file_decoder>> try_modules(const volatile module_request &req) const noexcept {
    return array_response<detail::file_decoder>(req);
  }

  /** @brief The kernel's own file (`executable_file` request). */
  [[nodiscard]] result<file_info> try_executable_file(const volatile executable_file_request &req) const noexcept {
    auto r = response(req, 16);
    if (!r)
      return unexpected(r.error());
    auto addr = word(*r, 8);
    if (!addr)
      return unexpected(addr.error());
    auto record = resolve_(*addr, detail::file_decoder::record_size);
    if (!record)
      return unexpected(record.error());
    return detail::file_decoder::decode(*addr, *record);
  }

  /** @brief Result of `try_mp`: the boot CPU id, flags, and an iterator over every CPU. */
  template <mp_arch Arch> struct mp_info {
    uint64_t flags = 0;
    /** @brief LAPIC id / MPIDR / hartid of the boot CPU (see `mp_cpu::hw_id`). */
    uint64_t bsp_hw_id = 0;
    record_iterator<detail::mp_decoder<Arch>> cpus;
  };

  /** @brief Decodes the MP response for architecture @p Arch (x86_64 layout differs from aarch64/riscv64). */
  template <mp_arch Arch> [[nodiscard]] result<mp_info<Arch>> try_mp(const volatile mp_request &req) const noexcept {
    constexpr bool x86 = Arch == mp_arch::x86_64;
    constexpr std::size_t header_size = x86 ? 32 : 40;
    auto r = response(req, header_size);
    if (!r)
      return unexpected(r.error());
    uint64_t flags = 0;
    uint64_t bsp = 0;
    if constexpr (x86) {
      auto f = boot_bytes::read_le_at<uint32_t>(*r, 8);
      auto b = boot_bytes::read_le_at<uint32_t>(*r, 12);
      if (!f || !b)
        return unexpected(error::out_of_bounds);
      flags = *f;
      bsp = *b;
    } else {
      auto f = word(*r, 8);
      auto b = word(*r, 16);
      if (!f || !b)
        return unexpected(error::out_of_bounds);
      flags = *f;
      bsp = *b;
    }
    auto count = word(*r, x86 ? 16 : 24);
    auto cpus = word(*r, x86 ? 24 : 32);
    if (!count || !cpus)
      return unexpected(error::out_of_bounds);
    return mp_info<Arch>{flags, bsp, record_iterator<detail::mp_decoder<Arch>>(resolve_, *cpus, *count)};
  }

  /** @brief Folds the whole Limine memory map into @p map via `boot_memory_map::try_add` (RAM kinds only). */
  template <typename Map> [[nodiscard]] result<void> try_fill_memory_map(const volatile memmap_request &req, Map &map) const noexcept {
    auto it = try_memmap(req);
    if (!it)
      return unexpected(it.error());
    for (auto entry : *it) {
      if (!entry)
        return unexpected(entry.error());
      if (auto added = map.try_add(entry->kind(), entry->base, entry->length); !added)
        return added;
    }
    return {};
  }

private:
  /** @brief Resolves @p req's response record, requiring at least @p size bytes. */
  template <typename Request>
  [[nodiscard]] result<span<const std::byte>> response(const volatile Request &req, std::size_t size) const noexcept {
    const uint64_t address = response_address(req);
    if (address == 0)
      return unexpected(error::not_found);
    return resolve_(address, size);
  }

  [[nodiscard]] static result<uint64_t> word(span<const std::byte> record, std::size_t offset) noexcept {
    return boot_bytes::read_le_at<uint64_t>(record, offset);
  }

  template <typename Request> [[nodiscard]] result<uint64_t> single_address(const volatile Request &req) const noexcept {
    auto r = response(req, 16);
    if (!r)
      return unexpected(r.error());
    return word(*r, 8);
  }

  template <typename Decoder, typename Request>
  [[nodiscard]] result<record_iterator<Decoder>> array_response(const volatile Request &req) const noexcept {
    auto r = response(req, 24);
    if (!r)
      return unexpected(r.error());
    auto count = word(*r, 8);
    auto array = word(*r, 16);
    if (!count || !array)
      return unexpected(error::out_of_bounds);
    return record_iterator<Decoder>(resolve_, *array, *count);
  }

  Resolver resolve_;
};

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::boot::limine
