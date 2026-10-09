<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Wiring the USB host stack on a Raspberry Pi

This guide shows how to put `usb_stack` (see [`usb_host.md`](usb_host.md)) on a real chip, using the Raspberry Pi as
the example. The same five steps apply to any SoC: **(1)** power the controller and map its registers, **(2)** write an
`Env` (registers, DMA memory, delay), **(3)** pick the controller driver and its config, **(4)** route the interrupt or
poll, **(5)** hand the controller to `bootldr::usb_stack` and register class drivers.

> The register addresses, bus aliases and mailbox tags below come from the public BCM283x / BCM2711 documentation and
> from memory of existing bare-metal projects. The drivers were tested only against register-level simulators, never on
> hardware: verify each value against your board's datasheet before relying on it.

## Which Pi has which controller

| Board | USB controller | Driver | Reachable devices |
| --- | --- | --- | --- |
| Pi Zero, Zero W, Zero 2 W (micro-USB OTG port) | DWC2 | [`dwc2_hcd`](dwc2_hcd.md) | Directly attached device (use an OTG cable) |
| Pi 1/2/3 (B models), 3B+ | DWC2, but the port feeds an **onboard hub** (LAN9514/LAN7515) holding Ethernet and the type-A sockets | [`dwc2_hcd`](dwc2_hcd.md) | **Nothing**: the stack has no hub support, so devices behind the hub are not reachable |
| Pi 4 / CM4 USB-C port | DWC2 (OTG, usually used as a gadget) | [`dwc2_hcd`](dwc2_hcd.md) | Directly attached device |
| Pi 4 type-A ports | VIA VL805 xHCI behind PCIe | [`xhci_hcd`](xhci_hcd.md) | Needs PCIe bring-up and VL805 firmware load first (see the end of this guide) |
| Pi 5 | RP1 southbridge xHCI behind PCIe | [`xhci_hcd`](xhci_hcd.md) | Needs PCIe/RP1 bring-up first |

The rest of the guide wires the **Pi Zero (DWC2)**, the simplest complete case.

## Step 1: power and map the controller

```cpp
// BCM2835 (Zero, Pi 1): peripherals at 0x20000000; BCM2837 (Zero 2 W, Pi 3): 0x3F000000; BCM2711 (Pi 4): 0xFE000000.
// The DWC2 register window is at peripheral base + 0x980000. Map it as device memory (non-cacheable, no reordering).
constexpr std::uintptr_t usb_base = 0x20000000 + 0x980000;

// The USB power domain is off after boot. Ask the VideoCore firmware to power it on through the mailbox property
// interface: tag 0x00028001 (SET_POWER_STATE), device id 3 (USB HCD), state = on | wait-for-stable (0b11).
// `mailbox_property_call` is your board's mailbox helper (a 16-byte aligned message buffer, channel 8); it is not part
// of structo because it depends on how you map and cache the mailbox registers.
bool power_on_usb() {
  struct set_power { std::uint32_t device_id; std::uint32_t state; };
  set_power req{3, 0b11};
  return mailbox_property_call(0x00028001, req) && (req.state & 1) /* power is on */ && !(req.state & 2) /* device exists */;
}
```

Also tell the firmware to enable VBUS if your board gates it (the Pi Zero USB port is always powered; on boards with a
GPIO-controlled switch, drive that GPIO yourself).

## Step 2: the `Env`

```cpp
#include <structo/hw/dwc2_hcd.hpp>
#include <structo/bootldr/usb_stack.hpp>

struct rpi_usb_env {
  // Pointer to the register window mapped in step 1; read32/write32 take BYTE offsets from it.
  volatile std::uint32_t *mmio = reinterpret_cast<volatile std::uint32_t *>(usb_base);

  // The scheduler provides the sleeping; the controller driver only sleeps in reset_port() (>= 50 ms USB reset).
  structo::bootldr::scheduler *sched = nullptr;

  std::uint32_t read32(std::size_t offset) noexcept { return mmio[offset / 4]; }
  void write32(std::size_t offset, std::uint32_t value) noexcept { mmio[offset / 4] = value; }

  // DMA memory: zeroed, physically contiguous, uncached (the drivers do no cache maintenance) and below 4 GiB.
  // The simplest way on a Pi is a static buffer in a region you mapped non-cacheable in the MMU, handed out by a
  // bump allocator (`dma_pool` here, your code; structo's buddy_allocator also works). `phys` is the address the USB
  // core's AHB master uses, NOT the ARM address: on the Pi that is the ARM physical address ORed with the VideoCore
  // "uncached" alias, 0xC0000000 (BCM2835/6/7). On BCM2711 the DWC2 sees ARM physical addresses directly (no alias).
  structo::hw::usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept {
    void *p = dma_pool.alloc(size, align);
    if (!p)
      return {};
    return {p, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(p)) | 0xC0000000u, size};
  }
  void dma_free(const structo::hw::usb_dma_buffer &buf) noexcept { dma_pool.free(buf.virt); }

  // Orders the CPU's writes to DMA memory before the register write that starts the transfer. On ARMv6 (Pi 1/Zero)
  // use a data synchronisation barrier (mcr p15, 0, r0, c7, c10, 4); on ARMv7/ARMv8 `dsb sy` (or the builtin below).
  void barrier() noexcept { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

  // Reuse the scheduler: the delay lets other tasks run (e.g. a UART shell) during the USB reset.
  reloco::task<void> delay_ms(unsigned ms) noexcept {
    (void)co_await sched->sleep_for(ms);
  }
};
```

