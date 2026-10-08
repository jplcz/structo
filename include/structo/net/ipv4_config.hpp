// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file ipv4_config.hpp
 * @brief `structo::net::ipv4_config`: the addressing parameters of an IPv4
 * interface, whether set statically or learned from DHCP. C++20 only.
 *
 * @code
 * // Static configuration: apply it to the node once at boot.
 * ip.configure(structo::net::ipv4_config::make_static(
 *     {192, 168, 7, 2},   // our address
 *     {255, 255, 255, 0}, // netmask
 *     {192, 168, 7, 1},   // default gateway (optional)
 *     {192, 168, 7, 1})); // DNS server (optional)
 * @endcode
 */

#include "ipv4.hpp"

#if RELOCO_HAS_COROUTINES

namespace structo::net {

struct ipv4_config {
  ipv4_address address{};  ///< Unspecified (0.0.0.0) means "not configured".
  ipv4_address netmask{};
  ipv4_address gateway{};  ///< Unspecified if there is no default route.
  ipv4_address dns{};      ///< Unspecified if unknown.
  std::uint32_t lease_seconds = 0; ///< 0 for a static (never expiring) configuration.

  [[nodiscard]] constexpr bool configured() const noexcept { return !address.is_unspecified(); }

  [[nodiscard]] static constexpr ipv4_config make_static(ipv4_address address, ipv4_address netmask = {},
                                                         ipv4_address gateway = {}, ipv4_address dns = {}) noexcept {
    ipv4_config c;
    c.address = address;
    c.netmask = netmask;
    c.gateway = gateway;
    c.dns = dns;
    return c;
  }
};

} // namespace structo::net

#endif // RELOCO_HAS_COROUTINES
