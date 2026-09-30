// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Decodes a Flattened Device Tree blob's physical memory description into a
// structo::boot_memory_map, then prints every free region and the single
// largest one -- the two queries an early-boot page allocator bootstraps
// from before any heap exists.

#include <structo/boot_memory_map.hpp>
#include <structo/fdt_writer.hpp>

#include <cstdio>

using reloco::span;
using structo::fdt::fdt_writer;

namespace {

// A single 1 GiB RAM range at 0x40000000 with a 4 KiB reserved carveout at
// its base, standing in for firmware/bootloader-supplied DTB bytes.
reloco::result<reloco::span<const std::byte>> build_demo_blob(reloco::span<std::byte> storage) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return reloco::unexpected(made.error());
  fdt_writer w = std::move(made).value();

  if (auto r = w.begin_node(""); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.property_u32("#address-cells", 2); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.property_u32("#size-cells", 1); !r)
    return reloco::unexpected(r.error());

  if (auto r = w.begin_node("memory@40000000"); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.property_string("device_type", "memory"); !r)
    return reloco::unexpected(r.error());
  const uint32_t reg[3] = {0x0, 0x40000000, 0x40000000};
  if (auto r = w.property_u32_array("reg", span<const uint32_t>(reg, 3)); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return reloco::unexpected(r.error());

  if (auto r = w.begin_node("reserved-memory"); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.property_u32("#address-cells", 2); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.property_u32("#size-cells", 1); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.begin_node("carveout@40000000"); !r)
    return reloco::unexpected(r.error());
  const uint32_t carveout_reg[3] = {0x0, 0x40000000, 0x1000};
  if (auto r = w.property_u32_array("reg", span<const uint32_t>(carveout_reg, 3)); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.end_node(); !r) // end /reserved-memory
    return reloco::unexpected(r.error());

  if (auto r = w.end_node(); !r) // end /
    return reloco::unexpected(r.error());

  return w.finish();
}

} // namespace

int main() {
  reloco::array<std::byte, 1024> storage{};
  auto blob = build_demo_blob(span<std::byte>(storage.data(), storage.size()));
  if (!blob) {
    std::fprintf(stderr, "failed to build demo DTB\n");
    return 1;
  }

  auto map = structo::boot_memory_map<8>::try_from_dtb(*blob);
  if (!map) {
    std::fprintf(stderr, "failed to extract memory map\n");
    return 1;
  }

  std::printf("free regions:\n");
  for (std::size_t i = 0; i < map->free.size(); ++i)
    std::printf("  [%zu] base=0x%llx size=0x%llx\n", i, static_cast<unsigned long long>(map->free[i].base),
                static_cast<unsigned long long>(map->free[i].size));

  auto largest = map->try_largest_free_region();
  if (largest) {
    std::printf("largest free region: base=0x%llx size=0x%llx\n", static_cast<unsigned long long>(largest->base),
                static_cast<unsigned long long>(largest->size));
  }

  return 0;
}
