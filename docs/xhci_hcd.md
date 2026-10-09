<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# xHCI host controller driver

> **C++20 only.** `hw/xhci_hcd.hpp` is empty without `RELOCO_HAS_COROUTINES`.

`structo::hw::xhci_hcd<Env, MaxTransfers = 16>` is a generic, header-only xHCI 1.1/1.2 driver (USB 2.0 and
USB 3.x root ports). It plugs into `usb_host_controller_ref` (see [usb_host.md](usb_host.md)), so
`usb::usb_host`, the class drivers and `bootldr::usb_stack` run on top unchanged.

## Env

The `Env` contract is shared with the OHCI/EHCI drivers (`hw/usb_hcd_env.hpp`).

```cpp
struct my_xhci_env {
  // Address of the first capability register (CAPLENGTH). Set from the PCI BAR / device tree.
  volatile std::uint32_t *mmio;

  // 32-bit register access; `offset` is a byte offset from `mmio`, always 4-byte aligned.
  // 64-bit registers are accessed as two 32-bit writes, low half first.
  std::uint32_t read32(std::size_t offset) noexcept { return mmio[offset / 4]; }
  void write32(std::size_t offset, std::uint32_t v) noexcept { mmio[offset / 4] = v; }

  // Zeroed, physically contiguous, coherent memory aligned to `align` (<= 4096). `phys` is the
  // bus address the controller uses. Used for rings, contexts, DCBAA, scratchpads and bounce buffers.
  structo::hw::usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept;
  void dma_free(const structo::hw::usb_dma_buffer &) noexcept;

  // Orders descriptor writes before doorbells / register writes.
  void barrier() noexcept { std::atomic_thread_fence(std::memory_order_seq_cst); }

  // Non-blocking sleep, used by port reset and its recovery time.
  reloco::task<void> delay_ms(unsigned ms) noexcept;
};
```

## Wiring

```cpp
my_xhci_env env{...};
structo::hw::xhci_hcd<my_xhci_env> hcd{env};  // 16 transfers in flight
if (!hcd.start())      // halt, HCRST, rings, scratchpads, RUN + INTE; every wait is bounded
  panic();
structo::hw::usb_host_controller_ref ref{hcd};  // what usb_host / usb_stack take

// Interrupt handler, or call from a polling loop. It drains the event ring (command completions,
// transfer events, port changes), advances ERDP and acknowledges IMAN.IP / USBSTS.EINT.
hcd.irq();   // hcd.poll() is an alias
```

`stop()` (also run by the destructor) halts the controller and completes outstanding transfers with
`usb_status::cancelled`.

With `bootldr::usb_stack`, hand it `ref`; it polls `port_status()`, calls `reset_port()` and cancels
transfers on unplug. Keep `irq()`/`poll()` running (e.g. via `usb_stack::poll_with`).

## What happens transparently

* **SET_ADDRESS**: xHCI cannot send it on the wire. `submit()` intercepts the standard request on
  address 0 and runs Enable Slot, then Address Device (BSR=0). The transfer then completes successfully
  and the USB address from the setup packet is mapped to the slot for later pipes.
* **Address-0 rule**: a pipe carries no port, yet xHCI needs a slot even for the first
  GET_DESCRIPTOR. Every `pipe.address == 0` transfer is bound to the root port most recently passed to
  `reset_port()` (`usb_host` enumerates one port at a time, resetting first). It uses Address Device with BSR=1.
* **EP0 max packet** differing from the context triggers Evaluate Context (forced to 512 on SuperSpeed).
* **Endpoints** are configured lazily (Configure Endpoint) on the first transfer to each
  (address, endpoint, direction), with one 16-TRB ring each.
* **Transfers**: control = Setup (IDT) + optional Data + Status; bulk/interrupt = Normal TRBs
  (chained, 64 KiB each, ISP on IN). Short packets give `actual = requested - residual`.
* **Errors**: Stall -> `stall`, Babble -> `babble`, other codes -> `bus_error`. The endpoint is then recovered
  (Reset Endpoint + Set TR Dequeue Pointer), so later transfers work.
* **cancel()** runs Stop Endpoint + Set TR Dequeue Pointer; the bounce buffer is freed only afterwards.
  The `usb_transfer` is never touched after `cancel()` returns.
* **Unplug** (CCS=0): Disable Slot; pending transfers complete with `usb_status::disconnected`.
* **reset_data_toggle()**: toggles live in hardware, so the endpoint is reset via Reset Endpoint + Set TR Dequeue.
* **Port reset**: PR (USB2) or warm reset (USB3 inactive/compliance), bounded wait for PRC/WRC, then
  PED=1 / PLS=U0 is verified and a 10 ms recovery is applied. PORTSC writes preserve PED and never clobber RW1C bits.
* **Speed**: PORTSC port speed is mapped through the supported-protocol PSI table, falling back to the default
  table (`usb_speed::super` / `super_plus` were added).

## Errors from `submit()`

`try_again` (all `MaxTransfers` slots busy), `not_found` (unknown address, or address 0 with no port reset yet),
`resource_exhausted` (endpoint table full), `unsupported_operation` (isochronous), `out_of_range` (transfer
too large), `invalid_argument`.

## Limits

Root ports only (no hubs / transaction translators), no isochronous, no streams, no SuperSpeed companion
burst tuning (max burst 0), at most 8 slots and 8 endpoints per slot, about 850 KB per transfer, 16 root ports,
one command in flight with no command timeout, little-endian host.
