# USB host stack

> **C++20 only.** Everything here is coroutine based (`reloco::task`) and compiles to nothing
> without `RELOCO_HAS_COROUTINES`.

A small, allocation-free USB **host** stack: you write a controller driver (xHCI/EHCI/OHCI,
DWC2/DWC3, MUSB ... in host mode), the stack does enumeration and the class drivers.

| Header | What it is |
|---|---|
| `hw/usb_host_controller_ref.hpp` | `usb_host_controller_ref` + `usb_host_traits<Backend>`: the controller driver contract, transfer types, awaitable transfers |
| `usb/usb_defs.hpp` | Constants, `device_descriptor`, bounds-checked `config_view` (interfaces, endpoints, class-specific descriptors) |
| `usb/usb_host.hpp` | `usb_device` + `usb_host`: root-port enumeration, address allocation, control helpers |
| `usb/cdc_acm.hpp` | CDC-ACM virtual serial port -> `hw::uart_ref` backend |
| `usb/cdc_ecm.hpp` | CDC-ECM Ethernet adapter -> raw-frame `hw::net_device_ref` backend |
| `usb/mass_storage.hpp` | Bulk-Only Transport + SCSI -> coroutine API and a blocking `hw::block_device_ref` backend |
| `usb/event.hpp` | `event` (single-waiter wake-up) and `byte_ring` (SPSC byte ring) used by the class drivers |

**Scope:** root ports only (no hubs), one configuration (the first), high/full/low speed devices,
bulk/control/interrupt pipes (no isochronous), ECM only (**no RNDIS**), BOT only (no UASP), LUN 0.

## Execution model

Transfers are **interrupt driven**. A coroutine does `co_await hcd.in(pipe, buf)`; the controller
driver queues the transfer and later, from its IRQ handler, calls `transfer.complete(status, actual)`
which resumes the coroutine inline. A completion that arrives before the coroutine has suspended
(inline completion, or an IRQ on another CPU) is handled without losing the wake-up. Destroying a
suspended coroutine cancels the transfer via `cancel()`.

## Writing a controller driver

```cpp
// A host controller driver is a plain class; structo::hw::usb_host_traits tells the stack how to use it.
struct my_hcd { /* register block, schedule memory ... */ };

template <> struct structo::hw::usb_host_traits<my_hcd> {
  // Number of root hub ports.
  static unsigned port_count(my_hcd &) noexcept;

  // Read the root port status register: is something plugged in, is the port enabled, how fast.
  // `changed` reports a connect/disconnect since the last call.
  static reloco::result<structo::hw::usb_port_status> port_status(my_hcd &, unsigned port) noexcept;

  // Reset the port (hold reset >= 50 ms, then the 10 ms recovery time). A task because it takes time:
  // park on your timer interrupt. When it completes the port must be enabled and its speed known.
  static reloco::task<void> reset_port(my_hcd &, unsigned port) noexcept;

  // Put `t` on the hardware schedule. The request is in t.pipe (address, endpoint, type, max packet,
  // speed), t.setup (control transfers), t.data/t.length (buffer). Later -- from the IRQ handler, or right
  // here for an immediate completion -- call t.complete(status, actual_bytes) exactly once.
  // Return an error (error::not_found for "device gone") if it cannot be queued.
  static reloco::result<void> submit(my_hcd &, structo::hw::usb_transfer &t) noexcept;

  // The awaiting coroutine was destroyed: unlink `t`. The driver must not touch it afterwards.
  static void cancel(my_hcd &, structo::hw::usb_transfer &t) noexcept;

  // Optional: reset an endpoint's data toggle (called after CLEAR_FEATURE(ENDPOINT_HALT) / SET_INTERFACE).
  static void reset_data_toggle(my_hcd &, const structo::hw::usb_pipe &) noexcept;
};
```

The controller splits transfers into packets, tracks data toggles, retries NAKs (use
`usb_status::timeout` only for a real NAK timeout) and ends IN transfers on a short packet.
`usb_status` values map to `reloco::error` through `usb_completion::to_result()`: `timeout` ->
`timed_out`, `cancelled`/`disconnected` -> `operation_canceled`, others -> `io_error`.

