// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/array.hpp>
#include <reloco/external_vector.hpp>
#include <reloco/vector.hpp>
#include <structo/fdt_index.hpp>
#include <structo/fdt_writer.hpp>

using reloco::array;
using reloco::error;
using reloco::external_vector;
using reloco::span;
using reloco::vector;
using structo::fdt::fdt_index;
using structo::fdt::fdt_index_node;
using structo::fdt::fdt_index_npos;
using structo::fdt::fdt_index_phandle_entry;
using structo::fdt::fdt_reader;
using structo::fdt::fdt_writer;
using build_frame = structo::fdt::detail::fdt_index_build_frame;

namespace {

using index_type = fdt_index<external_vector>;

// A fixed-capacity trio of caller-owned buffers, sized generously enough
// for every test tree below, mirroring how a real embedded/kernel caller
// would size these from a known bound on node/phandle/depth counts.
struct index_storage {
  array<fdt_index_node, 32> nodes{};
  array<fdt_index_phandle_entry, 16> phandles{};
  array<build_frame, 16> stack{};

  [[nodiscard]] reloco::result<index_type> build(const fdt_reader &reader) {
    return index_type::try_build(
        reader, external_vector<fdt_index_node>(span<fdt_index_node>(nodes.data(), nodes.size())),
        external_vector<fdt_index_phandle_entry>(span<fdt_index_phandle_entry>(phandles.data(), phandles.size())),
        external_vector<build_frame>(span<build_frame>(stack.data(), stack.size())));
  }
};

// Builds:
//   / {
//     #address-cells = <2>;
//     compatible = "linux,dummy-board";
//     cpus {
//       cpu@0 { phandle = <0x10>; device_type = "cpu"; };
//       cpu@1 { phandle = <0x11>; };
//     };
//     chosen {
//       bootargs = "console=ttyS0";
//     };
//   };
vector<std::byte> build_sample_blob() {
  auto storage = vector<std::byte>::try_create(4096).value();
  (void)storage.try_resize(4096, std::byte{0});
  auto made = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  auto w = std::move(made).value();
  (void)w.begin_node("");
  (void)w.property_u32("#address-cells", 2);
  (void)w.property_string("compatible", "linux,dummy-board");
  (void)w.begin_node("cpus");
  (void)w.begin_node("cpu@0");
  (void)w.property_u32("phandle", 0x10);
  (void)w.property_string("device_type", "cpu");
  (void)w.end_node();
  (void)w.begin_node("cpu@1");
  (void)w.property_u32("phandle", 0x11);
  (void)w.end_node();
  (void)w.end_node();
  (void)w.begin_node("chosen");
  (void)w.property_string("bootargs", "console=ttyS0");
  (void)w.end_node();
  (void)w.end_node();
  auto blob = w.finish();
  auto out = vector<std::byte>::try_create(blob->size()).value();
  (void)out.try_resize(blob->size(), std::byte{0});
  std::size_t i = 0;
  for (std::byte b : *blob)
    out[i++] = b;
  return out;
}

vector<std::size_t> collect_children(const index_type &idx, std::size_t node) {
  auto out = vector<std::size_t>::try_create().value();
  for (std::size_t child : idx.children(node))
    (void)out.try_push_back(child);
  return out;
}

} // namespace

TEST(FdtIndexTest, BuildSucceedsAndCountsMatch) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto made = storage.build(reader);
  ASSERT_TRUE(made);
  auto idx = std::move(made).value();
  // root, cpus, cpu@0, cpu@1, chosen == 5 nodes.
  EXPECT_EQ(idx.node_count(), 5u);
}

TEST(FdtIndexTest, RootAndChildrenNavigateWithoutDescending) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto root = idx.root();
  ASSERT_TRUE(root);
  EXPECT_EQ(idx.node(*root).name, "");
  EXPECT_EQ(idx.parent_of(*root), fdt_index_npos);

  auto root_children = collect_children(idx, *root);
  ASSERT_EQ(root_children.size(), 2u);
  EXPECT_EQ(idx.node(root_children[0]).name, "cpus");
  EXPECT_EQ(idx.node(root_children[1]).name, "chosen");

  // Grandchildren must not appear when only iterating root's direct
  // children -- confirms children() never descends past one level.
  for (std::size_t child : root_children)
    EXPECT_NE(idx.node(child).name, "cpu@0");
}

TEST(FdtIndexTest, NameOfRetrievesNodeNamesWhileIteratingAndOnFailure) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto root = idx.root();
  ASSERT_TRUE(root);
  EXPECT_EQ(idx.name_of(*root), "");

  auto root_children = collect_children(idx, *root);
  for (std::size_t child : root_children)
    EXPECT_EQ(idx.name_of(child), idx.node(child).name);

  auto ok = idx.try_name_of(*root);
  ASSERT_TRUE(ok);
  EXPECT_EQ(*ok, "");

  auto bad = idx.try_name_of(idx.node_count() + 42);
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(), error::out_of_bounds);
}

