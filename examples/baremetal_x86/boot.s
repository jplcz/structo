/* SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The real entry point. Multiboot2 hands control over in 32-bit
 * protected mode with paging disabled, a flat (base 0, limit 4 GiB)
 * segment setup, interrupts disabled, and `eax`/`ebx` holding the
 * bootloader magic / boot-information structure's physical address --
 * exactly the two values `kmain` needs, passed through as plain
 * arguments (cdecl: pushed right-to-left).
 */

.section .bss
.align 16
stack_bottom:
.skip 16384 /* 16 KiB: a demo kernel needs nothing larger. */
stack_top:

.section .text
.global _start
.type _start, @function
_start:
  cli
  mov $stack_top, %esp
  mov %esp, %ebp

  /* kmain(std::uint32_t magic, std::uint32_t info_phys_addr) */
  push %ebx
  push %eax
  call kmain

  /* kmain is documented to never return (it ends in its own halt
   * loop), but stop here defensively if it ever does. */
  cli
.Lhang:
  hlt
  jmp .Lhang
.size _start, . - _start
