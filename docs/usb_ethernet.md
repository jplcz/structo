<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Plugging the USB stack and a USB Ethernet adapter together

This guide shows how to get an IPv4 network out of a USB Ethernet adapter (CDC-ECM class) using `bootldr::usb_stack`,
`usb::cdc_ecm`, `hw::ethernet_device` and `bootldr::netstack`. It assumes the USB host controller already works (see
[`usb_host.md`](usb_host.md) and, for a chip walk-through, [`usb_host_raspberry_pi.md`](usb_host_raspberry_pi.md)).
A runnable version of everything below is [`examples/netstack_usb_demo.cpp`](../examples/netstack_usb_demo.cpp).

## The layers

```
UDP sockets (bootldr::udp_socket)          your application
bootldr::netstack<Mtu>                     IPv4, ICMP echo, UDP demux, optional DHCP client
hw::net_device_ref  <- ethernet_device     Ethernet framing: ARP, MAC filtering; presents IPv4 datagrams
hw::net_device_ref  <- usb::cdc_ecm        raw Ethernet frames over USB bulk endpoints
bootldr::usb_stack                         hot-plug: reset, enumerate, match, spawn one coroutine per device
hw::usb_host_controller_ref                OHCI / EHCI / xHCI / DWC2 driver
```

Everything the network needs lives in the coroutine the `usb_stack` spawns for the adapter, so unplugging the cable
ends the coroutine and tears the whole network down; plugging it again builds it again.

## Which adapters work

CDC-ECM class devices: Linux gadget Ethernet (`g_ether`, `ecm`), many USB-Ethernet dongles that offer an ECM
configuration, phones in "USB tethering (ECM)" mode, and other boards acting as a USB gadget. **Not supported:** RNDIS
(Windows-style tethering) and NCM, and vendor-specific chips (ASIX, Realtek ... in their native mode). Devices behind
a hub are not reachable (the stack has no hub support).

## Step 1: match the adapter

```cpp
// Called by usb_stack with the parsed descriptors of every newly connected device. Return true to claim it.
// `d` is the enumerated device; here we claim anything that exposes a CDC interface with the ECM subclass.
bool match_ecm(void * /*ctx*/, const structo::usb::usb_device &d) noexcept {
  return d.config().find_interface(structo::usb::usb_class::cdc, structo::usb::cdc::subclass_ecm).has_value();
}
```

## Step 2: the per-device coroutine

```cpp
constexpr std::size_t ip_mtu = 1500;   // largest IPv4 packet the Ethernet layer and the netstack buffer (Ethernet = 1500)

// usb_stack spawns this when match_ecm() returned true. `ctx` is the pointer given to add_driver (your settings),
// `stack` gives access to the scheduler, and `dev` is a shared pointer that keeps the device object alive for as
// long as this coroutine holds it.
reloco::task<void> ecm_driver(void *ctx, structo::bootldr::usb_stack &stack,
                              structo::bootldr::usb_device_ptr dev) noexcept {
  const auto &app = *static_cast<const app_settings *>(ctx);

  // 1. USB side: selects the ECM data alternate setting, reads the MAC string descriptor, applies the packet filter
  //    (directed, broadcast, all-multicast) and finds the bulk endpoints. Fails with the error of the first transfer that did not work.
  structo::usb::cdc_ecm ecm;
  if (auto r = co_await ecm.attach(dev->device()); !r)
    co_return;

  // 2. Present the adapter as a generic raw-frame network device. `ecm` must outlive every user of `raw`.
  structo::hw::net_device_ref raw{ecm};

  // 3. Ethernet layer on top of the raw device: adds/strips Ethernet headers, answers/sends ARP, drops frames not for
  //    our MAC. Template arguments: <ip_mtu = largest IP packet, 8 = ARP cache entries>. Arguments: the raw device,
  //    a millisecond clock (ARP timeouts) and its context pointer (nullptr = the clock needs none).
  structo::hw::ethernet_device<ip_mtu, 8> eth{raw, &now_ms, nullptr};
  structo::hw::net_device_ref ip_side{eth};      // what the netstack sees: IPv4 datagrams in and out

  // 4. IPv4 + UDP on top. Either a fixed address (static_ip) or DHCP; with `dhcp = true` the lease overrides static_ip.
  structo::bootldr::netstack_config cfg;
  cfg.static_ip = structo::net::ipv4_config::make_static(app.ip, app.netmask, app.gateway);
  // cfg.dhcp = true;                            // ...or ask the network for an address instead
  structo::bootldr::netstack<ip_mtu> net{stack.sched(), ip_side, cfg};
  net.use_ethernet(eth);                         // lets the netstack tell ARP which IP is ours
  if (!net.start()) {                            // spawns the receive and timer tasks
    ecm.detach();
    co_return;
  }

  // 5. Your application: sockets bound to ports. Tasks using a socket must be cancelled before the socket dies.
  structo::bootldr::udp_socket sock{net};
  if (sock.bind(app.port)) {
    auto echo = stack.sched().spawn(echo_task(sock));   // example service, see below

    // The device's gone_event is set when the cable is unplugged; every transfer fails from then on.
    co_await dev->gone_event().wait();

    if (echo)
      (void)stack.sched().cancel(*echo);
  }

  // 6. Teardown in reverse order, so nothing references a destroyed object.
  net.stop();
  ecm.detach();
}

// A tiny UDP echo: receive a datagram into `buf`, then send the same bytes back to whoever sent it.
reloco::task<void> echo_task(structo::bootldr::udp_socket &s) noexcept {
  reloco::array<std::uint8_t, 512> buf{};
  for (;;) {
    auto rx = co_await s.receive_from(reloco::span<std::uint8_t>(buf));   // fails once the socket/stack stops
    if (!rx)
      co_return;
    (void)co_await s.send_to(rx->source, rx->source_port, reloco::span<const std::uint8_t>(buf).first(rx->size));
  }
}
```

