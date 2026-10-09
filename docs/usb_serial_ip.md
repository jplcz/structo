<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# IPv4 over a USB serial adapter: PPP and SLIP examples

A USB CDC-ACM device (a USB "serial port": a phone/board in serial-gadget mode, an FTDI-less dongle, Linux
`g_serial`, another microcontroller) is a byte pipe. Run **PPP** or **SLIP** over it and the board gets an IPv4
link without needing an Ethernet class driver. Both examples below sit on `bootldr::usb_stack` +
`usb::cdc_acm` (see [`usb_host.md`](usb_host.md)) and feed the same `bootldr::netstack` as the Ethernet case
([`usb_ethernet.md`](usb_ethernet.md)). A runnable PPP version is `examples/netstack_usb_demo.cpp --mode ppp`.

| | PPP (`hw::ppp_device`) | SLIP (`hw::slip_device`) |
| --- | --- | --- |
| Address assignment | negotiated (IPCP): the peer can give the board its address and DNS | none: both ends are configured by hand |
| Link up/down detection | yes (LCP) | no (always "up") |
| Host side | `pppd` | `slattach` + `ifconfig`/`ip` |
| Overhead / complexity | higher | tiny |

The layers are the same in both cases:

```
UDP sockets -> bootldr::netstack -> polled_net_device -> ppp_device / slip_device -> uart_ref -> usb::cdc_acm -> usb_stack
```

## Matching the serial adapter (shared by both examples)

```cpp
// Called by usb_stack for every newly connected device. Return true to claim it: here, anything exposing a
// CDC interface with the ACM (abstract control model = serial port) subclass.
bool match_acm(void * /*ctx*/, const structo::usb::usb_device &d) noexcept {
  return d.config().find_interface(structo::usb::usb_class::cdc, structo::usb::cdc::subclass_acm).has_value();
}

constexpr std::size_t ip_mtu = 1500;  // largest IPv4 datagram buffered by the netstack / PPP (SLIP: use 1006, RFC 1055)

// Retries parked coroutine sends/receives of the polled network device; one scheduler round per iteration.
template <class P> reloco::task<void> poll_loop(P &pnd, structo::bootldr::scheduler &sched) noexcept {
  for (;;) {
    pnd.poll();
    co_await sched.yield();
  }
}
```

## Example 1: PPP

```cpp
// usb_stack spawns this when match_acm() returned true. `ctx` is the pointer given to add_driver (your settings),
// `stack` gives access to the scheduler, `dev` keeps the USB device object alive while this coroutine holds it.
reloco::task<void> ppp_driver(void *ctx, structo::bootldr::usb_stack &stack,
                              structo::bootldr::usb_device_ptr dev) noexcept {
  const auto &app = *static_cast<const app_settings *>(ctx);
  auto &sched = stack.sched();

  // 1. USB side: finds the bulk endpoints (the line coding is sent when the UART configuration is applied). The two pumps then move bytes between the
  //    endpoints and the UART rings (512/512 = RX/TX ring sizes in bytes); they end by themselves on detach.
  structo::usb::cdc_acm<512, 512> acm;
  if (auto r = co_await acm.attach(dev->device()); !r)
    co_return;
  auto rx = sched.spawn(acm.run_rx(), structo::bootldr::spawn_mode::joinable);
  auto tx = sched.spawn(acm.run_tx(), structo::bootldr::spawn_mode::joinable);

  {
    // 2. PPP settings. The board is usually up long before pppd is started on the host, so keep sending
    //    Configure-Requests every second instead of giving up after the default 10 tries (3 s apart).
    structo::net::ppp_config pcfg;
    pcfg.restart_ms = 1000;
    pcfg.max_configure = ~0u;                 // never give up
    // pcfg.local_address = ...;              // request a specific address; default 0.0.0.0 = let the peer choose
    // pcfg.peer_address = ...;               // address to hand to the peer if it asks us for one

    // 3. PPP over the serial line. Arguments: the UART (hw::uart_ref of the ACM device), a millisecond clock for the
    //    protocol timers + its context pointer (nullptr = none). It starts negotiating immediately.
    structo::hw::ppp_device<ip_mtu> ppp{structo::hw::uart_ref{acm}, &now_ms, nullptr, pcfg};
    structo::hw::polled_net_device<decltype(ppp)> pnd{ppp};   // adds coroutine send/receive to the polled device
    structo::hw::net_device_ref nic{pnd};                     // what the IP stack consumes

    // 4. IPv4 + UDP. No static address: it comes from IPCP once the link is open; use_ppp() connects the netstack
    //    to the PPP link so it follows that address (and drops it when the link goes down).
    structo::bootldr::netstack<ip_mtu> net{sched, nic};
    net.use_ppp(ppp.link());

    // 5. Poll the device from a task; then run your application until the cable is pulled.
    auto poller = sched.spawn(poll_loop(pnd, sched));
    if (poller && net.start()) {
      structo::bootldr::udp_socket sock{net};
      if (sock.bind(app.port)) {
        auto echo = sched.spawn(echo_task(sock));             // see usb_ethernet.md for echo_task
        co_await dev->gone_event().wait();                    // set when the device is unplugged
        if (echo)
          (void)sched.cancel(*echo);                          // tasks using the socket must end before it does
      }
    }
    net.stop();
    if (poller)
      (void)sched.cancel(*poller);
  }

  // 6. Wake the pumps and wait for them BEFORE `acm` is destroyed.
  acm.detach();
  if (rx)
    (void)co_await sched.join(*rx);
  if (tx)
    (void)co_await sched.join(*tx);
}
```

