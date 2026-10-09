// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file usb_defs.hpp
 * @brief USB 2.0 constants and descriptor parsing for the host stack: standard requests, descriptor
 * types, class codes, `device_descriptor`, `endpoint_descriptor` and `config_view` (walks a
 * configuration descriptor to find interfaces, their endpoints and class-specific descriptors).
 * No allocation. C++20 only (empty otherwise), like the rest of the USB stack.
 */

#include "../hw/usb_host_controller_ref.hpp"

#if RELOCO_HAS_COROUTINES

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::usb {

namespace request {
inline constexpr std::uint8_t get_status = 0;
inline constexpr std::uint8_t clear_feature = 1;
inline constexpr std::uint8_t set_feature = 3;
inline constexpr std::uint8_t set_address = 5;
inline constexpr std::uint8_t get_descriptor = 6;
inline constexpr std::uint8_t get_configuration = 8;
inline constexpr std::uint8_t set_configuration = 9;
inline constexpr std::uint8_t get_interface = 10;
inline constexpr std::uint8_t set_interface = 11;
} // namespace request

/** @brief `bmRequestType` values for the common (recipient, type, direction) combinations. */
namespace request_type {
inline constexpr std::uint8_t standard_out_device = 0x00;
inline constexpr std::uint8_t standard_out_interface = 0x01;
inline constexpr std::uint8_t standard_out_endpoint = 0x02;
inline constexpr std::uint8_t standard_in_device = 0x80;
inline constexpr std::uint8_t class_out_interface = 0x21;
inline constexpr std::uint8_t class_in_interface = 0xA1;
} // namespace request_type

namespace descriptor_type {
inline constexpr std::uint8_t device = 1;
inline constexpr std::uint8_t configuration = 2;
inline constexpr std::uint8_t string = 3;
inline constexpr std::uint8_t interface = 4;
inline constexpr std::uint8_t endpoint = 5;
inline constexpr std::uint8_t cs_interface =
    0x24; ///< Class-specific interface descriptor (CDC functional descriptors).
} // namespace descriptor_type

inline constexpr std::uint16_t feature_endpoint_halt = 0;

namespace usb_class {
inline constexpr std::uint8_t cdc = 0x02;
inline constexpr std::uint8_t mass_storage = 0x08;
inline constexpr std::uint8_t cdc_data = 0x0A;
} // namespace usb_class

/** @brief "Don't care" for `config_view::find_interface` filters (0xFF is a valid vendor class code). */
inline constexpr std::uint16_t any = 0x100;

/** @brief Parsed standard device descriptor. */
struct device_descriptor {
  std::uint16_t bcd_usb = 0;
  std::uint8_t device_class = 0;
  std::uint8_t device_subclass = 0;
  std::uint8_t device_protocol = 0;
  std::uint8_t max_packet0 = 8;
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  std::uint16_t bcd_device = 0;
  std::uint8_t manufacturer_string = 0;
  std::uint8_t product_string = 0;
  std::uint8_t serial_string = 0;
  std::uint8_t num_configurations = 0;
};

[[nodiscard]] constexpr std::uint16_t le16(span<const std::uint8_t> p) noexcept {
  return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

/** @brief `error::invalid_argument` if shorter than 18 bytes or not a device descriptor. */
[[nodiscard]] inline reloco::result<device_descriptor>
parse_device_descriptor(reloco::span<const std::uint8_t> b) noexcept {
  if (b.size() < 18 || b[0] < 18 || b[1] != descriptor_type::device)
    return reloco::unexpected(reloco::error::invalid_argument);
  device_descriptor d;
  d.bcd_usb = le16(b.subspan(2));
  d.device_class = b[4];
  d.device_subclass = b[5];
  d.device_protocol = b[6];
  d.max_packet0 = b[7];
  d.vendor_id = le16(b.subspan(8));
  d.product_id = le16(b.subspan(10));
  d.bcd_device = le16(b.subspan(12));
  d.manufacturer_string = b[14];
  d.product_string = b[15];
  d.serial_string = b[16];
  d.num_configurations = b[17];
  return d;
}

struct endpoint_descriptor {
  std::uint8_t address = 0;    ///< Bit 7 = direction (1 = IN), bits 3..0 = endpoint number.
  std::uint8_t attributes = 0; ///< Bits 1..0 = transfer type.
  std::uint16_t max_packet = 0;
  std::uint8_t interval = 0;

  [[nodiscard]] constexpr hw::usb_direction direction() const noexcept {
    return (address & 0x80) ? hw::usb_direction::in : hw::usb_direction::out;
  }
  [[nodiscard]] constexpr hw::usb_transfer_type type() const noexcept {
    return static_cast<hw::usb_transfer_type>(attributes & 3);
  }
  [[nodiscard]] constexpr std::uint8_t number() const noexcept { return static_cast<std::uint8_t>(address & 0x0F); }
};

/** @brief One interface (alternate setting) with its endpoints, as found in a configuration descriptor. */
struct interface_info {
  static constexpr std::size_t max_endpoints = 6;

  std::uint8_t number = 0;
  std::uint8_t alt = 0;
  std::uint8_t interface_class = 0;
  std::uint8_t subclass = 0;
  std::uint8_t protocol = 0;
  reloco::array<endpoint_descriptor, max_endpoints> endpoints{};
  std::size_t endpoint_count = 0;
  std::size_t begin = 0; ///< Offset of the interface descriptor in the configuration descriptor.
  std::size_t end = 0;   ///< Offset just past its last descriptor (before the next interface).

  /** @brief First endpoint of this type and direction, or `nullptr`. */
  [[nodiscard]] const endpoint_descriptor *find_endpoint(hw::usb_transfer_type t, hw::usb_direction d) const noexcept {
    for (std::size_t i = 0; i < endpoint_count; ++i)
      if (endpoints[i].type() == t && endpoints[i].direction() == d)
        return &endpoints[i];
    return nullptr;
  }
};

/** @brief Read-only walker over a complete configuration descriptor (config + interface + endpoint + class-specific).
 */
class config_view {
public:
  config_view() noexcept = default;
  explicit config_view(reloco::span<const std::uint8_t> bytes) noexcept : b_(bytes) {}

  [[nodiscard]] reloco::span<const std::uint8_t> bytes() const noexcept { return b_; }
  [[nodiscard]] bool valid() const noexcept { return b_.size() >= 9 && b_[1] == descriptor_type::configuration; }
  /** @brief `bConfigurationValue` to pass to SET_CONFIGURATION. */
  [[nodiscard]] std::uint8_t configuration_value() const noexcept { return valid() ? b_[5] : std::uint8_t{0}; }
  [[nodiscard]] std::uint16_t total_length() const noexcept { return valid() ? le16(b_.subspan(2)) : std::uint16_t{0}; }

  /**
   * @brief Finds the `nth` interface descriptor matching the filters (`usb::any` = don't care) and collects its
   * endpoints. `error::not_found` if there is none, `error::invalid_argument` for a malformed descriptor chain.
   */
  [[nodiscard]] reloco::result<interface_info> find_interface(std::uint16_t cls, std::uint16_t subclass = any,
                                                              std::uint16_t protocol = any, std::uint16_t alt = any,
                                                              unsigned nth = 0) const noexcept {
    if (!valid())
      return reloco::unexpected(reloco::error::invalid_argument);
    std::size_t pos = b_[0];
    while (pos + 2 <= b_.size()) {
      const std::size_t len = b_[pos];
      if (len < 2 || pos + len > b_.size())
        return reloco::unexpected(reloco::error::invalid_argument);
      if (b_[pos + 1] == descriptor_type::interface && len >= 9 && matches(b_[pos + 5], cls) &&
          matches(b_[pos + 6], subclass) && matches(b_[pos + 7], protocol) && matches(b_[pos + 3], alt)) {
        if (nth-- == 0)
          return collect(pos);
      }
      pos += len;
    }
    return reloco::unexpected(reloco::error::not_found);
  }

  /** @brief Class-specific interface descriptor of `subtype` inside `itf`, e.g. a CDC functional descriptor. */
  [[nodiscard]] reloco::result<reloco::span<const std::uint8_t>>
  find_cs_descriptor(const interface_info &itf, std::uint8_t subtype) const noexcept {
    std::size_t pos = itf.begin;
    while (pos + 3 <= itf.end && pos + 3 <= b_.size()) {
      const std::size_t len = b_[pos];
      if (len < 2 || pos + len > b_.size())
        break;
      if (b_[pos + 1] == descriptor_type::cs_interface && b_[pos + 2] == subtype)
        return b_.subspan(pos, len);
      pos += len;
    }
    return reloco::unexpected(reloco::error::not_found);
  }

private:
  static constexpr bool matches(std::uint8_t v, std::uint16_t want) noexcept { return want == any || v == want; }

  reloco::result<interface_info> collect(std::size_t pos) const noexcept {
    interface_info itf;
    itf.number = b_[pos + 2];
    itf.alt = b_[pos + 3];
    itf.interface_class = b_[pos + 5];
    itf.subclass = b_[pos + 6];
    itf.protocol = b_[pos + 7];
    itf.begin = pos;
    pos += b_[pos];
    while (pos + 2 <= b_.size()) {
      const std::size_t len = b_[pos];
      if (len < 2 || pos + len > b_.size())
        return reloco::unexpected(reloco::error::invalid_argument);
      if (b_[pos + 1] == descriptor_type::interface)
        break;
      if (b_[pos + 1] == descriptor_type::endpoint && len >= 7 && itf.endpoint_count < interface_info::max_endpoints) {
        endpoint_descriptor &e = itf.endpoints[itf.endpoint_count++];
        e.address = b_[pos + 2];
        e.attributes = b_[pos + 3];
        e.max_packet = static_cast<std::uint16_t>(le16(b_.subspan(pos + 4)) & 0x07FF);
        e.interval = b_[pos + 6];
      }
      pos += len;
    }
    itf.end = pos;
    return itf;
  }

  reloco::span<const std::uint8_t> b_;
};

} // namespace structo::usb

#endif // RELOCO_HAS_COROUTINES