## Step 3: register everything with the stack

```cpp
structo::bootldr::scheduler sched;
sched.set_clock(&now_ms, nullptr);                  // required: usb_stack, ethernet_device and netstack all sleep

structo::hw::usb_host_controller_ref hcd_ref{hcd};  // from ohci_hcd / ehci_hcd / xhci_hcd / dwc2_hcd

structo::bootldr::usb_stack_config ucfg;
ucfg.poll_ms = 20;                                  // how often each root port is checked for connect/disconnect
ucfg.debounce_ms = 50;                              // how long a connection must be stable before enumeration
structo::bootldr::usb_stack usb{sched, hcd_ref, ucfg};

app_settings app{/* ip, netmask, gateway, port */};
(void)usb.add_driver({&match_ecm, &ecm_driver, nullptr, &app});   // {match, run, match ctx, run ctx}
(void)usb.poll_with(&service_hcd, &hcd);            // calls hcd.poll() once per scheduler round, before the tasks
(void)usb.start();                                  // one watcher task per root port
sched.run();                                        // plug the adapter: the network comes up by itself
```

`service_hcd` is a one-liner that calls `poll()` on the controller (or `irq()` for a controller flagged by an
interrupt, but never from the real interrupt context: the scheduler has no locking and completions resume
coroutines inline).

## Talking to it from a PC

Connect the adapter to the PC (cable, or a USB gadget on the other end) and give the PC's side an address in the
board's subnet. For the static address in the example (`192.168.78.2/24`):

```sh
sudo ip addr add 192.168.78.1/24 dev <the new interface>
sudo ip link set <the new interface> up
ping 192.168.78.2                                  # answered by the netstack's ICMP echo
echo hi | nc -u -w1 192.168.78.2 7                 # answered by echo_task
```

## Using DHCP, or a different consumer

- **DHCP:** set `cfg.dhcp = true`. The netstack sends DISCOVER/REQUEST, applies the lease and renews it; `net.config()`
  returns the current address. Leave `static_ip` empty or keep it as the fallback until the lease arrives.
- **Raw frames only:** skip `ethernet_device` and `netstack`; use `ecm.send()` / `ecm.receive()` (coroutines) or the
  `net_device_ref` directly, for example to run your own protocol.
- **A different adapter class:** only `match_*` and the first lines of the coroutine change; PPP over a CDC-ACM serial
  adapter is the other example in `netstack_usb_demo` (`--mode ppp`).

## Troubleshooting

- `attach` fails: the adapter is not ECM (check `lsusb -v` for `bInterfaceSubClass 6`), or a control transfer failed:
  look at the error code and at the controller driver's counters.
- ARP never resolves: the other end is on another subnet, or the adapter's packet filter was not set (the driver sets
  it in `attach`; some devices need a few hundred ms after SET_INTERFACE).
- Works once, not after replug: something still references objects from the old coroutine; make sure sockets and
  tasks are cancelled before `net.stop()` and that `ecm.detach()` runs before `ecm` is destroyed.
- No packets at all: the scheduler's clock is not set, or `poll()` is not called regularly.
