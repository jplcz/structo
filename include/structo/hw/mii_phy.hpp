// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mii_phy.hpp
 * @brief MII/MDIO PHY support layer for Ethernet MAC drivers: `mdio_bus_ref` (the only thing a MAC driver
 * provides: read/write one 16-bit PHY register) and `mii_phy`, a generic IEEE 802.3 Clause 22 PHY
 * driver (probe, reset, autonegotiation, forced speed, debounced link/speed/duplex status, MMD
 * indirect access) with optional per-PHY quirk hooks. No sleeping or coroutines: bounded polling,
 * C++17 is enough.
 *
 * @code
 * // 1. The MAC driver exposes its MDIO controller (e.g. the MIIM data/address registers).
 * struct my_mdio {
 *   // ... register access ...
 * };
 * template <> struct structo::hw::mdio_traits<my_mdio> {
 *   // phy = PHY address 0..31, reg = register 0..31.
 *   static reloco::result<std::uint16_t> read(my_mdio &m, unsigned phy, unsigned reg) noexcept;
 *   static reloco::result<void> write(my_mdio &m, unsigned phy, unsigned reg, std::uint16_t value) noexcept;
 * };
 *
 * // 2. Bring the PHY up once at init.
 * my_mdio mdio;
 * structo::hw::mii_phy phy{structo::hw::mdio_bus_ref{mdio}}; // address unknown: probe() scans 0..31
 * (void)phy.probe();                                         // finds the PHY, reads its ID and abilities
 * (void)phy.reset();                                         // soft reset, bounded wait for completion
 * (void)phy.start_autoneg();                                 // advertise everything the PHY can do
 *
 * // 3. In the driver's link_up() (called by ethernet_nic): react to changes.
 * auto l = phy.poll();                                       // debounced: reads the latched BMSR twice
 * if (l && l->changed)
 *   configure_mac_speed_duplex(l->speed, l->full_duplex);    // reprogram the MAC when the link comes up
 * return l && l->up;
 * @endcode
 */

#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace structo::hw {

/** @brief Opt-in customization point for an MDIO controller; specialize with static `read`/`write`. */
template <typename Backend> struct mdio_traits;

namespace detail {

template <typename Backend, typename = void> struct has_mdio_traits : std::false_type {};
template <typename Backend>
struct has_mdio_traits<
    Backend, std::void_t<decltype(mdio_traits<Backend>::read(std::declval<Backend &>(), 0u, 0u)),
                         decltype(mdio_traits<Backend>::write(std::declval<Backend &>(), 0u, 0u, std::uint16_t{}))>>
    : std::true_type {};

} // namespace detail

/** @brief Type-erased, non-owning handle over an MDIO controller. Unbound refs fail with `unsupported_operation`. */
class RELOCO_POINTER mdio_bus_ref {
public:
  constexpr mdio_bus_ref() noexcept = default;

