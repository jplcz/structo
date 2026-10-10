// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file arm_tf_fip.hpp
 * @brief Arm Trusted Firmware (TF-A) Firmware Image Package (FIP) parser and
 * ToC writer, matching `include/tools_share/firmware_image_package.h` and
 * `fiptool`.
 *
 * ## Layout (all little-endian)
 *
 * | offset | field |
 * |---|---|
 * | 0 | ToC header: `name` u32 (`0xAA640001`), `serial_number` u32, `flags` u64 (bits 32-47: platform flags) |
 * | 16 | ToC entries, 40 bytes each: `uuid` (16 raw bytes), `offset_address` u64 (from the FIP start), `size` u64,
 * `flags` u64 | | ... | an all-zero UUID entry terminates the ToC; payloads follow at their offsets |
 *
 * @code
 * // Read side: why - locate BL31/BL33 inside a FIP blob a boot ROM or BL2 loaded.
 * // fip_bytes: the whole package as span<const std::byte>.
 * auto fip = structo::boot::arm_tf_fip::fip_reader::try_create(fip_bytes);
 * if (!fip)
 *   return;                                                      // not a FIP, or truncated
 * auto bl33 = fip->find(structo::boot::arm_tf_fip::uuids::non_trusted_firmware_bl33);
 * if (bl33)
 *   boot(bl33->payload);                                         // bounds-checked span into fip_bytes
 * auto it = fip->entries();
 * for (auto e : it) {                                           // walk every entry
 *   if (!e)
 *     break;                                                     // malformed entry or missing terminator
 *   use(e->uuid, e->payload.size());
 * }
 *
 * // Write side: why - build a ToC for payloads you have placed yourself.
 * structo::boot::arm_tf_fip::toc_writer w(toc_buffer, 0x12345678, 0);  // buffer, serial, flags
 * (void)w.add(structo::boot::arm_tf_fip::uuids::trusted_boot_firmware_bl2, bl2_offset, bl2_size, 0);
 * (void)w.finish();                                              // writes the all-zero terminator
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
#include <structo/detail/boot_bytes.hpp>

namespace structo::boot::arm_tf_fip {

using namespace reloco;
namespace boot_bytes = structo::boot::detail;

inline constexpr uint32_t toc_header_name = 0xAA640001;
/** @brief Serial number `fiptool` writes. */
inline constexpr uint32_t toc_header_serial_number = 0x12345678;
inline constexpr std::size_t toc_header_size = 16;
inline constexpr std::size_t toc_entry_size = 40;

/** @brief A 128-bit UUID as stored in a ToC entry (the raw bytes, in textual order). */
struct uuid {
  uint64_t hi = 0; // bytes 0-7, big-endian
  uint64_t lo = 0; // bytes 8-15, big-endian

