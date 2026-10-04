// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/fdt_cpu_topology.hpp>
#include <structo/fdt_index.hpp>
#include <structo/fdt_writer.hpp>

#include <reloco/array.hpp>
#include <reloco/external_vector.hpp>

using reloco::array;
using reloco::error;
using reloco::external_vector;
using reloco::result;
using reloco::span;
using reloco::unexpected;
using structo::arch::cpu_topology;
using structo::arch::fdt_cpu_topology_decoder;
using structo::fdt::fdt_index;
using structo::fdt::fdt_index_node;
using structo::fdt::fdt_index_phandle_entry;
using structo::fdt::fdt_reader;
using structo::fdt::fdt_writer;
using structo::fdt::detail::fdt_index_build_frame;

namespace {

/** @brief Builds a two-core, SMT-pair devicetree:
 * `/cpus { cpu@0; cpu@1; cpu-map { socket0 { cluster0 { core0 {
 * thread0 { cpu = <&cpu0>; }; thread1 { cpu = <&cpu1>; }; }; }; }; }; }`. */
result<span<const std::byte>> build_smt_pair_tree(span<std::byte> storage) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return unexpected(made.error());
  fdt_writer w = std::move(made).value();

  if (auto r = w.begin_node(""); !r)
    return unexpected(r.error());
  if (auto r = w.begin_node("cpus"); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("#address-cells", 1); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("#size-cells", 0); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("cpu@0"); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("phandle", 1); !r)
    return unexpected(r.error());
  const uint32_t reg0 = 0x0;
  if (auto r = w.property_u32_array("reg", span<const uint32_t>(&reg0, 1)); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("cpu@1"); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("phandle", 2); !r)
    return unexpected(r.error());
  const uint32_t reg1 = 0x1;
  if (auto r = w.property_u32_array("reg", span<const uint32_t>(&reg1, 1)); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());

  if (auto r = w.begin_node("cpu-map"); !r)
    return unexpected(r.error());
  if (auto r = w.begin_node("socket0"); !r)
    return unexpected(r.error());
  if (auto r = w.begin_node("cluster0"); !r)
    return unexpected(r.error());
  if (auto r = w.begin_node("core0"); !r)
    return unexpected(r.error());
  if (auto r = w.begin_node("thread0"); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("cpu", 1); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());
  if (auto r = w.begin_node("thread1"); !r)
    return unexpected(r.error());
  if (auto r = w.property_u32("cpu", 2); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r) // core0
    return unexpected(r.error());
  if (auto r = w.end_node(); !r) // cluster0
    return unexpected(r.error());
  if (auto r = w.end_node(); !r) // socket0
    return unexpected(r.error());
  if (auto r = w.end_node(); !r) // cpu-map
    return unexpected(r.error());

  if (auto r = w.end_node(); !r) // cpus
    return unexpected(r.error());
  if (auto r = w.end_node(); !r) // root
    return unexpected(r.error());

  return w.finish();
}

/** @brief Devicetree with a `/cpus` node but no `cpu-map` child. */
result<span<const std::byte>> build_cpus_without_cpu_map(span<std::byte> storage) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return unexpected(made.error());
  fdt_writer w = std::move(made).value();
  if (auto r = w.begin_node(""); !r)
    return unexpected(r.error());
  if (auto r = w.begin_node("cpus"); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());
  return w.finish();
}

/** @brief Devicetree with neither `/cpus` nor `cpu-map`. */
result<span<const std::byte>> build_tree_without_cpus(span<std::byte> storage) {
  auto made = fdt_writer::try_create(storage);
  if (!made)
    return unexpected(made.error());
  fdt_writer w = std::move(made).value();
  if (auto r = w.begin_node(""); !r)
    return unexpected(r.error());
  if (auto r = w.end_node(); !r)
    return unexpected(r.error());
  return w.finish();
}

using index_type = fdt_index<external_vector>;

result<index_type> build_index(const fdt_reader &reader, array<fdt_index_node, 32> &nodes,
                               array<fdt_index_phandle_entry, 32> &phandles,
                               array<fdt_index_build_frame, 32> &stack) {
  return index_type::try_build(
      reader, external_vector<fdt_index_node>(span<fdt_index_node>(nodes.data(), nodes.size())),
      external_vector<fdt_index_phandle_entry>(span<fdt_index_phandle_entry>(phandles.data(), phandles.size())),
      external_vector<fdt_index_build_frame>(span<fdt_index_build_frame>(stack.data(), stack.size())));
}

} // namespace

class FdtCpuTopologyTest : public ::testing::Test {};

TEST_F(FdtCpuTopologyTest, DecodesSmtSiblingsAsSharingFinestLevel) {
  array<std::byte, 1024> storage{};
  auto blob = build_smt_pair_tree(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(blob);

  auto reader = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader);

  array<fdt_index_node, 32> nodes{};
  array<fdt_index_phandle_entry, 32> phandles{};
  array<fdt_index_build_frame, 32> stack{};
  auto index = build_index(*reader, nodes, phandles, stack);
  ASSERT_TRUE(index);

  cpu_topology<8, 4> topo;
  auto count = fdt_cpu_topology_decoder::decode(*index, topo);
  ASSERT_TRUE(count);
  EXPECT_EQ(*count, 2u);
  // socket/cluster/core/thread: 3 ancestor levels above each thread leaf.
  EXPECT_EQ(topo.level_count(), 3u);
  EXPECT_TRUE(topo.shares_level(0, 1, 0));  // same core (SMT siblings)
  EXPECT_TRUE(topo.shares_level(0, 1, 1));  // same cluster
  EXPECT_TRUE(topo.shares_level(0, 1, 2));  // same socket
}

TEST_F(FdtCpuTopologyTest, FailsWithNotFoundWhenNoCpuMapNode) {
  array<std::byte, 256> storage{};
  auto blob = build_cpus_without_cpu_map(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(blob);

  auto reader = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader);

  array<fdt_index_node, 32> nodes{};
  array<fdt_index_phandle_entry, 32> phandles{};
  array<fdt_index_build_frame, 32> stack{};
  auto index = build_index(*reader, nodes, phandles, stack);
  ASSERT_TRUE(index);

  cpu_topology<8, 4> topo;
  auto count = fdt_cpu_topology_decoder::decode(*index, topo);
  ASSERT_FALSE(count);
  EXPECT_EQ(count.error(), error::not_found);
}

TEST_F(FdtCpuTopologyTest, FailsWithNotFoundWhenNoCpusNode) {
  array<std::byte, 256> storage{};
  auto blob = build_tree_without_cpus(span<std::byte>(storage.data(), storage.size()));
  ASSERT_TRUE(blob);

  auto reader = fdt_reader::try_create(*blob);
  ASSERT_TRUE(reader);

  array<fdt_index_node, 32> nodes{};
  array<fdt_index_phandle_entry, 32> phandles{};
  array<fdt_index_build_frame, 32> stack{};
  auto index = build_index(*reader, nodes, phandles, stack);
  ASSERT_TRUE(index);

  cpu_topology<8, 4> topo;
  auto count = fdt_cpu_topology_decoder::decode(*index, topo);
  ASSERT_FALSE(count);
  EXPECT_EQ(count.error(), error::not_found);
}