TEST(FdtIndexTest, ParentLookupIsConstantTime) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto root = idx.root();
  ASSERT_TRUE(root);
  auto cpus_children = collect_children(idx, *root);
  const std::size_t cpus = cpus_children[0];
  auto cpu_children = collect_children(idx, cpus);
  ASSERT_EQ(cpu_children.size(), 2u);
  for (std::size_t cpu : cpu_children)
    EXPECT_EQ(idx.parent_of(cpu), cpus);
  EXPECT_EQ(idx.parent_of(cpus), *root);
}

TEST(FdtIndexTest, FallibleAccessorsRejectOutOfBoundsIndexWithoutAborting) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  const std::size_t bogus = idx.node_count() + 42;

  auto bad_node = idx.try_node(bogus);
  ASSERT_FALSE(bad_node);
  EXPECT_EQ(bad_node.error(), error::out_of_bounds);

  auto bad_parent = idx.try_parent_of(bogus);
  ASSERT_FALSE(bad_parent);
  EXPECT_EQ(bad_parent.error(), error::out_of_bounds);

  auto bad_children = idx.try_children(bogus);
  ASSERT_FALSE(bad_children);
  EXPECT_EQ(bad_children.error(), error::out_of_bounds);

  auto bad_properties = idx.try_properties(bogus);
  ASSERT_FALSE(bad_properties);
  EXPECT_EQ(bad_properties.error(), error::out_of_bounds);

  // In-bounds calls still succeed and agree with the asserting siblings.
  auto root = idx.root();
  ASSERT_TRUE(root);
  auto ok_parent = idx.try_parent_of(*root);
  ASSERT_TRUE(ok_parent);
  EXPECT_EQ(*ok_parent, idx.parent_of(*root));
}

TEST(FdtIndexTest, PropertiesOfNodeExcludeChildNodesAndGrandchildProperties) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto root = idx.root();
  ASSERT_TRUE(root);
  std::size_t count = 0;
  for (auto prop : idx.properties(*root)) {
    ASSERT_TRUE(prop);
    if (count == 0) {
      EXPECT_EQ(prop->name, "#address-cells");
    } else if (count == 1) {
      EXPECT_EQ(prop->name, "compatible");
    }
    ++count;
  }
  EXPECT_EQ(count, 2u);
}

TEST(FdtIndexTest, FindByPhandleLocatesNodesAndRejectsUnknown) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto cpu0 = idx.find_by_phandle(0x10);
  ASSERT_TRUE(cpu0.has_value());
  EXPECT_EQ(idx.node(*cpu0).name, "cpu@0");

  auto cpu1 = idx.find_by_phandle(0x11);
  ASSERT_TRUE(cpu1.has_value());
  EXPECT_EQ(idx.node(*cpu1).name, "cpu@1");

  EXPECT_FALSE(idx.find_by_phandle(0x99).has_value());
  EXPECT_FALSE(idx.find_by_phandle(0).has_value()); // 0 is reserved, never indexed.
}

TEST(FdtIndexTest, FindChildLocatesByNameAndReportsAbsence) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto root = idx.root();
  ASSERT_TRUE(root);

  auto cpus = idx.find_child(*root, "cpus");
  ASSERT_TRUE(cpus);
  ASSERT_TRUE(cpus->has_value());
  EXPECT_EQ(idx.node(**cpus).name, "cpus");

  auto missing = idx.find_child(*root, "does-not-exist");
  ASSERT_TRUE(missing); // not an error -- just absent.
  EXPECT_FALSE(missing->has_value());

  // Grandchildren must not be found directly under root.
  auto grandchild = idx.find_child(*root, "cpu@0");
  ASSERT_TRUE(grandchild);
  EXPECT_FALSE(grandchild->has_value());

  auto bad = idx.find_child(idx.node_count() + 42, "cpus");
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(), error::out_of_bounds);
}

TEST(FdtIndexTest, FindPropertyLocatesByNameAndReportsAbsence) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto root = idx.root();
  ASSERT_TRUE(root);

  auto compatible = idx.find_property(*root, "compatible");
  ASSERT_TRUE(compatible);
  ASSERT_TRUE(compatible->has_value());
  EXPECT_EQ((*compatible)->name, "compatible");

  auto missing = idx.find_property(*root, "no-such-property");
  ASSERT_TRUE(missing); // not an error -- just absent.
  EXPECT_FALSE(missing->has_value());

  auto bad = idx.find_property(idx.node_count() + 42, "compatible");
  ASSERT_FALSE(bad);
  EXPECT_EQ(bad.error(), error::out_of_bounds);
}

