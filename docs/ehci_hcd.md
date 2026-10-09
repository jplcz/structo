<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::ehci_hcd` — generic EHCI 1.0 host controller

Header: `structo/hw/ehci_hcd.hpp` (empty unless `RELOCO_HAS_COROUTINES`). Tests: `tests/test_ehci_hcd.cpp`
(register-level model of the controller that executes the driver's QH/qTD schedule against `usb_sim::device`).

`ehci_hcd<Env, MaxTransfers>` drives a USB 2.0 high-speed controller through MMIO + DMA only and plugs into the generic
USB stack through `usb_host_traits<ehci_hcd<...>>`, i.e. `structo::hw::usb_host_controller_ref ref{hcd};`.

## Env

The `Env` contract is shared with `ohci_hcd` / `xhci_hcd` (`structo/hw/usb_hcd_env.hpp`). Offsets passed to
`read32`/`write32` are bytes from the first capability register.

```cpp
class my_ehci_env {
public:
  // What: 32-bit MMIO read at `off` bytes from the start of the EHCI capability registers (CAPLENGTH).
  // Why: the driver finds the operational registers itself (CAPLENGTH), so the Env never needs to know them.
  // off: byte offset from the MMIO window base, 4-byte aligned.
  std::uint32_t read32(std::size_t off) noexcept { return mmio_[off / 4]; }

  // What: 32-bit MMIO write. Same offset convention as read32.
  void write32(std::size_t off, std::uint32_t v) noexcept { mmio_[off / 4] = v; }

  // What: allocates ZEROED, physically contiguous, cache-coherent memory for QH/qTD/frame list/bounce buffers.
  // Why: the controller reads and writes these structures behind the CPU's back.
  // size: bytes wanted; align: required alignment of the *bus* address (the driver asks for 4096).
  // Result: `virt` is the CPU pointer, `phys` the address the controller must use. The driver requires
  // `phys + size <= 4 GiB` (it never programs the high dwords / CTRLDSSEGMENT other than 0) and fails
  // start() / submit() otherwise.
  structo::hw::usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept;

  // What: releases a block returned by dma_alloc.
  void dma_free(const structo::hw::usb_dma_buffer &b) noexcept;

  // What: orders CPU writes to DMA memory before the following MMIO write (e.g. a dmb/sfence).
  void barrier() noexcept;

  // What: suspends the calling coroutine for `ms` milliseconds (port reset timing: 50 ms reset, 10 ms recovery).
  // ms: delay in milliseconds; an Env without a timer may complete immediately only in simulation.
  reloco::task<void> delay_ms(unsigned ms) noexcept;
};
```

## Bring-up and interrupts

```cpp
my_ehci_env env;
// What: the driver instance. 16 = how many transfers can be in flight at once (each owns one queue head,
// 16 qTDs and a bounce buffer; the pool also keeps as many spare slots for queue heads that are still
// being unlinked from the hardware).
structo::hw::ehci_hcd<my_ehci_env, 16> hcd{env};

// What: halt + HCRESET, allocate frame list / queue heads, start both schedules, CONFIGFLAG=1, power ports.
// Fails (bounded, never hangs) with timed_out / unsupported_operation / allocation_failed.
if (!hcd.start()) { /* report */ }

// What: the interrupt service routine body; acknowledges USBSTS, completes finished transfers and
// processes the async-advance doorbell. Call it from the IRQ handler *or* poll with it:
// hcd.poll() is the same function. Without an interrupt line call poll() every few ms.
void on_ehci_irq() noexcept { hcd.irq(); }

// What: the type-erased controller handle taken by usb::usb_host and bootldr::usb_stack.
structo::hw::usb_host_controller_ref ref{hcd};
```

Use with the hot-plug manager: `bootldr::usb_stack stack{sched, ref, config};` — the stack polls `port_status()`,
calls `reset_port()` and cancels pending transfers when a device disappears; run `hcd.poll()` from a scheduler poller
if no interrupt is wired.

## Design

* **Schedules.** Async list: one permanently linked dummy QH (H bit); each transfer gets its own QH inserted
  behind it and removed on completion. Periodic list: 1024-entry frame list, every entry points to a dummy QH
  (S-mask 0, halted); interrupt QHs are chained behind it with S-mask `0x01`, i.e. polled once **every frame**
  (the endpoint's `bInterval` is ignored — a valid, if chatty, schedule for one-shot transfers).
* **Transfers.** Control = SETUP + data qTDs + STATUS. Bulk/interrupt = qTDs of up to 20 KiB (multiples of
  max-packet) over a page-aligned bounce buffer; at most 16 qTDs per transfer (≈ 320 KiB). A short IN packet
  ends a bulk transfer (control: jumps to the status stage via the Alternate Next pointer).
* **Data toggles** are kept in software per (address, endpoint, direction) and loaded into qTD token bit 31
  (DTC=1). They reset on `reset_data_toggle()` and on observed SET_CONFIGURATION / CLEAR_FEATURE(HALT) /
  SET_ADDRESS control transfers (`usb_device::set_interface` calls `reset_data_toggle` itself).
  A second bulk/interrupt transfer on an endpoint that already has one in flight is refused with `busy`;
  a cancelled transfer does not update the toggle.
* **Safe unlinking.** `cancel()` (and completion) unlinks the QH, then waits for the async-advance doorbell
  (IAAD → USBSTS.IAA) — or one FRINDEX frame for the periodic list — before freeing the memory. `submit()`
  fails with `try_again` once `MaxTransfers` are in flight even while earlier QHs are still draining.
* **Errors.** Halted without error bits → `stall`; XactErr / MissedMicroFrame / DataBufferError → `bus_error`;
  Babble → `babble`; Host System Error marks the controller failed and fails everything. Unplugging leaves
  transfers pending until the controller errors them out (`bus_error`) or the stack cancels them.

## Companion controllers (full/low speed)

EHCI only talks high speed. With `CONFIGFLAG=1` all ports belong to EHCI, so the driver hands other devices to the
companion controller (pair with `ohci_hcd` / a UHCI driver, whose ports are the same physical connectors):

* `port_status()`: a device in low-speed K state gets `PORT_OWNER` set at once and is reported as not connected.
* `reset_port()`: if the port is not enabled after the reset the device is full speed; `PORT_OWNER` is set and the
  call fails with `unsupported_operation`. The companion sees the connection and enumerates the device itself.
  A disconnect returns the port to EHCI.

## Limits

* Root-port devices only: no transaction translator support (no split transactions), so full/low-speed devices behind
  a hub need the companion controller or a hub TT driver (out of scope).
* No isochronous transfers; `bInterval` ignored for interrupt endpoints; no frame-list resizing.
* BIOS/OS ownership handoff via the legacy support capability lives in PCI config space (the `Env` only has MMIO),
  so it is **not** handled; do it before `start()` (`eecp()` returns the capability offset).
* All DMA memory must be below 4 GiB.
