<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# Guide: loading a kernel and optional images via TFTP, then continuing boot

C++20 only. The bootloader brings up `bootldr::netstack` (see
[`netstack_bootloader.md`](netstack_bootloader.md)), runs a *boot task* on the
same `bootldr::scheduler` that downloads the kernel (required) and a device
tree / initrd (optional) with `bootldr::tftp_client`, then stops the netstack
and carries on with the normal boot path (verify, jump).

```
main():  start netstack ──► spawn boot task ──► run scheduler until task done
                                                     │
         boot task:  get kernel  ──► get dtb (optional) ──► get initrd (optional)
                                                     │
main():  net.stop() ◄── join result ◄────────────────┘ ──► verify + jump
```

The netstack and the boot task are cooperative tasks of one scheduler: no
threads or locks, and the link is only serviced while `run_once()` runs.

## 1. Memory layout and result type

```cpp
// Where each image lands. In a real bootloader these are fixed RAM addresses
// (span over a memory-mapped region); static arrays keep the example short.
alignas(16) static std::uint8_t kernel_ram[8 * 1024 * 1024]; // required
alignas(16) static std::uint8_t dtb_ram[256 * 1024];         // optional
alignas(16) static std::uint8_t initrd_ram[16 * 1024 * 1024]; // optional

struct boot_images {
  std::size_t kernel_size = 0; // bytes received, 0 = missing
  std::size_t dtb_size = 0;
  std::size_t initrd_size = 0;
  bool done = false;           // set by the boot task, polled by main()
  reloco::error err{};         // why the required image failed (valid if kernel_size == 0)
};
```

## 2. The boot task

```cpp
// A coroutine that runs as a scheduler task next to the netstack tasks.
// `tftp` is bound to the netstack; `server` is the TFTP server's IPv4 address.
reloco::task<void> load_images(bootldr::tftp_client &tftp, structo::net::ipv4_address server,
                               boot_images &out) {
  // get(server, filename, buffer) downloads into memory; co_await on the
  // returned task yields result<size_t>: bytes received or an error
  // (not_found, timed_out after max_retries, out_of_range if the file does
  // not fit the buffer, ...).
  auto k = co_await tftp.get(server, "kernel.bin", reloco::span<std::uint8_t>(kernel_ram));
  if (!k) { // the kernel is required: report and stop
    out.err = k.error();
    out.done = true;
    co_return;
  }
  out.kernel_size = *k;

  // Optional images: failure (typically not_found) is not fatal, the boot
  // simply continues without them.
  if (auto d = co_await tftp.get(server, "board.dtb", reloco::span<std::uint8_t>(dtb_ram)))
    out.dtb_size = *d;
  if (auto r = co_await tftp.get(server, "initrd.img", reloco::span<std::uint8_t>(initrd_ram)))
    out.initrd_size = *r;

  out.done = true; // main() sees this, then stops the netstack
}
```

Images are fetched one after another: each `get` binds its own ephemeral UDP
socket and closes it on exit, so nothing is left bound afterwards. For images
larger than RAM-resident buffers (e.g. flashing), use the callback overload
`get(server, name, sink_fn, ctx)`; the sink is called once per 512-byte block.

## 3. Bringing it together in `main()`

```cpp
int main() {
  // ... UART, slip_device, polled_net_device `pnd`, `nic`, scheduler `sched`
  //     with set_clock(), and `cfg` exactly as in netstack_bootloader.md ...
  bootldr::netstack<1006> net{sched, nic, cfg}; // 1006 = link MTU, must match the host
  (void)net.poll_with(pnd);                    // pnd.poll() runs every scheduler round
  if (!net.start())                            // spawns the rx + timer tasks
    return boot_from_flash();                  // no network: fall back

  bootldr::tftp_options opt;
  opt.timeout_ms = 2000; // retransmit after 2 s of silence
  opt.max_retries = 5;   // then fail with timed_out
  bootldr::tftp_client tftp{net, opt};

  boot_images img;
  const structo::net::ipv4_address server{192, 168, 7, 1};
  (void)sched.spawn(load_images(tftp, server, img));

  // Wait for an address, then for the transfers. A boot timeout bounds the
  // whole network phase so an absent server cannot hang the boot.
  const std::uint64_t deadline = sched.now_ms() + 30'000;
  while (!img.done && sched.now_ms() < deadline)
    sched.run_once(); // services the UART, netstack and the boot task

  // Stop the netstack: cancels its rx/timer tasks, so nothing touches the
  // UART or the IP stack any more. Do this before using the UART directly.
  net.stop();
  // If the loop ended on the deadline the boot task may still be suspended;
  // run once more so its socket is closed cleanly, or cancel it by id.
  sched.run_once();

  if (img.kernel_size == 0)
    return boot_from_flash(); // network boot failed (img.err says why)

  // The scheduler is idle now: continue the normal boot path.
  if (!verify_image(kernel_ram, img.kernel_size))
    return boot_from_flash();
  return jump_to_kernel(kernel_ram, dtb_ram, img.dtb_size, initrd_ram, img.initrd_size);
}
```

## 4. Notes

- **Last ACK:** a `get` returns after the final ACK has been sent, so stopping
  the netstack right after `done` does not truncate the last transfer.
- **Reusing the UART:** after `net.stop()` the `polled_net_device` poller is
  still registered but no task drives it; do not call `run_once()` afterwards
  if you hand the UART to something else, or discard the scheduler.
- **Selecting images:** build the filenames from the board id, MAC or DHCP
  options (e.g. `"<board>/kernel.bin"`) before spawning the task.
- **Host side:** any TFTP server reachable over the link works, e.g.
  `sudo dnsmasq --no-daemon --port=0 --enable-tftp --tftp-root=/srv/tftp
  --interface=sl0 --bind-interfaces`; for testing use the Linux demo setup in
  [`netstack_linux_demo.md`](netstack_linux_demo.md).
- **Security:** TFTP is unauthenticated and unencrypted; always verify the
  downloaded kernel (signature or hash) before jumping to it.
