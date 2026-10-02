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
 *
 * `boot.s` also calls `crt0.cpp`'s `structo_run_global_constructors()`
 * (running every translation unit's `.init_array` entries) before
 * `kmain` itself, and `monotonic_allocator.cpp` provides a monotonic
 * (bump-pointer) allocator backing both global `operator new` (used by
 * `g_boot_proof` below, proof that it's already up before any global
 * constructor runs) and `reloco::default_allocator()` -- exercised here
 * by `kmain`'s own `reloco::vector<std::uint64_t>`, collecting every
 * "available" memory-map region's length as it's walked.
 */

#include "monotonic_allocator.hpp"
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

#include <reloco/iterator.hpp>
#include <reloco/vector.hpp>

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

/**
 * @brief Rust-style, `reloco::iterator_adaptor`-derived walk over a legacy
 * Multiboot 1 `mmap_addr`/`mmap_length` BIOS memory map, mirroring
 * `structo::arch::x86::multiboot2_mmap_iterator`'s shape (see
 * `multiboot2.hpp`) so both bootloader protocols are consumed through the
 * same `.for_each()`-style call, not a hand-rolled byte-offset `for` loop.
 */
class multiboot1_mmap_iterator : public reloco::iterator_adaptor<multiboot1_mmap_iterator, multiboot1_mmap_entry> {
public:
  using item_type = multiboot1_mmap_entry;

  multiboot1_mmap_iterator(std::uintptr_t mmap_addr, std::uint32_t mmap_length) noexcept
      : addr_(mmap_addr), length_(mmap_length) {}

  [[nodiscard]] reloco::optional<item_type> next_impl() noexcept {
    if (offset_ >= length_)
      return reloco::nullopt;
    const auto *entry = reinterpret_cast<const multiboot1_mmap_entry *>(addr_ + offset_);
    // Each entry's actual on-disk size is `size + 4` (the `size` field
    // itself isn't counted), per spec.
    offset_ += entry->size + 4;
    return reloco::optional<item_type>(*entry);
  }

private:
  std::uintptr_t addr_;
  std::uint32_t length_;
  std::uint32_t offset_ = 0;
};

void dump_multiboot1_memory_map(reloco::sink out, std::uintptr_t info_phys_addr,
                                reloco::vector<std::uint64_t> &available_lengths) noexcept {
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
  multiboot1_mmap_iterator(static_cast<std::uintptr_t>(mmap_addr), mmap_length)
      .for_each([&](const multiboot1_mmap_entry &entry) {
        bool available = entry.type == multiboot1_mmap_type_available;
        // Hex only, deliberately: decimal formatting of a `uint64_t` on
        // 32-bit x86 needs a software 64-bit divide (`__udivdi3`), which
        // this freestanding, `-nostdlib` build has no libgcc to supply.
        (void)microfmt::format_to(out, "  base={:#018x} length={:#018x} {}\n", entry.base_addr, entry.length,
                                  available ? "available" : "reserved");
        if (available)
          (void)available_lengths.try_push_back(entry.length);
      });
}

// Exists purely to prove, at runtime, that `.init_array` global
// constructors really do run before `kmain` (see `crt0.cpp`), and that
// `operator new`/the monotonic allocator (`monotonic_allocator.cpp`)
// are already usable from inside one. `marker` is heap-allocated
// (rather than just a plain data member) specifically so this exercises
// `new`, not merely construction order.
struct boot_proof {
  unsigned *marker;

  boot_proof() noexcept : marker(new unsigned(0xC0FFEEu)) {}
};
boot_proof g_boot_proof;

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
  (void)microfmt::format_to(out, "Global constructors + monotonic allocator OK (marker={:#x}).\n",
                            *g_boot_proof.marker);

  // `reloco::vector<std::uint64_t>::try_create()` allocates through
  // `reloco::default_allocator()` -- `monotonic_allocator.cpp`'s override
  // -- collecting every "available" memory-map region's length below as
  // proof that a real allocator-backed `reloco` container works here,
  // not just raw `operator new` (see `g_boot_proof` above).
  auto available_lengths_result = reloco::vector<std::uint64_t>::try_create();
  RELOCO_ASSERT(available_lengths_result.has_value(), "vector<uint64_t>::try_create");
  auto &available_lengths = *available_lengths_result;

  if (magic == structo::arch::x86::multiboot2_bootloader_magic) {
    (void)microfmt::format_to(out, "Multiboot2 magic OK.\n");

    auto info_span = reloco::span<const std::byte>(
        reinterpret_cast<const std::byte *>(static_cast<std::uintptr_t>(info_phys_addr)), max_boot_info_size);
    auto reader = structo::arch::x86::multiboot2_boot_info_reader::try_create(info_span);
    if (!reader) {
      (void)microfmt::format_to(out, "FATAL: failed to parse Multiboot2 boot info\n");
    } else {
      (void)microfmt::format_to(out, "Firmware memory map:\n");
      // `*reader` already derives from `reloco::iterator_adaptor` (see
      // `multiboot2.hpp`), so the "find every `memory_map` tag, ignoring
      // any trailing parse error" walk is a `.filter().for_each()` chain
      // rather than a manual range-for with an explicit `break`/`continue`
      // -- a malformed tag still surfaces as a filtered-out `reloco::error`
      // item exactly once, and the adaptor is then permanently exhausted,
      // so `for_each()` drains cleanly without needing an early-break.
      reader
          ->filter([](const auto &tag_r) {
            return tag_r.has_value() && tag_r->type == structo::arch::x86::multiboot2_tag_type::memory_map;
          })
          .for_each([&](const auto &tag_r) {
            auto entries_r = structo::arch::x86::multiboot2_mmap_entries(*tag_r);
            if (!entries_r)
              return;
            entries_r->for_each([&](const auto &entry_r) {
              if (!entry_r)
                return;
              const auto &entry = *entry_r;
              // Hex only, deliberately: decimal formatting of a
              // `uint64_t` on 32-bit x86 needs a software 64-bit divide
              // (`__udivdi3`), which this freestanding, `-nostdlib`
              // build has no libgcc to supply.
              (void)microfmt::format_to(out, "  base={:#018x} length={:#018x} {}\n", entry.base_addr, entry.length,
                                        entry.is_available() ? "available" : "reserved");
              if (entry.is_available())
                (void)available_lengths.try_push_back(entry.length);
            });
          });
    }
  } else if (magic == multiboot1_bootloader_magic) {
    // The legacy path: e.g. `qemu-system-i386 -kernel ...` directly (see
    // `multiboot1_header.cpp`/README.md) -- QEMU's own built-in loader
    // never implemented Multiboot 2.
    (void)microfmt::format_to(out, "Multiboot1 magic OK.\n");
    dump_multiboot1_memory_map(out, static_cast<std::uintptr_t>(info_phys_addr), available_lengths);
  } else {
    (void)microfmt::format_to(out, "FATAL: bad bootloader magic {:#x} (expected {:#x} or {:#x})\n", magic,
                              structo::arch::x86::multiboot2_bootloader_magic, multiboot1_bootloader_magic);
    (void)microfmt::format_to(out, "FATAL: not loaded via Multiboot 1 or 2 (bad magic in eax)\n");
    for (;;) {
      asm volatile("cli; hlt");
    }
  }

  // Rust `Iterator::sum()`, via the vector's own `.iter()` convenience
  // (equivalent to `reloco::iter(available_lengths)`) -- a left-fold over
  // `operator+` draining a fresh borrowing iterator, not a manual indexed
  // `for` loop.
  std::uint64_t total_available = available_lengths.iter().sum();
  (void)microfmt::format_to(out,
                            "reloco::vector<uint64_t> (default_allocator-backed): {} available region(s), "
                            "total={:#018x} bytes.\n",
                            available_lengths.size(), total_available);

  (void)microfmt::format_to(out, "Demo complete -- halting.\n");

  for (;;) {
    asm volatile("cli; hlt");
  }
}
