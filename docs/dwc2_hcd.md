<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::dwc2_hcd` — generic DWC2 (DesignWare USB 2.0 OTG) host controller

Header: `structo/hw/dwc2_hcd.hpp` (C++20 with coroutines; empty otherwise). A header-only driver for the Synopsys
DesignWare Hi-Speed USB 2.0 On-The-Go core (known as DWC2 / `dwc_otg`; Raspberry Pi, STM32 OTG_HS, Allwinner,
Rockchip, Amlogic, Ingenic ... ) in **host mode with Internal DMA**. It knows nothing about the SoC: registers, DMA
memory and time come from a board-provided `Env` (see `structo/hw/usb_hcd_env.hpp`), the board-specific parts of the
core (PHY, FIFO sizes, AHB burst) come from a small `dwc2_config`, and the upper USB stack (`usb::usb_host`,
`bootldr::usb_stack`) talks to it through `usb_host_controller_ref`.

## The `Env` class

```cpp
#include <structo/hw/dwc2_hcd.hpp>

struct my_usb_env {
  // Where the core's register window starts (the DWC2 is a SoC block). Kept here only so read32/write32 can reach it.
  volatile std::uint32_t *mmio;

  // Reads one 32-bit core register. `offset` is the BYTE offset from the start of the register window
  // (GAHBCFG is at 0x008, GINTSTS at 0x014, HPRT at 0x440, host channel n at 0x500 + 0x20 * n ...). The driver calls
  // it for every register access, so do the volatile access here and any bus fix-up your board needs.
  std::uint32_t read32(std::size_t offset) noexcept { return mmio[offset / 4]; }

  // Writes one 32-bit core register; `offset` as above, `value` is what the core must see.
  void write32(std::size_t offset, std::uint32_t value) noexcept { mmio[offset / 4] = value; }

  // Gives the driver `size` bytes of ZEROED memory the core can DMA to and from: one bounce block per transfer
  // (setup packet, zero-length area and the data). `align` is a power of two; the driver asks for 64 (it also keeps
  // HCDMA 4-byte aligned). The memory must be physically contiguous, coherent (uncached, or the platform keeps it
  // coherent: the driver does no cache maintenance) and, since HCDMA is a 32-bit register, below 4 GiB.
  // `phys` in the result is the address the CORE uses (the AHB master's view). Return an empty buffer on failure.
  structo::hw::usb_dma_buffer dma_alloc(std::size_t size, std::size_t align) noexcept {
    void *p = my_dma_pool.alloc(size, align);
    return {p, my_virt_to_bus(p), size};
  }

  // Gives memory obtained from dma_alloc() back; `buf` is exactly what dma_alloc returned.
  void dma_free(const structo::hw::usb_dma_buffer &buf) noexcept { my_dma_pool.free(buf.virt); }

