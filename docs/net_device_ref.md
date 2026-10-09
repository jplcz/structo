<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::net_device_ref`

`include/structo/hw/net_device_ref.hpp`

A type-erased, non-owning, *coroutine-based* handle over a frame-oriented
network device (SLIP, PPP, Ethernet, ...), plus the
`net_device_traits<Backend>` customization point. `send` and `receive`
return `reloco::task`s; how a backend completes them is its own business, so
the same consumer code runs over a polled device (`polled_net_device`, e.g.
[`slip_device`](slip_device.md)) or an interrupt-driven one that resumes the
parked coroutine from its ISR. Same shape as [`uart_ref`](uart_ref.md): a
context pointer plus a `const vtable *`, no RTTI.

> **C++ standard:** C++20 only. The header is empty when
> `RELOCO_HAS_COROUTINES` is 0.

Unbound refs fail every operation with `error::unsupported_operation`
(the returned tasks complete immediately with it; `mtu()` returns `0`).

## Usage

```cpp
template <> struct structo::hw::net_device_traits<my_nic> {
  // Largest frame (as seen by the stack) the device can carry.
  static std::size_t mtu(const my_nic &) noexcept { return 1500; }
  // Is the link usable now (carrier up, PPP LCP opened, ...)?
  static reloco::result<bool> link_up(my_nic &n) noexcept { return n.carrier(); }
  // Completes with the length of one received frame copied into `dst`.
  // `dst` stays valid until the task finishes.
  static reloco::task<std::size_t> receive(my_nic &n, reloco::span<std::uint8_t> dst) noexcept;
  // Completes once the whole frame has been queued for transmission.
  static reloco::task<void> send(my_nic &n, reloco::span<const std::uint8_t> frame) noexcept;

  // Optional: link-layer address. If absent (SLIP/PPP have none),
  // net_device_ref::mac_address() fails with unsupported_operation.
  static reloco::result<structo::hw::net_mac_address> mac_address(my_nic &n) noexcept;
};

my_nic nic;
structo::hw::net_device_ref dev{nic};         // explicit, non-owning: `nic` must outlive
                                              // dev and its copies; rvalues are rejected

std::size_t max = dev.mtu();                  // size receive buffers with this
auto up = dev.link_up();                      // result<bool>
auto mac = dev.mac_address();                 // result<net_mac_address> = array<uint8_t, 6>

reloco::task<void> pump(structo::hw::net_device_ref dev) {
  std::array<std::uint8_t, 1500> buf;         // must stay valid until the awaited task completes
  // Resolves to the received frame length.
  auto n = co_await dev.receive(buf);
  // Frame to transmit; the span must stay valid until send completes.
  co_await dev.send(reloco::span<const std::uint8_t>(buf.data(), n.value()));
}
```

## API

| Member | Description |
| --- | --- |
| `net_mac_address` | `reloco::array<std::uint8_t, 6>`. |
| `net_device_ref()` | Unbound ref. |
| `explicit net_device_ref(Backend &)` | Binds a backend with `net_device_traits` providing `mtu`, `link_up`, `send`, `receive`. Rvalues are deleted. |
| `explicit operator bool()` | Whether bound. |
| `mtu()` | Maximum frame size; `0` if unbound. |
| `link_up()` | `result<bool>`. |
| `mac_address()` | `result<net_mac_address>`; `unsupported_operation` if the backend has no `mac_address`. |
| `send(span<const uint8_t>)` | `task<void>` completing once the frame is queued. |
| `receive(span<uint8_t>)` | `task<size_t>` completing with the received frame length. |

See also [`ethernet_device`](ethernet_device.md).
