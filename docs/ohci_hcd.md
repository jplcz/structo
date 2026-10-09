<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::ohci_hcd` — generic OHCI (USB 1.1) host controller

Header: `structo/hw/ohci_hcd.hpp` (C++20 with coroutines; empty otherwise). A header-only OpenHCI 1.0a driver. It knows
nothing about the SoC: registers, DMA memory and time come from a board-provided `Env`
(see `structo/hw/usb_hcd_env.hpp`), and the upper USB stack (`usb::usb_host`, `bootldr::usb_stack`) talks to it through
`usb_host_controller_ref`.

## The `Env` class

```cpp
#include <structo/hw/ohci_hcd.hpp>

struct my_usb_env {
  // Where the controller's register window starts (an OHCI controller is a PCI BAR or a SoC block).
  // Kept here only so read32/write32 can reach it.
  volatile std::uint32_t *mmio;

  // Reads one 32-bit controller register. `offset` is the BYTE offset from the start of the register window
  // (HcRevision is at 0, HcControl at 4, HcRhPortStatus[0] at 0x54 ...). The driver calls it for every register
  // access, so do the volatile access here and any bus fix-up (byte swapping on big-endian SoCs) your board needs.
  std::uint32_t read32(std::size_t offset) noexcept { return mmio[offset / 4]; }

  // Writes one 32-bit controller register; `offset` as above, `value` is what the controller must see.
  void write32(std::size_t offset, std::uint32_t value) noexcept { mmio[offset / 4] = value; }

  // Gives the driver `size` bytes of ZEROED memory the controller can DMA to and from (the HCCA, endpoint and
  // transfer descriptors, and per-transfer bounce buffers). `align` is a power of two (up to 4096): the driver asks
  // for 256 for the schedule block. The memory must be physically contiguous, coherent (uncached, or the platform keeps
  // it coherent: the driver does no cache maintenance) and, since OHCI has 32-bit pointers, below 4 GiB.
  // `phys` in the result is the address the CONTROLLER uses (the bus address). Return an empty buffer on failure.
  structo::hw::usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept {
    void *p = my_dma_pool.alloc(size, align);
    return {p, my_virt_to_bus(p), size};
  }

  // Gives memory obtained from dma_alloc() back; `buf` is exactly what dma_alloc returned.
  void dma_free(const structo::hw::usb_dma_buffer &buf) noexcept { my_dma_pool.free(buf.virt); }

  // Full memory barrier. The driver calls it after writing descriptors and before telling the controller to look at
  // them, and after reading the done-queue head before reading the descriptors it covers.
  void barrier() noexcept { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

  // Waits `ms` milliseconds without burning the CPU. reset_port() uses it for the >= 50 ms USB reset and the 10 ms
  // recovery time. Return a task that completes when the time has passed, e.g. `co_await sched.sleep_for(ms)`.
  reloco::task<void> delay_ms(unsigned ms) noexcept { co_await my_sched.sleep_for(ms); }
};
```

## Using the driver

```cpp
my_usb_env env;                                   // the board layer above
structo::hw::ohci_hcd<my_usb_env, 16> hcd{env};   // second argument: transfers in flight at the same time (default 16)

if (auto r = hcd.start(); !r)                     // takes the controller over from BIOS/SMM, resets it, builds the
  return;                                         // schedule, starts the frame counter and powers the root hub

structo::hw::usb_host_controller_ref ref{hcd};    // what usb::usb_host and bootldr::usb_stack take

// Wiring: call irq() from the controller's interrupt handler, or poll() periodically (e.g. from a scheduler poller)
// when there is no interrupt. Both do the same: acknowledge the controller, complete finished transfers, report
// root hub changes and recycle the descriptors of finished or cancelled transfers.
void ohci_interrupt() { hcd.irq(); }
```

`start()` fails with `unsupported_operation` (not an OpenHCI 1.0 controller), `allocation_failed`, `timed_out` (BIOS
hand-off or software reset never completed: every wait is a bounded spin) or `io_error`. `stop()` (also run by the
destructor) halts the controller, completes pending transfers with `usb_status::cancelled` and frees the DMA memory.

Optionally `hcd.set_port_event_hook(fn, ctx)` registers a function that `irq()` calls when the root hub reports a
change (a faster hot-plug reaction than the periodic `port_status()` poll).

## With `bootldr::usb_stack`

```cpp
bootldr::scheduler sched;
bootldr::usb_stack stack{sched, ref, {}};          // the hot-plug manager: polls port_status(), reset_port(), enumerates
stack.add_driver({&match, &driver_task, nullptr, nullptr});
sched.add_poller([](void *c) noexcept { static_cast<decltype(hcd) *>(c)->poll(); }, &hcd); // no IRQ: poll each round
stack.start();
sched.run();
```

`usb_stack` cancels the transfers of an unplugged device. Without it, a transfer to a device that disappeared completes
with `usb_status::disconnected` (the controller reports DeviceNotResponding while the root port is no longer
connected and enabled).

## How it works, and the limits

- Root hub ports only (`HcRhDescriptorA.NDP`, at most 15); no external-hub transaction translator work is needed for
  USB 1.1 hubs. Control, bulk and interrupt transfers; **no isochronous** (`unsupported_operation`).
- One endpoint descriptor per in-flight transfer. Interrupt transfers go on a 63-node periodic tree, polled every
  `interval` ms (rounded down to a power of two, at most 32); they are one-shot like bulk transfers.
- Data goes through a per-transfer DMA bounce buffer in TDs of at most 4 KiB (a multiple of the max packet size).
  Bulk/interrupt transfers use up to 32 TDs (128 KiB with 64-byte packets), control data stages are limited to 4 KiB
  (`out_of_range` otherwise). IN transfers end on a short packet; a zero-length packet is sent for OUT only when the
  length is 0.
- Data toggles are kept in software per (address, endpoint, direction) and written into every TD. They restart at
  DATA0 after SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE (all endpoints of the device),
  CLEAR_FEATURE(ENDPOINT_HALT) (that endpoint) and `reset_data_toggle()`.
- At most one transfer per (address, endpoint, direction) at a time (`error::busy`); `error::try_again` when
  `MaxTransfers` are in flight.
- `cancel()` never frees at once: the controller may still be reading the descriptors, so they are unlinked one
  start-of-frame later and recycled one more frame after that. Keep calling `irq()`/`poll()` (the SOF interrupt is only
  enabled while such removals are pending).
- Errors map to `usb_status`: STALL → `stall`, DeviceNotResponding → `disconnected` (port down) or `timeout`,
  Data/BufferOverrun → `babble`, CRC/bit stuffing/PID/others → `bus_error`. An unrecoverable controller error fails all
  transfers with `bus_error`.
- `reset_port()` waits for the root hub power-good time before the first reset, then resets (≥ 50 ms in 10 ms steps),
  makes sure the port is enabled and waits the 10 ms recovery time.
- No NAK timeout: a NAKed transfer stays pending until data arrives or it is cancelled.

The simulator test (`tests/test_ohci_hcd.cpp`) contains a register-level OHCI model that walks the driver's HCCA/ED/TD
structures and drives the shared packet-level USB device from `tests/usb_hcd_sim.hpp`.