  // Full memory barrier. The driver calls it after writing the bounce buffer and the channel registers and before
  // setting HCCHAR.ChEna, which hands the channel to the core.
  void barrier() noexcept { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

  // Waits `ms` milliseconds without burning the CPU. reset_port() uses it for the power-good time, the >= 50 ms USB
  // reset and the 10 ms recovery time (nothing else sleeps). Return a task that completes when the time has passed,
  // e.g. `co_await sched.sleep_for(ms)`.
  reloco::task<void> delay_ms(unsigned ms) noexcept { co_await my_sched.sleep_for(ms); }
};
```

What the board must do **before** `start()`: enable the clocks, release the core and PHY resets, power the PHY and
switch VBUS on (the driver only sets the core's `PrtPwr`; an external VBUS switch is yours), and route the interrupt.

## Using the driver

```cpp
my_usb_env env;                                   // the board layer above

structo::hw::dwc2_config cfg;                     // board knobs of the core; every field has a typical default
cfg.phy = structo::hw::dwc2_phy::utmi_8bit;       // PHY interface: utmi_8bit, utmi_16bit, ulpi or fs_serial
                                                  // (dedicated full-speed transceiver, no high speed). Picks the
                                                  // GUSBCFG PHY bits, the turnaround time and HFIR/HCFG clocks.
cfg.rx_fifo_words = 512;                          // GRXFSIZ: the receive FIFO shared by all channels, in 32-bit words
cfg.np_tx_fifo_words = 256;                       // GNPTXFSIZ: non-periodic (control/bulk OUT) transmit FIFO
cfg.p_tx_fifo_words = 256;                        // HPTXFSIZ: periodic (interrupt OUT) transmit FIFO. The three
                                                  // sizes together must fit GHWCFG3.DfifoDepth, otherwise start()
                                                  // returns invalid_argument. Keep each >= the largest packet / 4.
cfg.ahb_burst = 0;                                // GAHBCFG.HBstLen: 0 single, 1 INCR, 3 INCR4, 5 INCR8, 7 INCR16;
                                                  // use what your bus fabric accepts
cfg.power_good_ms = 20;                           // wait after VBUS-on before the first port reset

structo::hw::dwc2_hcd<my_usb_env, 8> hcd{env, cfg}; // 2nd argument: transfers in flight at once (1..16, default 16);
                                                    // one host channel each, so min(MaxTransfers, hardware channels)

if (auto r = hcd.start(); !r)                     // resets the core, forces host mode, programs FIFOs, DMA and
  return;                                         // interrupts and powers the root port

structo::hw::usb_host_controller_ref ref{hcd};    // what usb::usb_host and bootldr::usb_stack take

// Wiring: call irq() from the core's interrupt handler, or poll() periodically (e.g. from a scheduler poller) when
// there is no interrupt. Both do the same: acknowledge the core, evaluate halted channels, complete finished
// transfers, retry NAKed ones in the next frame, report root port changes and recycle cancelled channels.
void dwc2_interrupt() { hcd.irq(); }
```

`start()` fails with `unsupported_operation` (`GSNPSID` is not a DWC2 core, or the core was built without Internal
DMA: slave/FIFO mode is not supported), `invalid_argument` (FIFO sizes do not fit), or `timed_out` (core reset, FIFO
flush or the switch to host mode never completed: every wait is a bounded spin). `stop()` (also run by the destructor)
masks the interrupts, halts all channels, soft-resets the core, completes pending transfers with
`usb_status::cancelled` and frees the DMA memory.

Optionally `hcd.set_port_event_hook(fn, ctx)` registers a function that `irq()` calls when the root port reports a
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

`usb_stack` cancels the transfers of an unplugged device. Without it, a transfer to a device that disappeared
completes with `usb_status::disconnected` (the core reports the disconnect and the root port is no longer connected
and enabled).

## How it works, and the limits

- One root port; control, bulk and interrupt transfers; full/low-speed devices directly on the port, high speed
  supported. **No isochronous** (`unsupported_operation`), no hubs and no split transactions (a device behind a
  high-speed hub is not reachable). Host mode only; Internal DMA only (`GAHBCFG.DMAEn`).
- One host channel per in-flight transfer, `min(GHWCFG2.NumHstChnl + 1, MaxTransfers)` in total. A transfer is a
  sequence of channel runs, each one `HCCHAR`/`HCTSIZ`/`HCDMA` programming followed by `ChEna`: control = SETUP
  (8 bytes, PID SETUP), data stage (starts at DATA1) and a zero-length DATA1 status stage in the opposite direction;
  bulk = chunks of up to 1023 packets (and 512 KiB); interrupt = one packet per run. Largest transfer: 256 KiB
  (`out_of_range` beyond).
- Data goes through a per-transfer DMA bounce block. IN transfers are programmed in whole packets, so a device that
  sends more than the caller's buffer completes with `usb_status::babble`. `HCDMA` must be 4-byte aligned: a
  transfer longer than one packet therefore needs a max packet size that is a multiple of 4 (`invalid_argument`
  otherwise; all standard sizes are).
- Data toggles are kept in software per (address, endpoint, direction) and written into `HCTSIZ.Pid` of every run.
  They restart at DATA0 after SET_ADDRESS, SET_CONFIGURATION, SET_INTERFACE (all endpoints of the device),
  CLEAR_FEATURE(ENDPOINT_HALT) (that endpoint) and `reset_data_toggle()`.
- NAK: a NAKed bulk/control run is re-armed one frame later (the SOF interrupt is unmasked only while such a retry
  is waiting). High-speed OUT endpoints are probed with PING (`HCTSIZ.DoPng`) after NAK/NYET, as the protocol
  requires. There is no NAK timeout: a NAKed transfer stays pending until data arrives or it is cancelled.
- Transaction errors (`XactErr`, frame overrun, data-toggle error) are retried; three in a row fail the transfer
  with `timeout` (`disconnected` when the root port went down meanwhile).
- **Interrupt endpoints, simplification:** `usb_pipe::interval` is ignored. An interrupt IN transfer is re-armed in
  every following frame (`HCCHAR.OddFrm` set to the parity of the next frame) until the device answers with data, so
  an endpoint is polled more often than its descriptor asks for. Transfers are one-shot like bulk transfers.
- At most one transfer per (address, endpoint, direction) at a time (`error::busy`); `error::try_again` when every
  channel is busy.
- `cancel()` disables the channel and waits (bounded) for it to halt. If it does not halt in time, the channel and
  its bounce block stay allocated (a "zombie") until `irq()`/`poll()` sees the channel halted, so the core never DMAs
  into freed memory; the endpoint reports `busy` until then. `stop()` soft-resets the core, which ends any DMA.
- Errors map to `usb_status` like the other drivers: STALL → `stall`, no response → `timeout` (or `disconnected` when
  the port is down), babble → `babble`, AHB error and everything else → `bus_error`. Losing host mode (the core
  falls back to device mode) fails all transfers with `bus_error` and every later `submit()` with `invalid_state`.
- `reset_port()` waits `power_good_ms` once, asserts `PrtRst` for 60 ms, waits for the core to enable the port, reads
  `PrtSpd`, programs `HCFG.FSLSPclkSel` and `HFIR` for the negotiated speed and waits the 10 ms recovery time. The
  HPRT register has write-1-to-clear bits (`PrtConnDet`, `PrtEnChng`, `PrtOvrCurrChng`) and a write-1-to-disable bit
  (`PrtEna`); every read-modify-write in the driver masks them, so a pending status change is never lost.
- The usb stack must give each pipe the device's real speed (`usb_pipe::speed`): it selects `HCCHAR.LSpdDev` and the
  PING protocol.

The simulator test (`tests/test_dwc2_hcd.cpp`) contains a register-level DWC2 model that executes a channel when
`HCCHAR.ChEna` is written (checking the programming: alignment, `HCTSIZ`, `OddFrm` parity, PING after NYET ...) and
drives the shared packet-level USB device from `tests/usb_hcd_sim.hpp`.
