<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `baremetal_x86_demo`

A minimal, 32-bit protected-mode x86 kernel image, bootable via **either**
Multiboot 1 or Multiboot 2, that prints a banner plus the firmware memory
map to both the VGA text console and a COM1 serial port. It exists to
exercise several of this repository's headers the way their intended
(bare-metal) caller actually would, end to end:
`structo::arch::x86::multiboot2_header`/`multiboot2_boot_info_reader`,
`structo::hw::vga_text_console` + `structo::hw::console_ref`,
`structo::arch::x86::vga_crtc`, and `structo::arch::x86::ns16550_uart` +
`structo::hw::uart_ref` -- instead of only against synthetic unit-test
fixtures or a hosted `main()`.

This is a **fully standalone CMake project**: it is not `add_subdirectory`'d
from (or otherwise wired into) the main `structo` build, has its own
`project()`, and cross-compiles with its own toolchain file. Configure and
build it entirely on its own, from this directory.

## Layout

| File | Role |
|---|---|
| `panic.hpp` / `panic.cpp` | Declares/defines `baremetal_panic`, wired up as `RELOCO_KERNEL_PANIC` (see below) -- writes directly to the VGA buffer and halts, since it must work even if the rest of the kernel's state is suspect. |
| `boot.s` | The real entry point (`_start`): sets up a stack, then calls `kmain(magic, info_phys_addr)` with the values the bootloader left in `eax`/`ebx`, regardless of which Multiboot protocol it used. |
| `multiboot1_header.cpp` | Embeds a hand-rolled, classic Multiboot 1 header (magic `0x1BADB002`) in a `.multiboot1` section, purely so `qemu-system-i386 -kernel` can load this image directly (see below). |
| `multiboot_header.cpp` | Embeds `structo::arch::x86::make_basic_header()`'s result in a dedicated `.multiboot` linker section so a real Multiboot2-aware bootloader (GRUB2) finds it within the first 32 KiB of the image. |
| `kmain.cpp` | The actual demo: validates whichever bootloader magic is present, binds VGA text console + CRTC cursor + COM1 `ns16550_uart` (mirroring all console output to serial), walks the boot information structure (via `multiboot2_boot_info_reader` for Multiboot 2, or a small local helper for Multiboot 1 -- see below), prints a banner and the memory map, then halts. |
| `linker.ld` | Places `.multiboot1` then `.multiboot` first, sets the entry point, and lays out the rest of a flat, non-relocatable image loaded at `1 MiB` (the conventional Multiboot load address). |
| `thirdparty/freebsd_libc/` | A handful of FreeBSD libc string routines (see below), vendored because this demo links `-nostdlib` yet still needs `strlen`/`memchr`/etc., which libstdc++'s own `std::char_traits<char>` implementation calls internally. |
| `toolchain-i686.cmake` | CMake toolchain file selecting the `i686-linux-gnu-{gcc,g++}` cross-compiler. |

## Why `RELOCO_KERNEL`

`reloco`'s `RELOCO_ASSERT`/`RELOCO_DEBUG_ASSERT` normally report failures
through a hosted, `std::fprintf(stderr, ...)`-based default handler --
unavailable here (no libc stdio, no OS). Defining `RELOCO_KERNEL`
(alongside `RELOCO_KERNEL_PANIC`, routed to `baremetal_panic`, see
`CMakeLists.txt`) switches every `reloco`/`structo` header this demo
includes onto the freestanding failure path instead, matching the exact
mechanism `docs/coding-guide.md`'s "No `std::` containers" section and
`reloco`'s own `reloco_config.hpp` document as the supported way to target
a kernel/freestanding build.

## Why this doesn't use `-ffreestanding`

`-ffreestanding` triggers libstdc++'s `bits/requires_hosted.h` guard on
`<string>` (pulled in transitively by `reloco::string_view`), which then
hard-errors out. Rather than fight that, this demo follows the same
convention `microfmt`'s own `examples/bare_metal/qemu-virt` uses: compile
in ordinary hosted-header mode (by discipline, nothing here actually
touches the hosted C++ runtime at run time) and link `-nostdlib -static`
separately, supplying any missing libc symbols explicitly (see below)
rather than declaring the whole translation unit freestanding.

## Why vendored FreeBSD libc string routines

