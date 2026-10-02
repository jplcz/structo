// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

/** @file kmain.cpp
 * @brief The actual demo, called from `boot.s`'s `_start` with the raw
 * `eax`/`ebx` values the bootloader left behind: validates the magic,
 * binds a VGA text console (plus its real CRTC hardware cursor) and a
 * COM1 serial port, fans both out through a single `microfmt::tee_sink`
 * (see `out` in `kmain`, built from `console_ref::as_sink()` plus the
 * serial UART's callback sink), prints a banner through it, then walks
 * the boot information structure and dumps the firmware memory map --
 * one `microfmt::format_to(out, ...)` call per line reaching both the
 * screen and the serial port identically, rather than hand-duplicating
 * each line's text/formatting per destination.
 *
 * Two bootloader protocols are accepted (see `multiboot1_header.cpp`/
 * `multiboot_header.cpp` for why both headers are embedded): Multiboot 2
 * (`eax == multiboot2_bootloader_magic`), parsed via `structo`'s own
 * `multiboot2_boot_info_reader`; and legacy Multiboot 1
 * (`eax == multiboot1_bootloader_magic`), parsed by this file's own
 * small, local `multiboot1_info_view` helper -- `structo`'s library is
 * deliberately Multiboot2-only (see `multiboot1_header.cpp`), so there is
 * no reusable reader to call into for that path.
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
#include <microfmt/sinks/tee_sink.hpp>

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

// The bootloader magic QEMU's (and any other Multiboot-*1*-only, e.g.
// legacy GRUB) `eax` holds at entry -- distinct from both
// `multiboot2_bootloader_magic` and the *header* magic
// `multiboot1_header.cpp` embeds (`0x1BADB002`): this is the value the
// *bootloader* hands back, per spec.
constexpr std::uint32_t multiboot1_bootloader_magic = 0x2BADB002;

// `structo`'s library deliberately has no Multiboot 1 reader (see
// `multiboot1_header.cpp`) -- only the handful of fields this demo
// actually prints are modeled here, by raw offset, matching the classic
// `multiboot_info`/`multiboot_mmap_entry` layouts byte-for-byte.
struct multiboot1_mmap_entry {
  std::uint32_t size; // Size of the *rest* of this entry, not including this field itself.
  std::uint64_t base_addr;
  std::uint64_t length;
  std::uint32_t type;
};
static_assert(sizeof(multiboot1_mmap_entry) == 24);

constexpr std::uint32_t multiboot1_info_flag_mem_map = 1u << 6;
constexpr std::uint32_t multiboot1_mmap_type_available = 1;

void dump_multiboot1_memory_map(reloco::sink out, std::uintptr_t info_phys_addr) noexcept {
  const auto *info_bytes = reinterpret_cast<const std::uint8_t *>(info_phys_addr);
  std::uint32_t flags{};
  __builtin_memcpy(&flags, info_bytes + 0, sizeof(flags));
  if ((flags & multiboot1_info_flag_mem_map) == 0) {
    (void)microfmt::format_to(out, "FATAL: bootloader provided no BIOS memory map (info flags bit 6 unset)\n");
    return;
  }

  std::uint32_t mmap_length{};
  std::uint32_t mmap_addr{};
  __builtin_memcpy(&mmap_length, info_bytes + 44, sizeof(mmap_length));
  __builtin_memcpy(&mmap_addr, info_bytes + 48, sizeof(mmap_addr));

  (void)microfmt::format_to(out, "Firmware memory map:\n");
  for (std::uint32_t offset = 0; offset < mmap_length;) {
    const auto *entry =
        reinterpret_cast<const multiboot1_mmap_entry *>(static_cast<std::uintptr_t>(mmap_addr) + offset);
    bool available = entry->type == multiboot1_mmap_type_available;
    // Hex only, deliberately: decimal formatting of a `uint64_t` on
    // 32-bit x86 needs a software 64-bit divide (`__udivdi3`), which
    // this freestanding, `-nostdlib` build has no libgcc to supply.
    (void)microfmt::format_to(out, "  base={:#018x} length={:#018x} {}\n", entry->base_addr, entry->length,
                              available ? "available" : "reserved");

    // Each entry's actual on-disk size is `size + 4` (the `size` field
    // itself isn't counted), per spec.
    offset += entry->size + 4;
  }
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

  // --- Fan out everything written through `out` to both the VGA text
  // console (literally, via `console_ref::as_sink()`) and COM1 ---
  microfmt::tee_sink<2> tee(console.as_sink(), serial.as_sink());
  auto out = tee.as_sink();

  (void)microfmt::format_to(out, "structo bare-metal x86 Multiboot demo\n");
  (void)microfmt::format_to(out, "================================================\n");

  if (magic == structo::arch::x86::multiboot2_bootloader_magic) {
    (void)microfmt::format_to(out, "Multiboot2 magic OK.\n");

    auto info_span = reloco::span<const std::byte>(
        reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(info_phys_addr)), max_boot_info_size);
    auto reader = structo::arch::x86::multiboot2_boot_info_reader::try_create(info_span);
    if (!reader) {
      (void)microfmt::format_to(out, "FATAL: failed to parse Multiboot2 boot info\n");
    } else {
      (void)microfmt::format_to(out, "Firmware memory map:\n");
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
          (void)microfmt::format_to(out, "  base={:#018x} length={:#018x} {}\n", entry.base_addr, entry.length,
                                    entry.is_available() ? "available" : "reserved");
        }
      }
    }
  } else if (magic == multiboot1_bootloader_magic) {
    // The legacy path: e.g. `qemu-system-i386 -kernel ...` directly (see
    // `multiboot1_header.cpp`/README.md) -- QEMU's own built-in loader
    // never implemented Multiboot 2.
    (void)microfmt::format_to(out, "Multiboot1 magic OK.\n");
    dump_multiboot1_memory_map(out, static_cast<std::uintptr_t>(info_phys_addr));
  } else {
    (void)microfmt::format_to(out, "FATAL: bad bootloader magic {:#x} (expected {:#x} or {:#x})\n", magic,
                              structo::arch::x86::multiboot2_bootloader_magic, multiboot1_bootloader_magic);
    (void)microfmt::format_to(out, "FATAL: not loaded via Multiboot 1 or 2 (bad magic in eax)\n");
    for (;;) {
      asm volatile("cli; hlt");
    }
  }

  (void)microfmt::format_to(out, "Demo complete -- halting.\n");

  for (;;) {
    asm volatile("cli; hlt");
  }
}