## Step 3: the controller driver and its config

```cpp
rpi_usb_env env;
structo::bootldr::scheduler sched;
env.sched = &sched;
sched.set_clock(&system_timer_ms, nullptr);        // required for sleep_for(); see clock_ref.hpp for timer wrappers

structo::hw::dwc2_config cfg;
cfg.phy = structo::hw::dwc2_phy::utmi_8bit;        // the Pi's on-chip PHY is UTMI+; check GHWCFG2/4 if the core
                                                   // does not respond
cfg.rx_fifo_words = 1024;                           // shared receive FIFO (32-bit words)
cfg.np_tx_fifo_words = 1024;                        // control/bulk OUT FIFO
cfg.p_tx_fifo_words = 1024;                         // interrupt OUT FIFO; the sum must fit GHWCFG3.DfifoDepth or
                                                    // start() returns invalid_argument: lower the numbers if it does
cfg.ahb_burst = 0;                                  // single bursts: always safe, raise later for speed

structo::hw::dwc2_hcd<rpi_usb_env, 8> hcd{env, cfg}; // 8 transfers in flight at once (one host channel each)

if (!power_on_usb() || !hcd.start())               // start() resets the core, forces host mode, powers the port
  panic("usb: controller did not start");
```

## Step 4: interrupt or polling

Easiest first: **poll**. `usb_stack::poll_with` runs a function once per scheduler round, before the tasks, which is
also the context where completions resume coroutines (the scheduler has no locking, so never call `hcd.irq()` from a
real interrupt handler while tasks may run):

```cpp
void service_hcd(void *ctx) noexcept { static_cast<decltype(hcd) *>(ctx)->poll(); }
```

To use the interrupt instead (lower latency), the USB core is **GPU interrupt line 9** on BCM2835/6/7 (bit 9 of
`IRQ_PENDING1`, enable with `ENABLE_IRQS1`). In the handler only record "USB needs service" in an atomic flag and let
the poller above call `hcd.poll()` when the flag is set; the interrupt then just wakes the idle loop
(`wfi`) instead of polling continuously.

## Step 5: `usb_stack` and class drivers

```cpp
// A class driver, exactly as in usb_host.md: match on the descriptors, then run one coroutine per device.
bool match_serial(void *, const structo::usb::usb_device &d) noexcept {
  return d.config().find_interface(structo::usb::usb_class::cdc, structo::usb::cdc::subclass_acm).has_value();
}
reloco::task<void> serial_task(void *, structo::bootldr::usb_stack &stack, structo::bootldr::usb_device_ptr dev) noexcept {
  structo::usb::cdc_acm<256, 256> serial;
  if (!co_await serial.attach(dev->device()))
    co_return;
  auto &s = stack.sched();
  auto rx = s.spawn(serial.run_rx(), structo::bootldr::spawn_mode::joinable);
  auto tx = s.spawn(serial.run_tx(), structo::bootldr::spawn_mode::joinable);
  // ... use structo::hw::uart_ref{serial}, e.g. as a console, while the device is present ...
  co_await dev->gone_event().wait();
  serial.detach();
  if (rx) (void)co_await s.join(*rx);
  if (tx) (void)co_await s.join(*tx);
}

structo::hw::usb_host_controller_ref ref{hcd};     // type-erased controller handle the stack takes
structo::bootldr::usb_stack usb{sched, ref};       // the hot-plug manager
(void)usb.add_driver({&match_serial, &serial_task});
(void)usb.poll_with(&service_hcd, &hcd);           // service the controller once per scheduler round
(void)usb.start();                                 // one watcher task per root port: reset, enumerate, match, spawn
sched.run();                                       // never returns; plug a USB serial adapter and it attaches
```

Swap `cdc_acm` for `cdc_ecm` to get an Ethernet adapter as a `net_device_ref` (feed it to `net::stack` as in
`examples/netstack_usb_demo.cpp`), or `mass_storage` for a USB stick.

## Troubleshooting

- `start()` returns `unsupported_operation`: wrong register base or the core is still powered off (step 1).
- `start()` returns `timed_out`: clocks or resets are not released, or the register window is mapped cacheable.
- Port never reports connected: VBUS is off, or you used a plain cable instead of an OTG cable (ID pin must be grounded).
- Enumeration works but transfers time out: `phys` in `dma_alloc` is wrong (the ARM address without the bus alias) or the
  DMA memory is cacheable.
- Device behind a hub (all type-A sockets on Pi 1/2/3 B models) is never seen: hubs are not supported.

## Pi 4 type-A ports and Pi 5 (xHCI)

`xhci_hcd` is used in exactly the same way (see [`xhci_hcd.md`](xhci_hcd.md); `Env` is identical, with the xHCI
register window as `mmio`). What differs is the bring-up *before* `start()`, which structo does not provide:

- **Pi 4:** the VL805 sits behind the BCM2711 PCIe root complex. You must initialise the PCIe controller (clocks,
  reset, inbound DMA window so the device may reach RAM, outbound window for its BAR), then ask the firmware to load
  the VL805 firmware with the mailbox tag `0x00030058` (NOTIFY_XHCI_RESET) with the device's PCI address. Then map
  BAR0 and use it as `mmio`; `phys` is the PCIe inbound-window address of the buffer.
- **Pi 5:** the xHCI controller is inside the RP1 chip behind PCIe; the same applies (PCIe, then RP1 enumeration).
- Both cases use MSI or polling: use `poll()` from the scheduler as in step 4.