## Enumerating a device

```cpp
my_hcd hcd;
structo::hw::usb_host_controller_ref ref{hcd};          // type-erased handle over the driver
structo::usb::usb_host host{ref, &delay_ms, nullptr};   // optional delay callback: task<void>(ctx, ms)
                                                        // used for the 2 ms SET_ADDRESS recovery time
std::uint8_t cfg[512];                                  // storage for the configuration descriptor
structo::usb::usb_device dev{ref, {cfg, sizeof cfg}};   // filled in by enumerate()

auto r = co_await host.enumerate(0, dev);               // port 0: reset, SET_ADDRESS, read descriptors,
                                                        // SET_CONFIGURATION (first configuration)
// dev.descriptor().vendor_id / product_id, dev.config() for the interface/endpoint walk.
host.release(dev);                                      // after disconnect: recycle the address
```

`enumerate` fails with `not_found` (nothing connected), `capacity_exceeded` (descriptor larger than
`cfg`), `invalid_argument` (malformed descriptors / bad port) or the transfer error.

## CDC-ACM serial port

```cpp
structo::usb::cdc_acm<256, 256> serial;       // RX and TX ring sizes (powers of two)
co_await serial.attach(dev);                  // finds comm + data interfaces, asserts DTR/RTS
structo::hw::uart_ref uart{serial};           // any code written against uart_ref now talks USB
auto rx = serial.run_rx();                    // pump coroutines: start them on your scheduler;
auto tx = serial.run_tx();                    // they end after detach() or a fatal transfer error
(void)uart.configure({.baud_rate = 9600});    // sent as SET_LINE_CODING by run_tx
uart.write_string("hello\r\n");               // queued in the TX ring, sent by run_tx
```

`uart_ref`'s non-blocking operations are safe from any context: they only touch lock-free rings.
`run_rx` only issues an IN transfer when the RX ring has room for a whole packet, so a slow reader
back-pressures the device instead of dropping bytes.

## CDC-ECM Ethernet

```cpp
structo::usb::cdc_ecm ecm;
co_await ecm.attach(dev);                     // reads the MAC (iMACAddress string), selects the data
                                              // interface alternate setting, sets the packet filter
structo::hw::net_device_ref raw{ecm};         // raw Ethernet frames (no FCS); feed it to
                                              // hw::ethernet_device or use it directly
co_await raw.send(frame);                     // one bulk OUT (+ zero-length packet if needed)
auto n = co_await raw.receive(buf);           // one frame per call; buf >= mtu() + 14 bytes
```

## Mass storage

```cpp
structo::usb::mass_storage msc;
co_await msc.attach(dev);                     // BOT interface, GET_MAX_LUN, TEST UNIT READY (with
                                              // REQUEST SENSE retries), READ CAPACITY(10)
co_await msc.read_blocks(lba, sectors);       // size must be a multiple of msc.block_size()
co_await msc.write_blocks(lba, sectors);      // READ/WRITE(10), split into 16 KiB commands
co_await msc.flush();                         // SYNCHRONIZE CACHE

// For code that cannot await (e.g. a filesystem reader): run a transfer to completion by polling.
// `pump` services the controller once (check its interrupt status / WFI); after 100000 calls the
// operation is cancelled and fails with error::timed_out.
structo::usb::usb_msc_block_device disk{msc, &pump_hcd, &hcd, 100000};
structo::hw::block_device_ref blk{disk};      // try_read_blocks / try_write_blocks / try_flush
```

Errors: a failed command status is `io_error`; a stalled endpoint is cleared; a phase error or malformed
CSW triggers Bulk-Only reset recovery (class reset + clear both bulk halts) and `io_error`.

## Guide: hot-plug detection and handling a device

The stack has no hidden thread: *you* own one long-running coroutine per root port that waits for a
port-change interrupt, then reacts. The building blocks are `usb_host_controller_ref::port_status()`
(its `changed` flag is set on connect/disconnect and cleared by the read), `usb_host::enumerate()`,
`config_view::find_interface()` to decide which class driver fits, and `release()` on removal.

