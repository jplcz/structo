// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Builds a structo::device_tree over a demo Flattened Device Tree blob and
// prints its /chosen bootargs and stdout-path -- the two properties almost
// every kernel/hypervisor boot path reads first.

#include <structo/device_tree.hpp>
#include <structo/fdt_writer.hpp>

#include <cstdio>

using reloco::span;
using structo::fdt::fdt_writer;

namespace {

reloco::result<reloco::span<const std::byte>> build_demo_blob(reloco::span<std::byte> storage) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return reloco::unexpected(made.error());
  fdt_writer w = std::move(made).value();

  if (auto r = w.begin_node(""); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.begin_node("chosen"); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.property_string("bootargs", "console=ttyS0 root=/dev/vda1"); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.property_string("stdout-path", "serial0:115200n8"); !r)
    return reloco::unexpected(r.error());
  if (auto r = w.end_node(); !r) // end chosen
    return reloco::unexpected(r.error());
  if (auto r = w.end_node(); !r) // end /
    return reloco::unexpected(r.error());

  return w.finish();
}

} // namespace

int main() {
  reloco::array<std::byte, 1024> raw{};
  auto blob = build_demo_blob(span<std::byte>(raw.data(), raw.size()));
  if (!blob) {
    std::fprintf(stderr, "failed to build demo DTB\n");
    return 1;
  }

  structo::device_tree_storage<8> storage;
  auto dt = structo::device_tree::try_open(*blob, storage);
  if (!dt) {
    std::fprintf(stderr, "failed to open device tree\n");
    return 1;
  }

  auto bootargs = dt->try_bootargs();
  if (bootargs)
    std::printf("bootargs: %.*s\n", static_cast<int>(bootargs->size()), bootargs->data());

  auto stdout_path = dt->try_stdout_path();
  if (stdout_path)
    std::printf("stdout-path: %.*s\n", static_cast<int>(stdout_path->size()), stdout_path->data());

  return 0;
}
