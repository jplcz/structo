// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

/** @file kmain.cpp
 * @brief The actual demo, called from `boot.s`'s `_start` with the raw
 * `eax`/`ebx` values the Multiboot2-compliant bootloader left behind:
 * validates the bootloader magic, binds a VGA text console (plus its
 * real CRTC hardware cursor) and a COM1 serial port, prints a banner to
 * both, then walks the boot information structure via
 * `multiboot2_boot_info_reader` and dumps the firmware memory map.
 *
 * The serial mirror exists so the demo is verifiable headlessly (e.g.
 * `qemu-system-i386 -kernel ... -display none -serial stdio`), not just
 * by eye against the VGA text buffer.
 */

#include "panic.hpp"

#include <structo/arch/x86/multiboot2.hpp>
#include <structo/arch/x86/port_io_space.hpp>
#include <structo/arch/x86/uart_ns16550.hpp>
#include <structo/arch/x86/vga_crtc.hpp>
#include <structo/hw/console_ref.hpp>
#include <structo/hw/uart_ref.hpp>
#include <structo/hw/vga_text_console.hpp>

#include <microfmt/microfmt.hpp>

#include <cstddef>
#include <cstdint>

namespace {

constexpr std::size_t vga_columns = 80;
constexpr std::size_t vga_rows = 25;
constexpr std::uintptr_t vga_phys_base = 0xB8000;

// The boot information structure's real size is only known once its own
// `total_size` field (the first 4 bytes) has been read -- but that read
// itself has to happen through a span of *some* assumed upper bound.
// 64 KiB is far beyond any boot information structure this basic driver
// has ever been tested against (the synthetic blobs in
// `tests/test_multiboot2.cpp` are a few hundred bytes), and -- with
// paging disabled and the kernel itself loaded at the conventional
// 1 MiB mark -- comfortably within identity-mapped, bootloader-owned
// low memory, so reading (not writing) this far past the structure's
// real end is harmless even if never dereferenced beyond what
// `multiboot2_boot_info_reader::try_create`'s own bounds-checked
// `total_size` validation actually allows iteration to reach.
constexpr std::size_t max_boot_info_size = 64 * 1024;

void write_both(structo::hw::console_ref &console, microfmt::sink serial, reloco::string_view text) noexcept {
  console.write(text);
  (void)microfmt::format_to(serial, "{}", text);
}

} // namespace

extern "C" [[noreturn]] void kmain(std::uint32_t magic, std::uint32_t info_phys_addr) noexcept {
  // --- Port I/O backend, shared by the VGA cursor driver and the serial UART ---
  structo::arch::x86::port_io_backend io_backend{};
  structo::io_space_ref<structo::port_io_space> ports(io_backend);

  // --- VGA text console, with a real hardware cursor ---
  structo::arch::x86::vga_crtc crtc(ports);
  (void)crtc.enable_cursor(14, 15); // Thin underline cursor.

  auto cursor_sink = structo::hw::vga_text_console::cursor_sink(
      [&crtc](std::size_t x, std::size_t y) { (void)crtc.move_cursor(x, y, vga_columns); });
  auto vga_span = reloco::span<std::byte>(reinterpret_cast<std::byte *>(vga_phys_base), vga_columns * vga_rows * 2);
  auto console_backend = structo::hw::vga_text_console::try_create(vga_span, vga_columns, vga_rows, cursor_sink);
  // columns/rows/buffer size are all compile-time constants sized to
  // match each other -- failure here would be this file's own bug, not
  // an environment condition, hence RELOCO_ASSERT (-> baremetal_panic)
  // rather than a graceful degraded path.
  RELOCO_ASSERT(console_backend.has_value(), "vga_text_console::try_create");
  structo::hw::console_ref console(*console_backend);
  console.clear(structo::hw::console_color::light_gray, structo::hw::console_color::blue);
  console.set_colors(structo::hw::console_color::light_gray, structo::hw::console_color::blue);

  // --- COM1 serial, mirroring everything written to the VGA console ---
  structo::arch::x86::ns16550_uart uart_chip(ports, structo::arch::x86::ns16550_uart::com1_base);
  structo::hw::uart_ref uart(uart_chip);
  (void)uart.configure(structo::hw::uart_config_115200_8n1);
  auto uart_write = [&uart](microfmt::string_view sv) noexcept {
    for (char c : sv) {
      if (c == '\n')
        (void)uart.put_byte(static_cast<std::uint8_t>('\r'));
      (void)uart.put_byte(static_cast<std::uint8_t>(c));
    }
  };
  auto serial = microfmt::make_callback_sink(uart_write);

  write_both(console, serial.as_sink(), "structo bare-metal x86 Multiboot2 demo\n");
  write_both(console, serial.as_sink(), "================================================\n");

  if (magic != structo::arch::x86::multiboot2_bootloader_magic) {
    (void)microfmt::format_to(serial.as_sink(), "FATAL: bad Multiboot2 magic {:#x} (expected {:#x})\n", magic,
                             structo::arch::x86::multiboot2_bootloader_magic);
    write_both(console, serial.as_sink(), "FATAL: not loaded via Multiboot2 (bad magic in eax)\n");
    for (;;) {
      asm volatile("cli; hlt");
    }
  }
  write_both(console, serial.as_sink(), "Multiboot2 magic OK.\n");

  auto info_span = reloco::span<const std::byte>(
      reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(info_phys_addr)), max_boot_info_size);
  auto reader = structo::arch::x86::multiboot2_boot_info_reader::try_create(info_span);
  if (!reader) {
    write_both(console, serial.as_sink(), "FATAL: failed to parse Multiboot2 boot info\n");
  } else {
    write_both(console, serial.as_sink(), "Firmware memory map:\n");
    for (auto tag_r : *reader) {
      if (!tag_r)
        break;
      const auto &tag = *tag_r;
      if (tag.type != structo::arch::x86::multiboot2_tag_type::memory_map)
        continue;

      auto entries_r = structo::arch::x86::multiboot2_mmap_entries(tag);
      if (!entries_r)
        continue;
      for (auto entry_r : *entries_r) {
        if (!entry_r)
          break;
        const auto &entry = *entry_r;
        // Hex only, deliberately: decimal formatting of a `uint64_t` on
        // 32-bit x86 needs a software 64-bit divide (`__udivdi3`), which
        // this freestanding, `-nostdlib` build has no libgcc to supply.
        (void)microfmt::format_to(serial.as_sink(), "  base={:#018x} length={:#018x} {}\n", entry.base_addr,
                                 entry.length, entry.is_available() ? "available" : "reserved");
        console.write(entry.is_available() ? "  [free]     " : "  [reserved] ");
        console.write(entry.is_available() ? "available region\n" : "reserved region\n");
      }
    }
  }

  write_both(console, serial.as_sink(), "Demo complete -- halting.\n");

  for (;;) {
    asm volatile("cli; hlt");
  }
}