Controller driver requirements for this to work:
- Your root-hub/port-change IRQ handler must wake the port task (below, `port_event.notify()`).
- When a device disappears, complete every transfer still queued for it with
  `usb_status::disconnected`. Class pump tasks then see `operation_canceled`, detach and finish.

```cpp
struct usb_port_manager {
  structo::hw::usb_host_controller_ref hcd;       // the controller driver handle
  structo::usb::usb_host host;                    // enumerator + address allocator
  structo::usb::event port_event;                 // notified by the controller's port-change IRQ
  std::uint8_t cfg[512];                          // configuration descriptor storage
  structo::usb::usb_device dev{hcd, {cfg, sizeof cfg}};
  structo::usb::cdc_acm<256, 256> serial;         // class drivers we are willing to handle
  structo::usb::mass_storage msc;
  reloco::task<void> pumps[2];                    // keeps the class pump coroutines alive
  bool present = false;

  // Your IRQ handler calls this when the root hub reports a port change.
  void on_port_irq() noexcept { port_event.notify(); }

  // Start once per port from your scheduler (or resume() it from the main loop).
  reloco::task<void> run(unsigned port) noexcept {
    for (;;) {
      // Sleep until the controller says something changed. Wake-ups may be spurious, so always
      // re-read the real state instead of trusting the event.
      co_await port_event.wait();
      auto st = hcd.port_status(port);            // also clears the `changed` flag
      if (!st)
        continue;

      if (present && !st->connected) {            // ---- unplug ----
        serial.detach();                          // lets run_rx/run_tx finish
        msc.detach();                             // later calls fail with not_initialized
        host.release(dev);                        // address goes back to the pool
        present = false;
        continue;
      }
      if (present || !st->connected)
        continue;                                 // nothing new

      // ---- plug-in ----
      // Real connectors bounce: wait ~100 ms (your timer) before resetting the port.
      co_await debounce_100ms();
      auto e = co_await host.enumerate(port, dev); // reset, address, descriptors, SET_CONFIGURATION
      if (!e)
        continue;                                 // unsupported/broken device: wait for the next change
      present = true;

      // Pick a class driver from what the device actually exposes.
      auto cfgv = dev.config();
      if (cfgv.find_interface(structo::usb::usb_class::cdc, structo::usb::cdc::subclass_acm)) {
        if (co_await serial.attach(dev)) {
          pumps[0] = serial.run_rx();             // tasks are lazy: resume() starts them; from then
          pumps[1] = serial.run_tx();             // on the controller IRQ drives them
          pumps[0].resume();
          pumps[1].resume();
          // Use it from anywhere: structo::hw::uart_ref{serial}.write_string("hello\r\n");
        }
      } else if (cfgv.find_interface(structo::usb::usb_class::mass_storage, 0x06, 0x50)) {
        if (co_await msc.attach(dev)) {
          // msc.read_blocks(...) / structo::usb::usb_msc_block_device -> block_device_ref
        }
      }
      // else: no driver for this device. It stays configured but unused until unplugged.
    }
  }
};
```

Notes:
- **Do not trust `connected` alone at boot**: a device already plugged in raises no change event.
  Call `port_event.notify()` once at start-up to run the first check.
- **One device per port task.** Hubs are not supported, so a root port hosts exactly one device. For
  several ports run one `usb_port_manager::run(port)` per port, each with its own `usb_device`.
- **No IRQ-driven port-change support?** Poll instead: replace the `port_event.wait()` with
  `co_await delay_ms(100)` and rely on `port_status().connected` (compare with `present`).
- **Transfers in flight during unplug** must be completed (not dropped) by the controller driver, see
  above; destroying a pump task also cancels its transfer through `cancel()`.
- **Re-attach after unplug**: `enumerate()` can be called again on the same `usb_device`; class driver
  objects are re-`attach()`ed. Pump tasks of the previous session must have finished (they end on
  `detach()`) before you assign new ones.
- **Printing what was plugged in**: `dev.descriptor().vendor_id` / `product_id` and
  `dev.get_string(dev.descriptor().product_string, buf)`.

## `bootldr::usb_stack`: managed hot-plug with a coroutine per device

