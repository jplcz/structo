/* SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Flat, non-relocatable image loaded by Multiboot2 at the conventional
 * 1 MiB physical/virtual address (paging is disabled at entry, so
 * physical == virtual here). `.multiboot` is placed first -- and,
 * combined with `-Ttext`/this being the very first output section, well
 * within the mandatory first-32-KiB search window -- so the bootloader
 * finds `multiboot_header.cpp`'s embedded header before loading
 * anything else.
 */

ENTRY(_start)

SECTIONS
{
  . = 1M;

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