Registration, identical for both examples (substitute the driver function):

```cpp
structo::bootldr::usb_stack usb{sched, hcd_ref};            // hcd_ref: hw::usb_host_controller_ref of your controller
(void)usb.add_driver({&match_acm, &ppp_driver, nullptr, &app}); // {match, run, match ctx, run ctx}
(void)usb.poll_with(&service_hcd, &hcd);                    // calls hcd.poll() once per scheduler round
(void)usb.start();
sched.run();
```

Host side (the other end of the cable; on Linux the gadget shows up as `/dev/ttyACM0`):

```sh
# local:remote addresses; the board takes 192.168.7.2 from the peer through IPCP.
sudo pppd /dev/ttyACM0 115200 192.168.7.1:192.168.7.2 noauth local nodetach
echo hi | nc -u -w1 192.168.7.2 7                           # answered by echo_task
```

## Example 2: SLIP

SLIP has no negotiation, so the board's address is configured statically, and `slip_device` must be *serviced*
so its UART transmit FIFO is fed even when no coroutine is waiting to send.

```cpp
reloco::task<void> slip_driver(void *ctx, structo::bootldr::usb_stack &stack,
                               structo::bootldr::usb_device_ptr dev) noexcept {
  const auto &app = *static_cast<const app_settings *>(ctx);
  auto &sched = stack.sched();

  // 1. USB side, exactly as in the PPP example.
  structo::usb::cdc_acm<512, 512> acm;
  if (auto r = co_await acm.attach(dev->device()); !r)
    co_return;
  auto rx = sched.spawn(acm.run_rx(), structo::bootldr::spawn_mode::joinable);
  auto tx = sched.spawn(acm.run_tx(), structo::bootldr::spawn_mode::joinable);

  {
    // 2. SLIP framing over the serial line. The template argument is the largest frame: 1006 bytes is the RFC 1055
    //    limit (the Linux default MTU of a SLIP interface is 296: use it if the host side does not set mtu).
    structo::hw::slip_device<1006> slip{structo::hw::uart_ref{acm}};
    structo::hw::polled_net_device<decltype(slip)> pnd{slip};
    structo::hw::net_device_ref nic{pnd};

    // 3. IPv4 + UDP with a fixed address: there is no negotiation to learn one from. Arguments of make_static:
    //    our address, the netmask, the gateway (0.0.0.0 = none; the link is point-to-point).
    structo::bootldr::netstack_config cfg;
    cfg.static_ip = structo::net::ipv4_config::make_static(app.ip, structo::net::ipv4_address{255, 255, 255, 0}, {});
    structo::bootldr::netstack<1006> net{sched, nic, cfg};

    // 4. A task that feeds the UART transmit FIFO and retries parked sends/receives every round.
    auto pump = sched.spawn([](decltype(slip) &s, decltype(pnd) &p, structo::bootldr::scheduler &sc) -> reloco::task<void> {
      for (;;) {
        s.service();                                          // moves queued frame bytes to the UART
        p.poll();
        co_await sc.yield();
      }
    }(slip, pnd, sched));

    if (pump && net.start()) {
      structo::bootldr::udp_socket sock{net};
      if (sock.bind(app.port)) {
        auto echo = sched.spawn(echo_task(sock));
        co_await dev->gone_event().wait();
        if (echo)
          (void)sched.cancel(*echo);
      }
    }
    net.stop();
    if (pump)
      (void)sched.cancel(*pump);
  }

  // 5. Same shutdown order as PPP.
  acm.detach();
  if (rx)
    (void)co_await sched.join(*rx);
  if (tx)
    (void)co_await sched.join(*tx);
}
```

Host side with Linux's SLIP line discipline:

```sh
sudo slattach -L -p slip -s 115200 /dev/ttyACM0 &            # turns the serial port into interface sl0
sudo ifconfig sl0 192.168.7.1 pointopoint 192.168.7.2 mtu 1006 up   # the board is 192.168.7.2 (app.ip)
echo hi | nc -u -w1 192.168.7.2 7
```

## Notes

- A CDC-ACM serial device ignores the baud rate on most USB gadgets, but the value is still passed through; pick
  the one the peer expects.
- If both a CDC-ECM and a CDC-ACM match, register the driver you want first or tighten `match_*` (for example on
  vendor/product ids): the first matching driver wins.
- Only the last lines of the coroutine differ between PPP and SLIP, so you can pick at run time (for example from a
  boot setting) and keep one `match_acm`.
- SLIP and PPP carry no Ethernet framing: no ARP and no MAC addresses.