`structo/bootldr/usb_stack.hpp` automates the guide above on top of the bootloader scheduler. It
polls every root port, debounces, enumerates, asks your registered drivers whether they want the
device, and spawns the matching driver's coroutine. State is heap-allocated and each device is a
`reloco::shared_ptr<usb_attached_device>`, so it stays valid until the last coroutine drops it.

```cpp
// Matcher: runs after enumeration; return true to claim the device.
bool match_acm(void*, const structo::usb::usb_device& d) noexcept {
  return d.config().find_interface(structo::usb::usb_class::cdc, structo::usb::cdc::subclass_acm).has_value();
}

// Per-device coroutine, spawned detached by the stack. `dev` is shared ownership of the device.
reloco::task<void> acm_attach(void* ctx, structo::bootldr::usb_stack& stack,
                              structo::bootldr::usb_device_ptr dev) noexcept {
  // ... set up class driver, spawn pumps using dev->device() ...
  // On unplug every transfer of this device fails with operation_canceled (it is NOT
  // hard-cancelled), so pumps end on their own; gone_event() is set to wake idle waiters.
  co_await dev->gone_event().wait();
  // clean up here; leaving the coroutine drops the shared_ptr.
}

structo::bootldr::usb_stack stack{sched, hcd_ref};       // hcd_ref: usb_host_controller_ref (must outlive stack)
stack.add_driver({&match_acm, &acm_attach, /*detached*/ nullptr, /*ctx*/ nullptr});
stack.poll_with(&pump_controller_completions, nullptr);  // controller completions must run on the scheduler thread
stack.start();                                           // needs scheduler.set_clock(); one watcher per root port
```

- `set_event_handler()` reports `attached`, `unclaimed`, `detached` and `enumeration_failed`.
- A failed enumeration is not retried until the device is unplugged.
- `usb_driver::detached` (optional) is a synchronous callback invoked just before transfers fail.
- `notify_port_change()` from the port IRQ shortens the poll interval; `stop()` detaches all devices.
- Limits: root ports only, no hubs, no RNDIS.

## Linux demos

Both demos fake the USB controller and device, so they need no hardware. They print a `NEXT:` line
telling you what to run on the host; the demo does nothing visible until you do.

```sh
# Serial log console: reads log lines the "board" sends over a fake CDC-ACM device.
./usb_serial_log_demo                 # prints /dev/pts/N
screen /dev/pts/N 115200              # in another terminal

# PPP over USB serial: the board sends LCP Configure-Requests until pppd answers.
./netstack_usb_demo --mode ppp        # prints /dev/pts/N
sudo pppd /dev/pts/N 115200 192.168.7.1:192.168.7.2 noauth local nodetach
echo hi | nc -u -w1 192.168.7.2 7000  # UDP echo

# Ethernet over USB (CDC-ECM) bridged to a TAP device.
ip tuntap add dev structo1 mode tap user $USER
ip addr add 192.168.78.1/24 dev structo1 && ip link set structo1 up
./netstack_usb_demo --mode ecm --tap structo1 --ip 192.168.78.2

# kill -USR1 <pid> toggles the fake cable (unplug/replug); the driver coroutine ends by itself.
```

## Host controller drivers

Generic controller drivers implement `usb_host_traits` on top of a small
board-supplied `Env` (MMIO, DMA, delay; see `hw/usb_hcd_env.hpp`):
[OHCI](ohci_hcd.md), [EHCI](ehci_hcd.md), [xHCI](xhci_hcd.md) and the Synopsys DesignWare
USB 2.0 OTG core in host mode ([DWC2](dwc2_hcd.md)).
Scope: control, bulk and interrupt transfers on root ports only.
A chip walk-through (Raspberry Pi) is in [usb_host_raspberry_pi.md](usb_host_raspberry_pi.md); how to get IPv4 over a
USB Ethernet adapter is in [usb_ethernet.md](usb_ethernet.md), and PPP/SLIP over a USB serial adapter in
[usb_serial_ip.md](usb_serial_ip.md).

## Testing

`tests/test_usb_host.cpp` contains a fake controller (inline, deferred and NAK-polled completion,
cancellation) and simulated CDC-ACM, CDC-ECM and mass-storage devices; use it as a template for
testing your own class drivers without hardware.
