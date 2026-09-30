// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Decodes a real-world Flattened Device Tree blob (captured from QEMU's
// aarch64 `virt` machine model -- see fixtures/qemu_virt_dtb.hpp) with both
// fdt_reader and fdt_index, checking the result against known-good facts
// about that specific machine model/configuration. This complements the
// hand-built fixtures in test_fdt_reader.cpp/test_fdt_index.cpp with a
// sanity check against actual firmware-generated output, not just a
// fixture this library's own writer produced.

#include "fixtures/qemu_virt_dtb.hpp"

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <reloco/external_vector.hpp>
#include <structo/fdt_index.hpp>
#include <structo/fdt_reader.hpp>

using structo::array;
using structo::external_vector;
using structo::span;
using structo::fdt::fdt_index;
using structo::fdt::fdt_index_node;
using structo::fdt::fdt_index_phandle_entry;
using structo::fdt::fdt_reader;
using structo::fdt::test::qemu_virt_dtb;
using structo::fdt::test::qemu_virt_dtb_size;
using build_frame = structo::fdt::detail::fdt_index_build_frame;

namespace {

using index_type = fdt_index<external_vector>;

// Real-world blobs can have many more nodes/phandles/nesting levels than
// the small hand-built trees used elsewhere; size generously.
struct real_world_index_storage {
  array<fdt_index_node, 128> nodes{};
  array<fdt_index_phandle_entry, 32> phandles{};
  array<build_frame, 16> stack{};

  [[nodiscard]] structo::result<index_type> build(const fdt_reader &reader) {
    return index_type::try_build(
        reader, external_vector<fdt_index_node>(span<fdt_index_node>(nodes.data(), nodes.size())),
        external_vector<fdt_index_phandle_entry>(span<fdt_index_phandle_entry>(phandles.data(), phandles.size())),
        external_vector<build_frame>(span<build_frame>(stack.data(), stack.size())));
  }
};

} // namespace

TEST(FdtRealWorldTest, ReaderDecodesQemuVirtDtbWithoutError) {
  auto made = fdt_reader::try_create(span<const std::byte>(qemu_virt_dtb, qemu_virt_dtb_size));
  ASSERT_TRUE(made);
  auto reader = std::move(made).value();

  std::size_t begin_count = 0;
  std::size_t end_count = 0;
  std::size_t property_count = 0;
  for (auto ev : reader) {
    ASSERT_TRUE(ev);
    switch (ev->kind) {
    case structo::fdt::fdt_event_kind::begin_node:
      ++begin_count;
      break;
    case structo::fdt::fdt_event_kind::end_node:
      ++end_count;
      break;
    case structo::fdt::fdt_event_kind::property:
      ++property_count;
      break;
    }
  }
  EXPECT_EQ(begin_count, end_count);
  EXPECT_GT(begin_count, 0u);
  EXPECT_GT(property_count, 0u);
}

TEST(FdtRealWorldTest, IndexBuildsAndCountsMatchQemuVirtLayout) {
  auto made_reader = fdt_reader::try_create(span<const std::byte>(qemu_virt_dtb, qemu_virt_dtb_size));
  ASSERT_TRUE(made_reader);
  auto reader = std::move(made_reader).value();

  real_world_index_storage storage;
  auto made_idx = storage.build(reader);
  ASSERT_TRUE(made_idx);
  auto idx = std::move(made_idx).value();

  // The captured blob has a known, fixed layout: root, 63 nodes total,
  // exactly 4 vCPUs (matching `-smp 4`), and 8 phandled nodes.
  EXPECT_EQ(idx.node_count(), 63u);

  auto root = idx.root();
  ASSERT_TRUE(root);
  EXPECT_EQ(idx.name_of(*root), "");
}

TEST(FdtRealWorldTest, FindByPathNavigatesQemuVirtLayout) {
  auto made_reader = fdt_reader::try_create(span<const std::byte>(qemu_virt_dtb, qemu_virt_dtb_size));
  ASSERT_TRUE(made_reader);
  auto reader = std::move(made_reader).value();

  real_world_index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto root = idx.root();
  ASSERT_TRUE(root);

  auto cpus = idx.find_by_path("/cpus");
  ASSERT_TRUE(cpus);
  EXPECT_EQ(idx.name_of(*cpus), "cpus");

  for (const char *cpu_path : {"/cpus/cpu@0", "/cpus/cpu@1", "/cpus/cpu@2", "/cpus/cpu@3"}) {
    auto cpu = idx.find_by_path(cpu_path);
    ASSERT_TRUE(cpu) << cpu_path;
    EXPECT_EQ(idx.parent_of(*cpu), *cpus);
  }
  EXPECT_FALSE(idx.find_by_path("/cpus/cpu@4"));

  auto memory = idx.find_by_path("/memory@40000000");
  ASSERT_TRUE(memory);
  EXPECT_EQ(idx.parent_of(*memory), *root);

  auto chosen = idx.find_by_path("/chosen");
  ASSERT_TRUE(chosen);
  auto stdout_path = idx.find_property(*chosen, "stdout-path");
  ASSERT_TRUE(stdout_path);
  EXPECT_TRUE(stdout_path->has_value());

  EXPECT_FALSE(idx.find_by_path("/no/such/node"));
  EXPECT_EQ(idx.find_by_path("/no/such/node").error(), structo::error::not_found);
}

TEST(FdtRealWorldTest, EveryPhandleRoundTripsThroughFindByPhandle) {
  auto made_reader = fdt_reader::try_create(span<const std::byte>(qemu_virt_dtb, qemu_virt_dtb_size));
  ASSERT_TRUE(made_reader);
  auto reader = std::move(made_reader).value();

  real_world_index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  std::size_t phandle_nodes_seen = 0;
  for (const auto &n : idx.all_nodes()) {
    if (n.phandle == 0)
      continue;
    ++phandle_nodes_seen;
    auto found = idx.find_by_phandle(n.phandle);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(idx.name_of(*found), n.name);
  }
  // QEMU's virt DTB (4 vCPUs) phandles the 4 CPUs, the GIC distributor, its
  // v2m MSI frame, the pl061 GPIO controller, and the APB clock -- 8 total.
  EXPECT_EQ(phandle_nodes_seen, 8u);
}

TEST(FdtRealWorldTest, EveryPropertyOfEveryNodeDecodesWithoutError) {
  auto made_reader = fdt_reader::try_create(span<const std::byte>(qemu_virt_dtb, qemu_virt_dtb_size));
  ASSERT_TRUE(made_reader);
  auto reader = std::move(made_reader).value();

  real_world_index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  std::size_t total_properties = 0;
  for (std::size_t i = 0; i < idx.node_count(); ++i) {
    auto props = idx.try_properties(i);
    ASSERT_TRUE(props);
    for (auto prop : *props) {
      ASSERT_TRUE(prop);
      EXPECT_FALSE(prop->name.empty());
      ++total_properties;
    }
  }
  EXPECT_GT(total_properties, 0u);
}
