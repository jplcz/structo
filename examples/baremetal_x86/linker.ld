/* SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Flat, non-relocatable image, bootable via *either* Multiboot 1 or
 * Multiboot 2, loaded at the conventional 1 MiB physical/virtual
 * address (paging is disabled at entry, so physical == virtual here).
 * `.multiboot1` and `.multiboot` are placed first, in that order --
 * both comfortably within the mandatory first-8-KiB (Multiboot 1) /
 * first-32-KiB (Multiboot 2) search windows their respective loaders
 * require -- so either a legacy Multiboot 1 loader (e.g.
 * `qemu-system-i386 -kernel`, see README.md) or a real Multiboot 2
 * loader (GRUB2) finds its own header before loading anything else.
 */

ENTRY(_start)

SECTIONS
{
  . = 1M;

  .multiboot1 : ALIGN(4) {
    *(.multiboot1)
  }

  .multiboot : ALIGN(8) {
    *(.multiboot)
  }

  .text : ALIGN(4K) {
    *(.text*)
  }

  .rodata : ALIGN(4K) {
    *(.rodata*)
  }

  .data : ALIGN(4K) {
    *(.data*)
  }

  .bss : ALIGN(4K) {
    *(COMMON)
    *(.bss*)
  }

  /DISCARD/ : {
    *(.comment)
    *(.eh_frame*)
    *(.note*)
  }
}
