// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file device_tree.hpp
 * @brief `structo::device_tree`: a thin, allocation-free bundle of
 * `structo::fdt::fdt_reader` + `structo::fdt::fdt_index` plus the handful
 * of `/chosen`-node lookups (`bootargs`, `stdout-path`) almost every
 * kernel/hypervisor boot path needs from its Flattened Device Tree
 * before anything else runs.
 *
 * Node/phandle/build-scratch storage stays entirely caller-owned (via
 * `device_tree_storage<NodeCapacity, PhandleCapacity, StackDepth>`, sized
 * from a known bound on the target's DTB), matching `fdt_index` itself
 * and the rest of reloco's "no heap before the allocator exists"
 * convention -- `structo::device_tree` just saves every caller from
 * re-deriving the `external_vector<T>`-over-`array<T, N>` plumbing
 * `fdt_index::try_build` expects.
 */

#include <reloco/array.hpp>
#include <reloco/external_vector.hpp>
#include <structo/fdt_index.hpp>
#include <structo/fdt_reader.hpp>

namespace structo {

/** @brief Caller-owned node/phandle/build-scratch storage for
 * `device_tree::try_open`. Size every capacity from a known bound on the
 * target's DTB (node count, phandle count, and maximum nesting depth,
 * respectively) -- exactly what `structo::fdt::fdt_index::try_build`
 * itself requires of its three container arguments. */
template <std::size_t NodeCapacity, std::size_t PhandleCapacity = NodeCapacity, std::size_t StackDepth = 32>
struct device_tree_storage {
  reloco::array<structo::fdt::fdt_index_node, NodeCapacity> nodes{};
  reloco::array<structo::fdt::fdt_index_phandle_entry, PhandleCapacity> phandles{};
  reloco::array<structo::fdt::detail::fdt_index_build_frame, StackDepth> stack{};
};

/** @brief `structo::fdt::fdt_reader` + a random-access `fdt_index` over
 * caller-owned `device_tree_storage`, plus the `/chosen`-node
 * conveniences every boot path reaches for first. Move-only (the
 * underlying `fdt_index` is `RELOCO_OWNER`-tagged); `reader` itself is a
 * cheap, copyable cursor over the same caller-owned blob the index was
 * built from. */
struct device_tree {
  structo::fdt::fdt_reader reader;
  structo::fdt::fdt_index<reloco::external_vector> index;

  /** @brief Parses @p dtb_blob and builds a random-access index over it
   * into @p storage. Fails with whatever error `fdt_reader::try_create`
   * or `fdt_index::try_build` reports: a malformed/truncated blob, or
   * `storage`'s node/phandle/stack capacity being too small for it. */
  template <std::size_t NodeCapacity, std::size_t PhandleCapacity, std::size_t StackDepth>
  [[nodiscard]] static reloco::result<device_tree>
  try_open(reloco::span<const std::byte> dtb_blob,
           device_tree_storage<NodeCapacity, PhandleCapacity, StackDepth> &storage) noexcept {
    auto reader = structo::fdt::fdt_reader::try_create(dtb_blob);
    if (!reader)
      return reloco::unexpected(reader.error());

    auto index = structo::fdt::fdt_index<reloco::external_vector>::try_build(
        *reader,
        reloco::external_vector<structo::fdt::fdt_index_node>(
            reloco::span<structo::fdt::fdt_index_node>(storage.nodes.data(), storage.nodes.size())),
        reloco::external_vector<structo::fdt::fdt_index_phandle_entry>(
            reloco::span<structo::fdt::fdt_index_phandle_entry>(storage.phandles.data(), storage.phandles.size())),
        reloco::external_vector<structo::fdt::detail::fdt_index_build_frame>(
            reloco::span<structo::fdt::detail::fdt_index_build_frame>(storage.stack.data(), storage.stack.size())));
    if (!index)
      return reloco::unexpected(index.error());

    return device_tree{*reader, std::move(*index)};
  }

  /** @brief Looks up a direct property named @p name on the node at
   * @p path (e.g. `"/chosen"`). Fails with `error::not_found` if @p path
   * doesn't resolve or the property doesn't exist, or whatever error
   * `find_by_path`/`find_property` propagate from a malformed index. */
  [[nodiscard]] reloco::result<structo::fdt::fdt_property_view>
  try_find_property(reloco::string_view path, reloco::string_view name) const noexcept {
    auto node = index.find_by_path(path);
    if (!node)
      return reloco::unexpected(node.error());
    auto prop = index.find_property(*node, name);
    if (!prop)
      return reloco::unexpected(prop.error());
    if (!prop->has_value())
      return reloco::unexpected(reloco::error::not_found);
    return **prop;
  }

  /** @brief `/chosen`'s `bootargs` property (the kernel command line
   * handed to it by the bootloader), as a string. */
  [[nodiscard]] reloco::result<reloco::string_view> try_bootargs() const noexcept {
    auto prop = try_find_property("/chosen", "bootargs");
    if (!prop)
      return reloco::unexpected(prop.error());
    return prop->try_as_string();
  }

  /** @brief `/chosen`'s `stdout-path` property (the preferred console
   * device, optionally followed by `:<options>`), as a string. */
  [[nodiscard]] reloco::result<reloco::string_view> try_stdout_path() const noexcept {
    auto prop = try_find_property("/chosen", "stdout-path");
    if (!prop)
      return reloco::unexpected(prop.error());
    return prop->try_as_string();
  }
};

} // namespace structo