  [[nodiscard]] constexpr bool is_null() const noexcept { return hi == 0 && lo == 0; }
  friend constexpr bool operator==(const uuid &a, const uuid &b) noexcept { return a.hi == b.hi && a.lo == b.lo; }
  friend constexpr bool operator!=(const uuid &a, const uuid &b) noexcept { return !(a == b); }
};

namespace detail {
constexpr uint8_t hex_digit(char c) noexcept {
  return static_cast<uint8_t>(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
}
} // namespace detail

/** @brief Builds a `uuid` from 32 hex digits (no dashes) in textual byte order. */
template <std::size_t N> [[nodiscard]] constexpr uuid uuid_from_hex(const char (&hex)[N]) noexcept {
  static_assert(N == 33, "expected 32 hex digits");
  const string_view s(hex, 32);
  uuid u;
  for (std::size_t i = 0; i < 16; ++i)
    (i < 8 ? u.hi : u.lo) = ((i < 8 ? u.hi : u.lo) << 8) |
                            static_cast<uint64_t>((detail::hex_digit(s[2 * i]) << 4) | detail::hex_digit(s[2 * i + 1]));
  return u;
}

/** @brief Well-known TF-A image UUIDs (from `firmware_image_package.h`). */
namespace uuids {
inline constexpr uuid trusted_update_firmware_scp_bl2u = uuid_from_hex("659227032f74e6448dff579ac1ff0610");
inline constexpr uuid trusted_update_firmware_bl2u = uuid_from_hex("60b3eb37c1e5ea419df319eda11f6801");
inline constexpr uuid trusted_update_firmware_ns_bl2u = uuid_from_hex("4f511d112be54e49b4c583c2f715840a");
inline constexpr uuid trusted_fwu_cert = uuid_from_hex("71408ab218d6874c8b2ec6dccd50f096");
inline constexpr uuid cca_content_cert = uuid_from_hex("36d83d85761d4daf96f1cd99d6569b00");
inline constexpr uuid core_swd_key_cert = uuid_from_hex("52222d31820f494d8bbcea6825d3c35a");
inline constexpr uuid plat_key_cert = uuid_from_hex("d43cd9025b9f412e8ac692b6d18be60d");
inline constexpr uuid trusted_boot_firmware_bl2 = uuid_from_hex("5ff9ec0b4d223e4da544c39d81c73f0a");
inline constexpr uuid scp_firmware_scp_bl2 = uuid_from_hex("9766fd3d89bee849ae5d78a140608213");
inline constexpr uuid el3_runtime_firmware_bl31 = uuid_from_hex("47d4086d4cfe98469b952950cbbd5a00");
inline constexpr uuid secure_payload_bl32 = uuid_from_hex("05d0e18953dc13478d2b500a4b7a3e38");
inline constexpr uuid secure_payload_bl32_extra1 = uuid_from_hex("0b70c29b2a5a78409f650a5682738288");
inline constexpr uuid secure_payload_bl32_extra2 = uuid_from_hex("8ea87bb1cfa23f4d85fde7bba50220d9");
inline constexpr uuid non_trusted_firmware_bl33 = uuid_from_hex("d6d0eea7fcead54b97829934f234b6e4");
inline constexpr uuid realm_monitor_mgmt_firmware = uuid_from_hex("6c0762a612f24b5692cbba8f633606d9");
inline constexpr uuid rot_key_cert = uuid_from_hex("862d1d72f860e411920b8be762160f24");
inline constexpr uuid trusted_key_cert = uuid_from_hex("827ee890f860e411a1b4777a21b4f94c");
inline constexpr uuid non_trusted_world_key_cert = uuid_from_hex("1c67873d5f63e411978d27c0c7148abd");
inline constexpr uuid scp_fw_key_cert = uuid_from_hex("024221a1f860e4118d9bf33c0e15a014");
inline constexpr uuid soc_fw_key_cert = uuid_from_hex("8ab8beccf960e4119ad0eb4822d8dcf8");
inline constexpr uuid trusted_os_fw_key_cert = uuid_from_hex("9477d603fb60e41185ddb7105b8cee04");
inline constexpr uuid non_trusted_fw_key_cert = uuid_from_hex("8ad5832afb60e4118aafdf30bbc49859");
inline constexpr uuid trusted_boot_fw_cert = uuid_from_hex("d6e269ea5d63e4118d8c9fbabe9956a5");
inline constexpr uuid scp_fw_content_cert = uuid_from_hex("44be6f045e63e411b28b73d8eaae9656");
inline constexpr uuid soc_fw_content_cert = uuid_from_hex("e2b20c205e63e4119ce8abccf92bb666");
inline constexpr uuid trusted_os_fw_content_cert = uuid_from_hex("a49f44115e63e41187283f05722af33d");
inline constexpr uuid non_trusted_fw_content_cert = uuid_from_hex("8ec4c1f35d63e411a7a987ee40b23fa7");
inline constexpr uuid sip_secure_partition_content_cert = uuid_from_hex("776dfd4486974c3b91ebc13e025a2a6f");
inline constexpr uuid plat_secure_partition_content_cert = uuid_from_hex("ddcbbf4acad611ea87d00242ac130003");
inline constexpr uuid hw_config = uuid_from_hex("08b8f1d9c9cf9349a9626fbc6b7265cc");
inline constexpr uuid tb_fw_config = uuid_from_hex("6c0458ffaf6b7d4f82edaa27bc69bfd2");
inline constexpr uuid soc_fw_config = uuid_from_hex("9979814b0376fb468c8e8d267f7859e0");
inline constexpr uuid tos_fw_config = uuid_from_hex("26257c1adbc67f478d96c4c4b0248021");
inline constexpr uuid nt_fw_config = uuid_from_hex("28da981593e87e44ac661aaf801550f9");
inline constexpr uuid fw_config = uuid_from_hex("5807e16a845947be8ed5648e8dddab0e");
} // namespace uuids

/** @brief Decoded ToC header. */
struct toc_header {
  uint32_t serial_number = 0;
  uint64_t flags = 0;
  /** @brief Platform-specific ToC flags (bits 32-47 of `flags`). */
  [[nodiscard]] constexpr uint16_t platform_flags() const noexcept {
    return static_cast<uint16_t>((flags >> 32) & 0xFFFF);
  }
};

/** @brief One ToC entry together with its payload, a view into the FIP. */
struct fip_entry {
  uuid id;
  uint64_t offset = 0;
  uint64_t size = 0;
  uint64_t flags = 0;
  span<const std::byte> payload{};
};

[[nodiscard]] inline result<uuid> read_uuid(span<const std::byte> region, std::size_t offset) noexcept {
  auto hi = boot_bytes::read_be_at<uint64_t>(region, offset);
  auto lo = boot_bytes::read_be_at<uint64_t>(region, offset + 8);
  if (!hi || !lo)
    return unexpected(error::out_of_bounds);
  return uuid{*hi, *lo};
}

/** @brief Iterates the ToC entries; stops after the all-zero terminator. A truncated ToC (no terminator), or an
 * entry whose payload lies outside the FIP, yields one error item, then iteration ends. */
class RELOCO_POINTER fip_entry_iterator : public iterator_adaptor<fip_entry_iterator, result<fip_entry>> {
public:
  using item_type = result<fip_entry>;

