<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::ethernet_nic`

`include/structo/hw/ethernet_nic.hpp`

> **C++ standard:** C++20 only (empty under C++17).

The adaptation layer for writing real Ethernet hardware drivers. A driver only
implements the descriptor-ring primitives of its MAC; `ethernet_nic<Driver>`
turns them into a `polled_net_device` backend carrying raw Ethernet frames, so
the rest of the stack (`ethernet_device`, `netstack`) needs nothing hardware
specific. The adapter does the chores every driver would otherwise repeat:

- rejects frames shorter than an Ethernet header or longer than `max_frame_size`;
- zero-pads short TX frames to the 60-byte minimum;
- strips the trailing FCS on RX if the hardware leaves it (`rx_includes_fcs`);
- drops runt/oversized RX frames (or ones that do not fit the caller's buffer)
  and always returns the descriptor, so a bad frame never wedges the ring;
- keeps counters (`stats()`): frames, bytes, TX ring full, runts, oversized, errors.

## Driver contract

All members are `noexcept`; "no progress right now" is `error::try_again`.

```cpp
// A driver for a hypothetical DMA MAC with TX/RX descriptor rings. On FreeBSD the equivalent
// pieces live in the NIC's if_transmit/rxeof paths (see ifnet(9), bus_dma(9)).
struct my_mac_driver {
  // Station address (e.g. read from the MAC's address registers or OTP).
  structo::hw::net_mac_address read_mac() noexcept;

  // PHY link state: cable plugged in and autonegotiation finished.
  bool link_up() noexcept;

  // TX: copy `frame` (already >= 60 bytes, no FCS) into the next free TX descriptor's buffer and
  // give the descriptor to the hardware. Return error::try_again if every descriptor is busy;
  // the caller retries on its next poll().
  reloco::result<void> tx_submit(reloco::span<const std::uint8_t> frame) noexcept;

  // RX (zero-copy): view the oldest frame the hardware has completed (descriptor owned by the CPU).
  // The span must stay valid until rx_release(). error::try_again if the ring is empty.
  reloco::result<reloco::span<const std::uint8_t>> rx_peek() noexcept;

  // RX: hand that descriptor back to the hardware. Called exactly once after every successful rx_peek().
  void rx_release() noexcept;

  // --- optional ---
  // True if the MAC leaves the 4-byte FCS on received frames (the adapter strips it). Default false.
  static constexpr bool rx_includes_fcs = false;
  // Largest frame the driver handles, without FCS. Default 1514 (1500 payload + 14 header).
  static constexpr std::size_t max_frame_size = 1514;
  // MAC filter controls, forwarded by ethernet_nic::set_promiscuous()/add_multicast();
  // without them those return error::unsupported_operation.
  reloco::result<void> set_promiscuous(bool on) noexcept;
  reloco::result<void> add_multicast(const structo::hw::net_mac_address &group) noexcept;
};
```

## Plugging it into the stack

```cpp
my_mac_driver drv;                                      // the hardware driver (registers, rings)
structo::hw::ethernet_nic<my_mac_driver> nic{drv};      // validated raw-frame backend; drv must outlive it
structo::hw::polled_net_device<decltype(nic)> pnd{nic}; // coroutine send/receive; pnd.poll() retries try_again
structo::hw::net_device_ref raw{pnd};                   // raw Ethernet frames in/out

std::uint64_t now_ms(void *) noexcept;                  // monotonic ms clock for ARP timers
structo::hw::ethernet_device<1500, 8> eth{raw, now_ms, nullptr}; // MAC comes from drv.read_mac()
structo::hw::net_device_ref ip_side{eth};               // IPv4 datagrams, what netstack consumes

// Then, as in ethernet_device.md: netstack{sched, ip_side, cfg}; net.poll_with(pnd); net.use_ethernet(eth);
```

Most MACs with an external PHY implement `link_up()` with [`mii_phy`](mii_phy.md)
(`phy.poll()`), reprogramming the MAC speed/duplex when `changed` is set.

An interrupt-driven driver can skip `polled_net_device` and implement
`net_device_traits` itself, resuming parked coroutines from its IRQ handler;
`ethernet_nic` is meant for polled rings.

## Reference driver

[`examples/netstack_tap_demo.cpp`](../examples/netstack_tap_demo.cpp)'s
`tap_driver` implements this contract on a Linux TAP file descriptor and is the
smallest complete example.