  template <typename Backend, std::enable_if_t<detail::has_mdio_traits<Backend>::value, int> = 0>
  constexpr explicit mdio_bus_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), read_(&do_read<Backend>), write_(&do_write<Backend>) {}

  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  mdio_bus_ref(Backend &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return read_ != nullptr; }

  [[nodiscard]] reloco::result<std::uint16_t> read(unsigned phy, unsigned reg) const noexcept {
    if (!read_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return read_(ctx_, phy, reg);
  }

  [[nodiscard]] reloco::result<void> write(unsigned phy, unsigned reg, std::uint16_t value) const noexcept {
    if (!write_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return write_(ctx_, phy, reg, value);
  }

private:
  template <typename B> static reloco::result<std::uint16_t> do_read(void *c, unsigned phy, unsigned reg) noexcept {
    return mdio_traits<B>::read(*static_cast<B *>(c), phy, reg);
  }
  template <typename B>
  static reloco::result<void> do_write(void *c, unsigned phy, unsigned reg, std::uint16_t v) noexcept {
    return mdio_traits<B>::write(*static_cast<B *>(c), phy, reg, v);
  }

  void *ctx_ = nullptr;
  reloco::result<std::uint16_t> (*read_)(void *, unsigned, unsigned) noexcept = nullptr;
  reloco::result<void> (*write_)(void *, unsigned, unsigned, std::uint16_t) noexcept = nullptr;
};

/** @brief IEEE 802.3 Clause 22 register numbers and bits. */
namespace mii {

inline constexpr unsigned reg_bmcr = 0;     ///< Basic mode control.
inline constexpr unsigned reg_bmsr = 1;     ///< Basic mode status.
inline constexpr unsigned reg_id1 = 2;      ///< PHY identifier (OUI high bits).
inline constexpr unsigned reg_id2 = 3;      ///< PHY identifier (OUI low bits, model, revision).
inline constexpr unsigned reg_anar = 4;     ///< Autonegotiation advertisement.
inline constexpr unsigned reg_anlpar = 5;   ///< Link partner ability.
inline constexpr unsigned reg_gbcr = 9;     ///< 1000BASE-T control (advertisement).
inline constexpr unsigned reg_gbsr = 10;    ///< 1000BASE-T status (link partner).
inline constexpr unsigned reg_mmdctl = 13;  ///< MMD access control (Clause 45 indirect access).
inline constexpr unsigned reg_mmdaddr = 14; ///< MMD access address/data.
inline constexpr unsigned reg_estatus = 15; ///< Extended status.

inline constexpr std::uint16_t bmcr_speed1000 = 0x0040;
inline constexpr std::uint16_t bmcr_full_duplex = 0x0100;
inline constexpr std::uint16_t bmcr_restart_an = 0x0200;
inline constexpr std::uint16_t bmcr_isolate = 0x0400;
inline constexpr std::uint16_t bmcr_power_down = 0x0800;
inline constexpr std::uint16_t bmcr_an_enable = 0x1000;
inline constexpr std::uint16_t bmcr_speed100 = 0x2000;
inline constexpr std::uint16_t bmcr_loopback = 0x4000;
inline constexpr std::uint16_t bmcr_reset = 0x8000;

inline constexpr std::uint16_t bmsr_extended_caps = 0x0001;
inline constexpr std::uint16_t bmsr_link = 0x0004;
inline constexpr std::uint16_t bmsr_an_capable = 0x0008;
inline constexpr std::uint16_t bmsr_an_complete = 0x0020;
inline constexpr std::uint16_t bmsr_extended_status = 0x0100;
inline constexpr std::uint16_t bmsr_10hd = 0x0800;
inline constexpr std::uint16_t bmsr_10fd = 0x1000;
inline constexpr std::uint16_t bmsr_100hd = 0x2000;
inline constexpr std::uint16_t bmsr_100fd = 0x4000;

/** @brief ANAR / ANLPAR ability bits (same layout in both registers). */
inline constexpr std::uint16_t an_selector_8023 = 0x0001;
inline constexpr std::uint16_t an_10hd = 0x0020;
inline constexpr std::uint16_t an_10fd = 0x0040;
inline constexpr std::uint16_t an_100hd = 0x0080;
inline constexpr std::uint16_t an_100fd = 0x0100;
inline constexpr std::uint16_t an_pause = 0x0400;
inline constexpr std::uint16_t an_all_speeds = an_10hd | an_10fd | an_100hd | an_100fd;

inline constexpr std::uint16_t gbcr_1000hd = 0x0100; ///< Advertise 1000BASE-T half duplex.
inline constexpr std::uint16_t gbcr_1000fd = 0x0200; ///< Advertise 1000BASE-T full duplex.
inline constexpr std::uint16_t gbsr_lp_1000hd = 0x0400;
inline constexpr std::uint16_t gbsr_lp_1000fd = 0x0800;

inline constexpr std::uint16_t estatus_1000hd = 0x1000;
inline constexpr std::uint16_t estatus_1000fd = 0x2000;

} // namespace mii

enum class mii_speed : std::uint16_t { none = 0, mbps10 = 10, mbps100 = 100, mbps1000 = 1000 };

/** @brief Result of `mii_phy::poll`. */
struct mii_link {
  bool up = false; ///< Link established (and, with autonegotiation, completed).
  mii_speed speed = mii_speed::none;
  bool full_duplex = false;
  bool changed = false; ///< Differs from the previous `poll()` result (reprogram the MAC).
};

class mii_phy;

/** @brief Optional per-PHY-model hooks, installed with `mii_phy::set_quirks()` (match on `mii_phy::id()`). */
struct mii_quirks {
  /** @brief Called at the end of `reset()` (errata writes, clock/RGMII delay setup ...). */
  reloco::result<void> (*post_reset)(void *ctx, const mdio_bus_ref &bus, unsigned phy) noexcept = nullptr;
  /** @brief Called by `poll()` with the generic result; may replace it using vendor status registers. */
  reloco::result<mii_link> (*read_status)(void *ctx, const mdio_bus_ref &bus, unsigned phy,
                                          const mii_link &generic) noexcept = nullptr;
  void *ctx = nullptr;
};

/** @brief Generic Clause 22 PHY driver. */
class mii_phy {
public:
  static constexpr unsigned any_address = 0xFF; ///< `probe()` scans all 32 addresses.

  /** @param bus MDIO controller (must outlive this object). @param address PHY address, or `any_address`. */
  explicit mii_phy(mdio_bus_ref bus, unsigned address = any_address) noexcept : bus_(bus), addr_(address) {}

  /**
   * @brief Finds the PHY (scans 0..31 unless an address was given), reads its identifier and abilities.
   * `error::not_found` if no PHY answers.
   */
  [[nodiscard]] reloco::result<void> probe() noexcept {
    unsigned first = addr_ == any_address ? 0 : addr_;
    unsigned last = addr_ == any_address ? 31 : addr_;
    if (first > 31)
      return reloco::unexpected(reloco::error::invalid_argument);
    for (unsigned a = first; a <= last; ++a) {
      auto id1 = bus_.read(a, mii::reg_id1);
      if (!id1)
        return reloco::unexpected(id1.error());
      auto id2 = bus_.read(a, mii::reg_id2);
      if (!id2)
        return reloco::unexpected(id2.error());
      // An absent PHY reads as all ones (pulled-up MDIO) or all zeros.
      if ((*id1 == 0xFFFF && *id2 == 0xFFFF) || (*id1 == 0 && *id2 == 0))
        continue;
      addr_ = a;
      id_ = (static_cast<std::uint32_t>(*id1) << 16) | *id2;
      return read_abilities();
    }
    return reloco::unexpected(reloco::error::not_found);
  }

  [[nodiscard]] unsigned address() const noexcept { return addr_; }
  /** @brief PHYID1:PHYID2 as read; `id() & ~0xFu` identifies the model, the low nibble is the revision. */
  [[nodiscard]] std::uint32_t id() const noexcept { return id_; }
  [[nodiscard]] bool gigabit_capable() const noexcept { return gbit_; }

  void set_quirks(const mii_quirks &q) noexcept { quirks_ = q; }

  /**
   * @brief Soft reset; polls BMCR up to `max_polls` times for the self-clearing reset bit
   * (`error::timed_out` otherwise), then runs the `post_reset` quirk.
   */
  [[nodiscard]] reloco::result<void> reset(unsigned max_polls = 1000) noexcept {
    if (auto r = write(mii::reg_bmcr, mii::bmcr_reset); !r)
      return r;
    for (unsigned i = 0; i < max_polls; ++i) {
      auto v = read(mii::reg_bmcr);
      if (!v)
        return reloco::unexpected(v.error());
      if (!(*v & mii::bmcr_reset)) {
        last_ = {};
        if (quirks_.post_reset)
          return quirks_.post_reset(quirks_.ctx, bus_, addr_);
        return {};
      }
    }
    return reloco::unexpected(reloco::error::timed_out);
  }

  /**
   * @brief Enables autonegotiation advertising every speed/duplex the PHY supports (plus pause), or only
   * those in `speeds` (`mii::an_*` bits for 10/100, `mii::gbcr_*` bits in `gigabit`), and restarts it.
   */
  [[nodiscard]] reloco::result<void> start_autoneg(std::uint16_t speeds = 0xFFFF,
                                                   std::uint16_t gigabit = 0xFFFF) noexcept {
    if (!an_capable_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    const std::uint16_t adv = static_cast<std::uint16_t>(mii::an_selector_8023 | mii::an_pause | (speeds & an_caps_));
    if (auto r = write(mii::reg_anar, adv); !r)
      return r;
    if (gbit_) {
      if (auto r = write(mii::reg_gbcr, static_cast<std::uint16_t>(gigabit & gb_caps_)); !r)
        return r;
    }
    last_ = {};
    return write(mii::reg_bmcr, static_cast<std::uint16_t>(mii::bmcr_an_enable | mii::bmcr_restart_an));
  }

  /** @brief Disables autonegotiation and forces `speed`/duplex. `error::invalid_argument` for `mii_speed::none`. */
  [[nodiscard]] reloco::result<void> force(mii_speed speed, bool full_duplex) noexcept {
    std::uint16_t v = full_duplex ? mii::bmcr_full_duplex : std::uint16_t{0};
    switch (speed) {
    case mii_speed::mbps10:
      break;
    case mii_speed::mbps100:
      v = static_cast<std::uint16_t>(v | mii::bmcr_speed100);
      break;
    case mii_speed::mbps1000:
      v = static_cast<std::uint16_t>(v | mii::bmcr_speed1000);
      break;
    default:
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    last_ = {};
    return write(mii::reg_bmcr, v);
  }

  /**
   * @brief Reads the link state. BMSR link is latched low, so it is read twice and the second value
   * counts. With autonegotiation the link only counts once it completed; the negotiated speed/duplex
   * come from the common abilities of ANAR/ANLPAR (and GBCR/GBSR); a forced link reports BMCR.
   * `changed` is set when the result differs from the previous call.
   */
  [[nodiscard]] reloco::result<mii_link> poll() noexcept {
    auto bmsr = read(mii::reg_bmsr);
    if (!bmsr)
      return reloco::unexpected(bmsr.error());
    bmsr = read(mii::reg_bmsr);
    if (!bmsr)
      return reloco::unexpected(bmsr.error());
    auto bmcr = read(mii::reg_bmcr);
    if (!bmcr)
      return reloco::unexpected(bmcr.error());

    mii_link l;
    if (*bmsr & mii::bmsr_link) {
      if (*bmcr & mii::bmcr_an_enable) {
        if (*bmsr & mii::bmsr_an_complete) {
          if (auto r = resolve(l); !r)
            return reloco::unexpected(r.error());
        }
      } else {
        l.up = true;
        l.full_duplex = (*bmcr & mii::bmcr_full_duplex) != 0;
        l.speed = (*bmcr & mii::bmcr_speed1000)  ? mii_speed::mbps1000
                  : (*bmcr & mii::bmcr_speed100) ? mii_speed::mbps100
                                                 : mii_speed::mbps10;
      }
    }
    if (quirks_.read_status) {
      auto q = quirks_.read_status(quirks_.ctx, bus_, addr_, l);
      if (!q)
        return q;
      l = *q;
    }
    l.changed = l.up != last_.up || l.speed != last_.speed || l.full_duplex != last_.full_duplex;
    last_ = l;
    return l;
  }

  /** @brief Reads an MMD (Clause 45) register through the Clause 22 indirect registers 13/14. */
  [[nodiscard]] reloco::result<std::uint16_t> read_mmd(unsigned devad, std::uint16_t reg) noexcept {
    if (auto r = mmd_select(devad, reg); !r)
      return reloco::unexpected(r.error());
    return read(mii::reg_mmdaddr);
  }

  [[nodiscard]] reloco::result<void> write_mmd(unsigned devad, std::uint16_t reg, std::uint16_t value) noexcept {
    if (auto r = mmd_select(devad, reg); !r)
      return r;
    return write(mii::reg_mmdaddr, value);
  }

  /** @brief Raw Clause 22 register access at this PHY's address. */
  [[nodiscard]] reloco::result<std::uint16_t> read(unsigned reg) const noexcept { return bus_.read(addr_, reg); }
  [[nodiscard]] reloco::result<void> write(unsigned reg, std::uint16_t value) const noexcept {
    return bus_.write(addr_, reg, value);
  }

private:
  reloco::result<void> read_abilities() noexcept {
    auto bmsr = read(mii::reg_bmsr);
    if (!bmsr)
      return reloco::unexpected(bmsr.error());
    an_capable_ = (*bmsr & mii::bmsr_an_capable) != 0;
    an_caps_ = 0;
    if (*bmsr & mii::bmsr_10hd)
      an_caps_ = static_cast<std::uint16_t>(an_caps_ | mii::an_10hd);
    if (*bmsr & mii::bmsr_10fd)
      an_caps_ = static_cast<std::uint16_t>(an_caps_ | mii::an_10fd);
    if (*bmsr & mii::bmsr_100hd)
      an_caps_ = static_cast<std::uint16_t>(an_caps_ | mii::an_100hd);
    if (*bmsr & mii::bmsr_100fd)
      an_caps_ = static_cast<std::uint16_t>(an_caps_ | mii::an_100fd);
    gbit_ = false;
    gb_caps_ = 0;
    if (*bmsr & mii::bmsr_extended_status) {
      auto es = read(mii::reg_estatus);
      if (!es)
        return reloco::unexpected(es.error());
      if (*es & mii::estatus_1000hd)
        gb_caps_ = static_cast<std::uint16_t>(gb_caps_ | mii::gbcr_1000hd);
      if (*es & mii::estatus_1000fd)
        gb_caps_ = static_cast<std::uint16_t>(gb_caps_ | mii::gbcr_1000fd);
      gbit_ = gb_caps_ != 0;
    }
    return {};
  }

  // Picks the best mode both sides advertise: 1000FD > 1000HD > 100FD > 100HD > 10FD > 10HD.
  reloco::result<void> resolve(mii_link &l) noexcept {
    auto anar = read(mii::reg_anar);
    if (!anar)
      return reloco::unexpected(anar.error());
    auto lpa = read(mii::reg_anlpar);
    if (!lpa)
      return reloco::unexpected(lpa.error());
    std::uint16_t gb_common = 0;
    if (gbit_) {
      auto gbcr = read(mii::reg_gbcr);
      if (!gbcr)
        return reloco::unexpected(gbcr.error());
      auto gbsr = read(mii::reg_gbsr);
      if (!gbsr)
        return reloco::unexpected(gbsr.error());
      gb_common = static_cast<std::uint16_t>(*gbcr & (*gbsr >> 2) & (mii::gbcr_1000hd | mii::gbcr_1000fd));
    }
    const std::uint16_t common = static_cast<std::uint16_t>(*anar & *lpa);
    l.up = true;
    if (gb_common & mii::gbcr_1000fd) {
      l.speed = mii_speed::mbps1000;
      l.full_duplex = true;
    } else if (gb_common & mii::gbcr_1000hd) {
      l.speed = mii_speed::mbps1000;
    } else if (common & mii::an_100fd) {
      l.speed = mii_speed::mbps100;
      l.full_duplex = true;
    } else if (common & mii::an_100hd) {
      l.speed = mii_speed::mbps100;
    } else if (common & mii::an_10fd) {
      l.speed = mii_speed::mbps10;
      l.full_duplex = true;
    } else if (common & mii::an_10hd) {
      l.speed = mii_speed::mbps10;
    } else {
      l.up = false; // no common mode
    }
    return {};
  }

  reloco::result<void> mmd_select(unsigned devad, std::uint16_t reg) noexcept {
    if (devad > 31)
      return reloco::unexpected(reloco::error::invalid_argument);
    const std::uint16_t dev = static_cast<std::uint16_t>(devad);
    if (auto r = write(mii::reg_mmdctl, dev); !r)
      return r;
    if (auto r = write(mii::reg_mmdaddr, reg); !r)
      return r;
    return write(mii::reg_mmdctl, static_cast<std::uint16_t>(0x4000 | dev)); // data, no post-increment
  }

  mdio_bus_ref bus_;
  unsigned addr_;
  std::uint32_t id_ = 0;
  bool an_capable_ = false;
  bool gbit_ = false;
  std::uint16_t an_caps_ = 0;
  std::uint16_t gb_caps_ = 0;
  mii_link last_{};
  mii_quirks quirks_{};
};

} // namespace structo::hw