  explicit fip_entry_iterator(span<const std::byte> fip) noexcept : fip_(fip) {}

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (done_)
      return nullopt;
    auto id = read_uuid(fip_, cursor_);
    if (!id)
      return fail(error::out_of_bounds); // No terminator before the end of the blob.
    if (id->is_null()) {
      done_ = true;
      return nullopt;
    }
    auto offset = boot_bytes::read_le_at<uint64_t>(fip_, cursor_ + 16);
    auto size = boot_bytes::read_le_at<uint64_t>(fip_, cursor_ + 24);
    auto flags = boot_bytes::read_le_at<uint64_t>(fip_, cursor_ + 32);
    if (!offset || !size || !flags)
      return fail(error::out_of_bounds);
    if (*offset > fip_.size() || *size > fip_.size() - static_cast<std::size_t>(*offset))
      return fail(error::out_of_bounds); // Also covers offset + size overflow.
    cursor_ += toc_entry_size;
    return optional<item_type>(
        item_type(fip_entry{*id, *offset, *size, *flags,
                            fip_.subspan(static_cast<std::size_t>(*offset), static_cast<std::size_t>(*size))}));
  }

private:
  [[nodiscard]] optional<item_type> fail(error e) noexcept {
    done_ = true;
    return optional<item_type>(item_type(unexpected(e)));
  }
  span<const std::byte> fip_;
  std::size_t cursor_ = toc_header_size;
  bool done_ = false;
};

/** @brief Validated view of a FIP blob. */
class RELOCO_POINTER fip_reader {
public:
  /** @brief Checks the ToC header signature (`0xAA640001`) and that the blob can hold a header. */
  [[nodiscard]] static result<fip_reader> try_create(span<const std::byte> fip) noexcept {
    auto name = boot_bytes::read_le_at<uint32_t>(fip, 0);
    auto serial = boot_bytes::read_le_at<uint32_t>(fip, 4);
    auto flags = boot_bytes::read_le_at<uint64_t>(fip, 8);
    if (!name || !serial || !flags)
      return unexpected(error::out_of_bounds);
    if (*name != toc_header_name)
      return unexpected(error::invalid_argument);
    return fip_reader(fip, toc_header{*serial, *flags});
  }