Linking `-nostdlib` means there is no libc to resolve `strlen`/`memchr` --
both of which libstdc++'s own `std::char_traits<char>` calls internally,
and which this demo's code (via `reloco::string_view`/`console_ref`)
transitively depends on. `thirdparty/freebsd_libc/` vendors the handful of
FreeBSD `lib/libc/string/*.c` files providing them (plus `memcmp`/
`memset`/`memcpy`/`memmove`, included preemptively since they're common
transitive dependencies too), compiled with `-fno-builtin` so the compiler
can't "recognize" these as the very builtins they're defining and
miscompile them into infinite recursion. See
`thirdparty/freebsd_libc/README.md` for exact provenance and licensing.

## Why this image embeds *two* Multiboot headers

QEMU's own `-kernel` Multiboot loader (`hw/i386/multiboot.c`) only ever
implemented Multiboot **1** (magic `0x1BADB002`) -- it has no Multiboot
*2* support at all, at any QEMU version. Meanwhile `structo::arch::x86`'s
own Multiboot support is deliberately Multiboot2-only (a materially
richer, tag-based boot information format; see `multiboot2.hpp`), and
that's the protocol a real bootloader like GRUB2 is expected to use. To
get both "exercises the library's real Multiboot2 reader" *and* "boots
directly under `qemu-system-i386 -kernel`, no GRUB/ISO detour needed",
this image embeds **both** headers, back to back, both well within
Multiboot 1's mandatory first-8-KiB and Multiboot 2's first-32-KiB search
windows (see `linker.ld`): `multiboot1_header.cpp`'s minimal, hand-rolled
classic header (not modeled anywhere in the library -- it's legacy/
QEMU-loader-only at this point, so there's little reuse value in adding
it to `structo` itself), and `multiboot_header.cpp`'s real
`structo::arch::x86::multiboot2_header`. `kmain.cpp` checks whichever
magic `eax` actually holds at entry and parses the matching boot
information format -- `multiboot2_boot_info_reader` for Multiboot 2, or a
small local helper (`dump_multiboot1_memory_map`) walking the classic
`multiboot_info`/`multiboot_mmap_entry` layout by raw offset for
Multiboot 1.

A GRUB2 ISO booted under OVMF (UEFI) is still the only way to exercise
the Multiboot *2* path in this environment: this environment's installed
GRUB packages are EFI-only, so the ISO `grub-mkrescue` produces only has
a UEFI El Torito boot image, not a legacy-BIOS one (Multiboot2 always
hands control to the kernel in 32-bit protected mode regardless of
whether the bootloader itself is 64-bit UEFI GRUB, so this is standard
practice even for a 32-bit kernel).

## Building and running

Requires: an `i686-linux-gnu-gcc`/`i686-linux-gnu-g++` cross-compiler, and
(for either run target) `qemu-system-i386` or, for the GRUB/Multiboot2
path, `grub-mkrescue` + `qemu-system-x86_64` + an OVMF firmware image
(`/usr/share/qemu/OVMF.fd` or similar -- override with
`-DOVMF_FIRMWARE=/path/to/OVMF.fd` if not auto-found). Sibling checkouts
of `reloco` and `microfmt` next to this `structo` checkout are expected by
default; override `-DRELOCO_INCLUDE_DIR=`/`-DMICROFMT_INCLUDE_DIR=` if
yours live elsewhere.

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchain-i686.cmake
cmake --build build --target baremetal_x86_demo          # links baremetal_x86_demo.elf

# Multiboot 1: boots the ELF image directly, no GRUB/ISO needed.
cmake --build build --target run_baremetal_x86_demo_mb1   # qemu-system-i386 -kernel ...

# Multiboot 2: needs a real Multiboot2-aware bootloader (GRUB2).
cmake --build build --target baremetal_x86_demo_iso       # builds a bootable GRUB2 ISO
cmake --build build --target run_baremetal_x86_demo       # boots it under qemu-system-x86_64 -bios OVMF.fd
```

All output (the banner, the magic-number check, the full memory map, and
the completion message) is written to both the VGA text console and COM1.
`run_baremetal_x86_demo_mb1` passes `-display none -serial stdio`, so the
serial output goes straight to the terminal; `run_baremetal_x86_demo`
passes `-nographic` instead, so only the serial output (including GRUB's
own, harmless "no suitable video mode found" warning -- expected, since
the kernel drives VGA text mode directly rather than requesting a GRUB
video mode) is visible. Drop those flags from the corresponding custom
command in `CMakeLists.txt` for a graphical VGA window instead. The demo
halts the CPU (`hlt` loop) once done; `-no-reboot` keeps QEMU from
resetting on the triple fault this would otherwise eventually cause.
