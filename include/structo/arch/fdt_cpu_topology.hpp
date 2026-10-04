// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fdt_cpu_topology.hpp
 * @brief `structo::arch::fdt_cpu_topology_decoder`: populates a
 * `cpu_topology<MaxCpus, MaxLevels, LevelId>` (see `cpu_topology.hpp`)
 * from a devicetree's `/cpus/cpu-map` node -- the ePAPR/Linux binding
 * describing a `socketN`/`clusterN` (arbitrarily nested)/`coreN`/
 * `threadN` hierarchy, each leaf a `cpu = <&phandle>;` reference to a
 * `/cpus/cpu@N` node.
 *
 * Built on top of `fdt_index` (not `fdt_reader`'s single-pass streaming
 * API, unlike `fdt_cpu_map.hpp`): scheduler-topology discovery happens
 * well after early boot, once a real index already exists, so this
 * decoder is free to use `find_by_path`/`find_by_phandle`/random child
 * lookups rather than the single-pass, no-index-yet style
 * `try_populate_hw_id_lut_from_fdt` (`fdt_cpu_map.hpp`) needs.
 *
 * Each `/cpus` child's *logical* CPU index is assigned the same way
 * `try_populate_hw_id_lut_from_fdt` assigns one -- sequentially, in
 * devicetree order, skipping any child whose own `status` property is
 * present and not `"okay"` -- so a `cpu_topology` populated by this
 * decoder indexes CPUs consistently with an `hw_id_lut` populated by
 * that one from the same blob.
 *
 * `cpu-map` levels are assigned finest-first relative to each leaf (not
 * by absolute depth from `cpu-map`'s root), so `level 0` always means
 * "this leaf's immediate sibling group" (typically the enclosing
 * `coreN`, for an SMT `threadN` leaf) regardless of how many `clusterN`
 * levels of nesting sit above it: walking from a leaf up to `cpu-map`'s
 * direct children, the first ancestor above the leaf becomes level 0,
 * the next becomes level 1, and so on. Every group id is unique across
 * the whole tree (a running per-depth counter, never reused across
 * sibling subtrees), so `cpu_topology::shares_level`'s simple equality
 * check is sufficient -- no need to also compare coarser levels.
 *
 * @code
 * // /cpus/cpu-map { socket0 { cluster0 { core0 { thread0 { cpu = <&cpu0>; };
 * //                                                thread1 { cpu = <&cpu1>; }; }; }; }; };
 * structo::arch::cpu_topology<8, 4> topo;
 * auto count = structo::arch::fdt_cpu_topology_decoder::decode(index, topo).value();
 * // count == 2; topo.shares_level(0, 1, 0) == true (same core)
 * @endcode
 */

#include <cstddef>

#include <structo/arch/cpu_topology.hpp>
#include <structo/fdt_index.hpp>

#include <reloco/error.hpp>
#include <reloco/string_view.hpp>

namespace structo::arch {

namespace detail {

/** @brief `name`, ignoring any trailing `"@unit-address"`. Mirrors
 * `structo::fdt::detail::base_node_name`/
 * `structo::arch::detail::fdt_cpu_base_node_name` (`fdt_cpu_map.hpp`),
 * duplicated here for the same reason that one is: keeping this header's
 * dependency surface to `fdt_index.hpp`/`cpu_topology.hpp` alone. Used to
 * tell an actual `/cpus/cpu@N` child apart from its `cpu-map` sibling,
 * which `/cpus` also has but which isn't itself a CPU. */

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-stack-address"
#endif

[[nodiscard]] inline reloco::string_view fdt_cpu_topology_base_node_name(reloco::string_view name) noexcept {
  const std::size_t at = name.find('@');
  return (at == reloco::string_view::npos) ? name : name.substr(0, at);
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

/** @brief Scratch state threaded through `fdt_cpu_topology_decoder`'s
 * recursive `cpu-map` subtree walk -- never stored in the finished
 * `cpu_topology`. */
template <std::size_t MaxCpus, std::size_t MaxLevels> struct fdt_cpu_topology_scratch {
  // `path[d]` is the group id assigned to the ancestor opened at depth `d`
  // (1-based: depth 1 is a direct child of `cpu-map`); `next_id[d]` is the
  // next fresh id to hand out at depth `d`.
  std::size_t path[MaxLevels + 1]{};
  std::size_t next_id[MaxLevels + 1]{};
  // `cpu_map_node[i]` is the `/cpus` node index of logical CPU `i`.
  std::size_t cpu_map_node[MaxCpus]{};
  std::size_t cpu_count{0};
  std::size_t max_levels_seen{0};
};

/** @brief Resolves the devicetree node index at `phandle` back to the
 * logical CPU index `fdt_cpu_topology_decoder::decode`'s first pass
 * assigned it, by linear scan of `scratch.cpu_map_node` (bounded by
 * `MaxCpus`, which is always small). */
template <std::size_t MaxCpus, std::size_t MaxLevels>
[[nodiscard]] inline reloco::optional<std::size_t>
fdt_cpu_topology_logical_cpu(const fdt_cpu_topology_scratch<MaxCpus, MaxLevels> &scratch, std::size_t node) noexcept {
  for (std::size_t i = 0; i < scratch.cpu_count; ++i) {
    if (scratch.cpu_map_node[i] == node)
      return reloco::optional<std::size_t>(i);
  }
  return reloco::nullopt;
}

/** @brief Recursively walks one `cpu-map` subtree rooted at `node` (a
 * direct or indirect descendant of `/cpus/cpu-map`), assigning a fresh
 * group id to every node visited and, upon reaching a `cpu = <&phandle>;`
 * leaf, recording that CPU's per-level ids into `topo` -- see the
 * file-level docs above for the finest-first level-numbering convention.
 * Bounded to `MaxLevels` levels of `cpu-map` nesting below its direct
 * children; deeper trees fail with `error::capacity_exceeded`. */
template <template <typename T> class Container, std::size_t MaxCpus, std::size_t MaxLevels, typename LevelId>
[[nodiscard]] reloco::result<void> fdt_cpu_topology_walk(const fdt::fdt_index<Container> &index, std::size_t node,
                                                         std::size_t depth,
                                                         fdt_cpu_topology_scratch<MaxCpus, MaxLevels> &scratch,
                                                         cpu_topology<MaxCpus, MaxLevels, LevelId> &topo) noexcept {
  if (depth > MaxLevels)
    return reloco::unexpected(reloco::error::capacity_exceeded);

  scratch.path[depth] = scratch.next_id[depth]++;

  auto cpu_prop = index.find_property(node, "cpu");
  if (!cpu_prop)
    return reloco::unexpected(cpu_prop.error());

  if (cpu_prop->has_value()) {
    // Leaf: `cpu = <&phandle>;` referencing a `/cpus/cpu@N` node.
    auto phandle_v = (*cpu_prop)->try_as_u32();
    if (!phandle_v)
      return reloco::unexpected(phandle_v.error());
    auto cpu_node = index.find_by_phandle(*phandle_v);
    if (!cpu_node.has_value())
      return reloco::unexpected(reloco::error::not_found);
    auto logical = fdt_cpu_topology_logical_cpu(scratch, *cpu_node);
    if (!logical.has_value())
      return reloco::unexpected(reloco::error::not_found);

    // `depth - 1` ancestors sit above this leaf (depths 1..depth-1); the
    // nearest one (depth - 1) is level 0, the next (depth - 2) is level 1,
    // and so on -- this leaf's own id at `path[depth]` is specific to the
    // one CPU it names and is deliberately not recorded as a level.
    const std::size_t levels_here = depth - 1;
    for (std::size_t level = 0; level < levels_here; ++level) {
      topo.set_level_id(*logical, level, static_cast<LevelId>(scratch.path[depth - 1 - level]));
    }
    if (levels_here > scratch.max_levels_seen)
      scratch.max_levels_seen = levels_here;
    return {};
  }

  auto kids = index.try_children(node);
  if (!kids)
    return reloco::unexpected(kids.error());
  for (std::size_t child : *kids) {
    if (auto r = fdt_cpu_topology_walk(index, child, depth + 1, scratch, topo); !r)
      return r;
  }
  return {};
}

} // namespace detail

/**
 * @brief Derives a `cpu_topology` from a devicetree's `/cpus/cpu-map`
 * node (see the file-level docs above for the binding and level
 * numbering convention).
 */
struct fdt_cpu_topology_decoder {
  /**
   * @brief Populates `topo` from `index`'s `/cpus` and `/cpus/cpu-map`
   * nodes.
   * @param index Random-access devicetree index (see `fdt_index.hpp`).
   * @param topo  Populated with one level-id set per enabled `/cpus`
   * child found under `cpu-map`, and `level_count()` set to the deepest
   * leaf's ancestor count seen. Not cleared first, so callers wanting a
   * clean population should pass a freshly-constructed, empty
   * `cpu_topology`.
   * @return The number of enabled CPUs found under `/cpus`.
   * `error::not_found` if `/cpus` or `/cpus/cpu-map` doesn't exist, or a
   * `cpu-map` leaf's `cpu` phandle doesn't resolve to a `/cpus` child;
   * `error::capacity_exceeded` if `/cpus` has more children than
   * `topo.max_cpus`, or `cpu-map` nests deeper than `topo.max_levels`
   * levels below its direct children; otherwise whatever error a
   * malformed property/index propagates.
   */
  template <template <typename T> class Container, std::size_t MaxCpus, std::size_t MaxLevels, typename LevelId>
  [[nodiscard]] static reloco::result<std::size_t> decode(const fdt::fdt_index<Container> &index,
                                                          cpu_topology<MaxCpus, MaxLevels, LevelId> &topo) noexcept {
    auto cpus = index.find_by_path("/cpus");
    if (!cpus)
      return reloco::unexpected(cpus.error());

    detail::fdt_cpu_topology_scratch<MaxCpus, MaxLevels> scratch{};

    auto kids = index.try_children(*cpus);
    if (!kids)
      return reloco::unexpected(kids.error());
    for (std::size_t child : *kids) {
      // `/cpus` may have non-CPU siblings of its `cpu@N` children -- most
      // notably `cpu-map` itself; skip anything whose base name isn't "cpu".
      if (detail::fdt_cpu_topology_base_node_name(index.name_of(child)) != "cpu")
        continue;
      auto status = index.find_property(child, "status");
      if (!status)
        return reloco::unexpected(status.error());
      if (status->has_value()) {
        auto s = (*status)->try_as_string();
        if (s && *s != "okay")
          continue;
      }
      if (scratch.cpu_count >= MaxCpus)
        return reloco::unexpected(reloco::error::capacity_exceeded);
      scratch.cpu_map_node[scratch.cpu_count] = child;
      ++scratch.cpu_count;
    }

    auto cpu_map = index.find_child(*cpus, "cpu-map");
    if (!cpu_map)
      return reloco::unexpected(cpu_map.error());
    if (!cpu_map->has_value())
      return reloco::unexpected(reloco::error::not_found);

    auto top_kids = index.try_children(**cpu_map);
    if (!top_kids)
      return reloco::unexpected(top_kids.error());
    for (std::size_t child : *top_kids) {
      if (auto r = detail::fdt_cpu_topology_walk(index, child, 1, scratch, topo); !r)
        return reloco::unexpected(r.error());
    }

    topo.set_level_count(scratch.max_levels_seen);
    return scratch.cpu_count;
  }
};

} // namespace structo::arch