  [[nodiscard]] const toc_header &header() const noexcept { return header_; }
  /** @brief A fresh single-pass iterator over the ToC entries. */
  [[nodiscard]] fip_entry_iterator entries() const noexcept { return fip_entry_iterator(fip_); }

  /** @brief First entry with UUID @p id; `error::not_found` if absent, or the error of a malformed entry
   * encountered first. */
  [[nodiscard]] result<fip_entry> find(const uuid &id) const noexcept {
    auto it = entries();
    for (auto e : it) {
      if (!e)
        return e;
      if (e->id == id)
        return e;
    }
    return unexpected(error::not_found);
  }

private:
  fip_reader(span<const std::byte> fip, toc_header h) noexcept : fip_(fip), header_(h) {}
  span<const std::byte> fip_;
  toc_header header_;
};

/** @brief Writes a ToC (header, entries, terminator) into caller-owned memory. Payload placement is the caller's. */
class toc_writer {
public:
  toc_writer(span<std::byte> out, uint32_t serial_number = toc_header_serial_number, uint64_t flags = 0) noexcept
      : out_(out) {
    if (boot_bytes::write_le_at<uint32_t>(out_, 0, toc_header_name) &&
        boot_bytes::write_le_at<uint32_t>(out_, 4, serial_number) && boot_bytes::write_le_at<uint64_t>(out_, 8, flags))
      ok_ = true;
  }

  /** @brief Appends an entry. The terminator slot is reserved, so one more entry than fits fails. */
  [[nodiscard]] result<void> add(const uuid &id, uint64_t offset, uint64_t size, uint64_t flags) noexcept {
    if (!ok_ || id.is_null())
      return unexpected(ok_ ? error::invalid_argument : error::capacity_exceeded);
    if (cursor_ + 2 * toc_entry_size > out_.size())
      return unexpected(error::capacity_exceeded);
    if (!boot_bytes::write_be_at<uint64_t>(out_, cursor_, id.hi) ||
        !boot_bytes::write_be_at<uint64_t>(out_, cursor_ + 8, id.lo) ||
        !boot_bytes::write_le_at<uint64_t>(out_, cursor_ + 16, offset) ||
        !boot_bytes::write_le_at<uint64_t>(out_, cursor_ + 24, size) ||
        !boot_bytes::write_le_at<uint64_t>(out_, cursor_ + 32, flags))
      return unexpected(error::capacity_exceeded);
    cursor_ += toc_entry_size;
    return {};
  }

  /** @brief Writes the all-zero terminator entry. */
  [[nodiscard]] result<void> finish() noexcept {
    if (!ok_ || cursor_ + toc_entry_size > out_.size())
      return unexpected(error::capacity_exceeded);
    for (std::size_t i = 0; i < toc_entry_size; ++i)
      out_[cursor_ + i] = std::byte{0};
    cursor_ += toc_entry_size;
    return {};
  }

  /** @brief ToC bytes written so far. */
  [[nodiscard]] std::size_t size() const noexcept { return cursor_; }

private:
  span<std::byte> out_;
  std::size_t cursor_ = toc_header_size;
  bool ok_ = false;
};

} // namespace structo::boot::arm_tf_fip