TEST(FdtIndexTest, FindByPathTranslatesFullPathsToNodes) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto root = idx.root();
  ASSERT_TRUE(root);

  auto root_by_path = idx.find_by_path("/");
  ASSERT_TRUE(root_by_path);
  EXPECT_EQ(*root_by_path, *root);

  auto cpu0 = idx.find_by_path("/cpus/cpu@0");
  ASSERT_TRUE(cpu0);
  EXPECT_EQ(idx.node(*cpu0).name, "cpu@0");

  auto chosen = idx.find_by_path("/chosen");
  ASSERT_TRUE(chosen);
  EXPECT_EQ(idx.node(*chosen).name, "chosen");

  // Repeated/trailing slashes are tolerated.
  auto cpu1 = idx.find_by_path("/cpus//cpu@1/");
  ASSERT_TRUE(cpu1);
  EXPECT_EQ(idx.node(*cpu1).name, "cpu@1");

  EXPECT_FALSE(idx.find_by_path("/no/such/node"));
  EXPECT_EQ(idx.find_by_path("/no/such/node").error(), error::not_found);
  EXPECT_EQ(idx.find_by_path("relative/path").error(), error::invalid_argument);
  EXPECT_EQ(idx.find_by_path("").error(), error::invalid_argument);
}

TEST(FdtIndexTest, AllNodesIsFlatPreorderView) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto idx = std::move(storage.build(reader)).value();

  auto all = idx.all_nodes();
  ASSERT_EQ(all.size(), 5u);
  EXPECT_EQ(all[0].name, "");
  EXPECT_EQ(all[1].name, "cpus");
  EXPECT_EQ(all[2].name, "cpu@0");
  EXPECT_EQ(all[3].name, "cpu@1");
  EXPECT_EQ(all[4].name, "chosen");
}

TEST(FdtIndexTest, TryBuildFailsWithCapacityExceededWhenNodeBufferTooSmall) {
  auto blob = build_sample_blob();
  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();

  array<fdt_index_node, 2> tiny_nodes{}; // 5 nodes exist; too small.
  array<fdt_index_phandle_entry, 16> phandles{};
  array<build_frame, 16> stack{};
  auto made = index_type::try_build(
      reader, external_vector<fdt_index_node>(span<fdt_index_node>(tiny_nodes.data(), tiny_nodes.size())),
      external_vector<fdt_index_phandle_entry>(span<fdt_index_phandle_entry>(phandles.data(), phandles.size())),
      external_vector<build_frame>(span<build_frame>(stack.data(), stack.size())));
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error(), error::capacity_exceeded);
}

TEST(FdtIndexTest, TryBuildFailsWithCapacityExceededWhenStackTooShallow) {
  // Six levels of nesting; the scratch stack only holds three frames.
  auto storage = vector<std::byte>::try_create(512).value();
  (void)storage.try_resize(512, std::byte{0});
  auto made_writer = fdt_writer::try_create(span<std::byte>(storage.data(), storage.size()));
  auto w = std::move(made_writer).value();
  for (int i = 0; i < 6; ++i)
    ASSERT_TRUE(w.begin_node(i == 0 ? "" : "n"));
  for (int i = 0; i < 6; ++i)
    ASSERT_TRUE(w.end_node());
  auto blob_r = w.finish();
  ASSERT_TRUE(blob_r);

  auto reader = std::move(fdt_reader::try_create(*blob_r)).value();

  array<fdt_index_node, 16> nodes{};
  array<fdt_index_phandle_entry, 4> phandles{};
  array<build_frame, 3> shallow_stack{};
  auto idx_made = index_type::try_build(
      reader, external_vector<fdt_index_node>(span<fdt_index_node>(nodes.data(), nodes.size())),
      external_vector<fdt_index_phandle_entry>(span<fdt_index_phandle_entry>(phandles.data(), phandles.size())),
      external_vector<build_frame>(span<build_frame>(shallow_stack.data(), shallow_stack.size())));
  ASSERT_FALSE(idx_made);
  EXPECT_EQ(idx_made.error(), error::capacity_exceeded);
}

TEST(FdtIndexTest, TryBuildFailsCleanlyOnCorruptStructBlock) {
  auto blob = build_sample_blob();
  auto load_be32 = [&](std::size_t off) {
    return (static_cast<uint32_t>(blob[off]) << 24) | (static_cast<uint32_t>(blob[off + 1]) << 16) |
           (static_cast<uint32_t>(blob[off + 2]) << 8) | static_cast<uint32_t>(blob[off + 3]);
  };
  const uint32_t off_dt_struct = load_be32(8);
  blob[off_dt_struct] = std::byte{0xff};
  blob[off_dt_struct + 1] = std::byte{0xff};
  blob[off_dt_struct + 2] = std::byte{0xff};
  blob[off_dt_struct + 3] = std::byte{0xff};

  auto reader = std::move(fdt_reader::try_create(span<const std::byte>(blob.data(), blob.size()))).value();
  index_storage storage;
  auto made = storage.build(reader);
  ASSERT_FALSE(made);
  EXPECT_EQ(made.error(), error::invalid_argument);
}
