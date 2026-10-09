<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::mii_phy`

`include/structo/hw/mii_phy.hpp`

> **C++ standard:** C++17 (no coroutines, no sleeping: bounded polling only).

MII/MDIO PHY support for [`ethernet_nic`](ethernet_nic.md) drivers. Most real
MACs talk to an external PHY over MDIO; this header holds the generic IEEE 802.3
Clause 22 logic so each driver does not reimplement it. A MAC driver provides
only register access (`mdio_bus_ref`); `mii_phy` does the rest.

- `probe()` -- scans addresses 0..31 (or checks a given one), reads the PHY ID
  and its 10/100/1000 abilities; `error::not_found` if nothing answers.
- `reset()` -- soft reset with a bounded wait (`error::timed_out`), then the
  `post_reset` quirk.
- `start_autoneg()` / `force()` -- autonegotiate (advertising all abilities or a
  subset) or force a speed/duplex.
- `poll()` -- debounced `mii_link{up, speed, full_duplex, changed}`. BMSR's link bit
  is latched low, so it is read twice; with autonegotiation the link counts once
  negotiation completed, and the best common mode wins
  (1000FD > 1000HD > 100FD > 100HD > 10FD > 10HD).
- `read_mmd()` / `write_mmd()` -- Clause 45 registers via the Clause 22 indirect
  registers 13/14 (EEE, 2.5G+ PHYs).
- `mii_quirks` -- optional `post_reset` and `read_status` hooks, chosen by the
  driver by matching `phy.id()`, for per-model errata or vendor status registers.

```cpp
// The MAC's MDIO controller (e.g. MIIM data/address registers behind a busy flag).
struct my_mdio {
  // ... register access ...
};

// Tell structo how to read/write one PHY register. phy = PHY address 0..31, reg = register 0..31.
template <> struct structo::hw::mdio_traits<my_mdio> {
  static reloco::result<std::uint16_t> read(my_mdio &m, unsigned phy, unsigned reg) noexcept;
  static reloco::result<void> write(my_mdio &m, unsigned phy, unsigned reg, std::uint16_t value) noexcept;
};

my_mdio mdio;
structo::hw::mii_phy phy{structo::hw::mdio_bus_ref{mdio}}; // address unknown: probe() scans for it

// Init, once: find the PHY, reset it, start autonegotiation advertising everything it can do.
(void)phy.probe();
(void)phy.reset();
(void)phy.start_autoneg();

// In the driver's link_up() (called by ethernet_nic through polled_net_device): poll the PHY and,
// when `changed` is set, reprogram the MAC for the negotiated speed and duplex.
auto l = phy.poll();
if (l && l->changed)
  configure_mac(l->speed, l->full_duplex); // e.g. MAC speed select + full-duplex bits
return l && l->up;
```

Not covered: Clause 45 native frames, per-vendor PHY driver tables (use quirks),
interrupt-driven link change (call `poll()` from the PHY interrupt handler).
