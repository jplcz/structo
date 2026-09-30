// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/device_tree.hpp>
#include <structo/fdt_writer.hpp>

using reloco::span;
using reloco::unexpected;
using structo::fdt::fdt_writer;

namespace {

// Same tree shape as reloco's own tests/test_fdt_index.cpp fixture:
//   / {
//     #address-cells = <2>;
//     compatible = "linux,dummy-board";
//     cpus {
//       cpu@0 { phandle = <0x10>; device_type = "cpu"; };
//       cpu@1 { phandle = <0x11>; };
//     };
//     chosen {
//       bootargs = "console=ttyS0";
//       stdout-path = "serial0:115200n8";
//     };
//   };
reloco::result<reloco::span<const std::byte>> build_sample_tree(reloco::span<std::byte> storage) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return unexpected(made.error());
  fdt_writer w = std::move(made).value();

  if (auto r = w.begin_node(""); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("#address-cells", 2); !r)
    return unexpected(r.error());
  if (auto r = w.property_string("compatible", "linux,dummy-board"); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("cpus"); !r)
    return unexpected(r.error());
  if (auto r = w.begin_node("cpu@0"); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("phandle", 0x10); !r)
    return unexpected(r.error());
  if (auto r = w.property_string("device_type", "cpu"); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());
  if (auto r = w.begin_node("cpu@1"); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("phandle", 0x11); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r) // end cpus
    return unexpected(r.error());

  if (auto r = w.begin_node("chosen"); !r)
    return unexpected(r.error());
  if (auto r = w.property_string("bootargs", "console=ttyS0"); !r)
    return unexpected(r.error());
  if (auto r = w.property_string("stdout-path", "serial0:115200n8"); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r) // end chosen
    return unexpected(r.error());

  if (auto r = w.end_node(); !r) // end /
    return unexpected(r.error());

  return w.finish();
}

} // namespace

TEST(DeviceTreeTest, TryOpenBuildsIndexOverCallerOwnedStorage) {
  reloco::array<std::byte, 4096> raw{};
  auto blob = build_sample_tree(span<std::byte>(raw.data(), raw.size()));
  ASSERT_TRUE(blob);

  structo::device_tree_storage<16> storage;
  auto dt = structo::device_tree::try_open(*blob, storage);
  ASSERT_TRUE(dt);

  // root, cpus, cpu@0, cpu@1, chosen == 5 nodes.
  EXPECT_EQ(dt->index.node_count(), 5u);
}

TEST(DeviceTreeTest, TryBootargsReturnsChosenBootargs) {
  reloco::array<std::byte, 4096> raw{};
  auto blob = build_sample_tree(span<std::byte>(raw.data(), raw.size()));
  ASSERT_TRUE(blob);

  structo::device_tree_storage<16> storage;
  auto dt = structo::device_tree::try_open(*blob, storage);
  ASSERT_TRUE(dt);

  auto bootargs = dt->try_bootargs();
  ASSERT_TRUE(bootargs);
  EXPECT_EQ(*bootargs, "console=ttyS0");
}

TEST(DeviceTreeTest, TryStdoutPathReturnsChosenStdoutPath) {
  reloco::array<std::byte, 4096> raw{};
  auto blob = build_sample_tree(span<std::byte>(raw.data(), raw.size()));
  ASSERT_TRUE(blob);

  structo::device_tree_storage<16> storage;
  auto dt = structo::device_tree::try_open(*blob, storage);
  ASSERT_TRUE(dt);

  auto stdout_path = dt->try_stdout_path();
  ASSERT_TRUE(stdout_path);
  EXPECT_EQ(*stdout_path, "serial0:115200n8");
}

TEST(DeviceTreeTest, TryFindPropertyFailsWhenPathMissing) {
  reloco::array<std::byte, 4096> raw{};
  auto blob = build_sample_tree(span<std::byte>(raw.data(), raw.size()));
  ASSERT_TRUE(blob);

  structo::device_tree_storage<16> storage;
  auto dt = structo::device_tree::try_open(*blob, storage);
  ASSERT_TRUE(dt);

  auto missing = dt->try_find_property("/no-such-node", "bootargs");
  EXPECT_FALSE(missing);
}
