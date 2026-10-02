// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fdt_memory.hpp
 * @brief `structo::fdt::try_extract_memory`: reads a devicetree's physical
 * memory description straight off a single-pass `fdt_reader` -- no
 * `fdt_index` build step, no per-node index buffers -- into two
 * caller-provided `reloco::region_set`s: one describing every byte of
 * installed RAM (`full`), and one describing what's actually available
 * for the caller to hand out (`free`, i.e. `full` minus every
 * reservation).
 *
 * Deliberately independent of `fdt_index.hpp`: this is meant to run
 * early enough (e.g. before any allocator/index scratch memory exists)
 * that building a random-access index isn't an option yet, so it walks
 * the struct block directly, the same way `fdt_reader` itself does.
 * Since the struct block doesn't guarantee `/memory` appears before
 * `/reserved-memory` (or vice versa), and `fdt_reader` is forward-only,
 * `full` and `free`'s reserved-memory subtraction are built from two
 * independent passes over two independent copies of the same reader
 * (`fdt_reader` is a cheap, non-owning cursor over caller-owned bytes --
 * copying it just forks the cursor, not the blob) rather than one
 * interleaved pass.
 *
 * A node is treated as describing physical RAM ("`/memory`-class") if
 * it's a direct child of the root with `device_type == "memory"`, or
 * whose name -- ignoring any `@unit-address` suffix -- is exactly
 * `"memory"`, `"secure-memory"`, or `"secure_memory"` (covers DTBs that
 * describe a secure/TEE-world RAM carve-out without a `device_type`).
 * Regular `/memory` nodes are gated by the normal `status` property
 * (active unless present and not `"okay"`); `/secure-memory` nodes are
 * instead gated by `secure-status` (the OP-TEE/TF-A convention: a TEE
 * only claims a secure-memory node once its `secure-status` is
 * `"okay"`, independent of `status`). Either property being absent
 * defaults the node to active. `/reserved-memory` children with
 * `status == "disabled"` are skipped the same way.
 */

#include "fdt_reader.hpp"
#include "region_set.hpp"
#include <reloco/optional.hpp>

namespace structo::fdt {

using namespace reloco;

namespace detail {

/** @brief Decodes a big-endian, `cells`-word (1 or 2 supported; more
 * can't fit in a `uint64_t`) address/size field starting at `offset`
 * within `value`. */
[[nodiscard]] inline result<uint64_t> read_be_cells(span<const std::byte> value, std::size_t offset,
                                                    uint32_t cells) noexcept {
  if (cells > 2)
    return unexpected(error::unsupported_operation);
  uint64_t out = 0;
  for (uint32_t i = 0; i < cells; ++i) {
    auto word = read_u32_at(value, offset + static_cast<std::size_t>(i) * 4);
    if (!word)
      return unexpected(word.error());
    out = (out << 32) | static_cast<uint64_t>(*word);
  }
  return out;
}

/** @brief Splits `reg_value` into `(address_cells + size_cells) * 4`-byte
 * entries and invokes `fn(base, size)` once per entry. Fails with
 * `error::invalid_argument` if `reg_value`'s length isn't an exact
 * multiple of one entry's byte width. */
template <typename Fn>
[[nodiscard]] result<void> for_each_reg_entry(span<const std::byte> reg_value, uint32_t address_cells,
                                              uint32_t size_cells, Fn &&fn) noexcept {
  const std::size_t entry_size = (static_cast<std::size_t>(address_cells) + size_cells) * 4;
  if (entry_size == 0 || reg_value.size() % entry_size != 0)
    return unexpected(error::invalid_argument);
  for (std::size_t offset = 0; offset < reg_value.size(); offset += entry_size) {
    auto base = read_be_cells(reg_value, offset, address_cells);
    if (!base)
      return unexpected(base.error());
    auto size = read_be_cells(reg_value, offset + static_cast<std::size_t>(address_cells) * 4, size_cells);
    if (!size)
      return unexpected(size.error());
    auto fn_res = fn(*base, *size);
    if (!fn_res)
      return unexpected(fn_res.error());
  }
  return {};
}

/** @brief `name`, ignoring any trailing `"@unit-address"`. */
[[nodiscard]] inline string_view base_node_name(string_view name) noexcept {
  const std::size_t at = name.find('@');
  if (at == string_view::npos)
    return name;
  // `name.substr(0, at)` is lifetime-bound to the `name` *parameter* (a
  // local stack copy of the view), not the string data it points at, so
  // clang's dataflow analysis flags it as returning a dangling reference
  // to a stack object. Building the view straight from the (still valid,
  // caller-owned) data pointer -- the same pointer-arithmetic idiom
  // `detail::read_cstring` uses -- sidesteps the false positive.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  string_view result(reinterpret_cast<const char *>(name.data()), at);
  RELOCO_END_UNSAFE_BUFFER_USAGE
  return result;
}

/** @brief Tracks a node's `status`/`secure-status` properties as they
 * stream by, and resolves whether the node should be treated as active.
 *
 * Regular (`/memory`) nodes are gated by the normal `status` property:
 * active unless it's present and not `"okay"`. Secure-world nodes
 * (`/secure-memory`, the OP-TEE/TF-A convention) are instead gated by
 * `secure-status`: a TEE only ever hands out ranges from a
 * `secure-memory` node whose `secure-status` is `"okay"`, regardless of
 * `status`. If both are somehow present, `secure-status` wins -- it's
 * the more specific property. Absent status of either kind defaults to
 * active, per the devicetree spec's fallback for an omitted `status`. */
struct node_status_tracker {
  bool status_seen = false;
  bool status_okay = false;
  bool secure_status_seen = false;
  bool secure_status_okay = false;

  void reset() noexcept {
    status_seen = false;
    status_okay = false;
    secure_status_seen = false;
    secure_status_okay = false;
  }

  void observe(string_view prop_name, string_view prop_value) noexcept {
    if (prop_name == "status") {
      status_seen = true;
      status_okay = prop_value == "okay";
    } else if (prop_name == "secure-status") {
      secure_status_seen = true;
      secure_status_okay = prop_value == "okay";
    }
  }

  [[nodiscard]] bool enabled() const noexcept {
    if (secure_status_seen)
      return secure_status_okay;
    if (status_seen)
      return status_okay;
    return true;
  }
};

/** @brief Pass 1: walks the whole struct block once, adding every
 * `reg` range of every active `/memory`-class direct child of the root
 * into `full`. Root's own `#address-cells`/`#size-cells` (default 2/1)
 * are what every such node's `reg` is decoded with, per the devicetree
 * spec (a node's `reg` uses the cell counts its *parent* declares). */
template <std::size_t Capacity, typename PhysInt>
result<void> scan_memory_nodes(fdt_reader reader, region_set<Capacity, PhysInt> &full) noexcept {
  int depth = 0;
  uint32_t root_address_cells = 2;
  uint32_t root_size_cells = 1;

  // Reused per depth-1 sibling of root -- only one is ever open at a time.
  bool tracking = false;
  bool is_memory = false;
  node_status_tracker status;
  optional<span<const std::byte>> pending_reg;

  for (auto ev : reader) {
    if (!ev)
      return unexpected(ev.error());
    switch (ev->kind) {
    case fdt_event_kind::begin_node: {
      const int own_depth = depth;
      ++depth;
      if (own_depth == 1) {
        tracking = true;
        status.reset();
        pending_reg = nullopt;
        is_memory = base_node_name(ev->node_name) == "memory" || base_node_name(ev->node_name) == "secure-memory" ||
                    base_node_name(ev->node_name) == "secure_memory";
      }
      break;
    }
    case fdt_event_kind::property: {
      const int own_depth = depth - 1;
      if (own_depth == 0) {
        if (ev->prop.name == "#address-cells") {
          auto v = ev->prop.try_as_u32();
          if (v)
            root_address_cells = *v;
        } else if (ev->prop.name == "#size-cells") {
          auto v = ev->prop.try_as_u32();
          if (v)
            root_size_cells = *v;
        }
      } else if (own_depth == 1 && tracking) {
        if (ev->prop.name == "status" || ev->prop.name == "secure-status") {
          auto s = ev->prop.try_as_string();
          if (s)
            status.observe(ev->prop.name, *s);
        } else if (ev->prop.name == "device_type") {
          auto s = ev->prop.try_as_string();
          if (s && *s == "memory")
            is_memory = true;
        } else if (ev->prop.name == "reg") {
          pending_reg = ev->prop.value;
        }
      }
      break;
    }
    case fdt_event_kind::end_node: {
      --depth;
      if (depth == 1 && tracking) {
        if (is_memory && status.enabled() && pending_reg.has_value()) {
          auto added = for_each_reg_entry(*pending_reg, root_address_cells, root_size_cells,
                                          [&full](uint64_t base, uint64_t size) noexcept {
                                            return full.try_add(static_cast<PhysInt>(base), static_cast<PhysInt>(size));
                                          });
          if (!added)
            return unexpected(added.error());
        }
        tracking = false;
      }
      break;
    }
    }
  }
  return {};
}

/** @brief Pass 2: walks the whole struct block once more, looking for a
 * direct child of the root named `"reserved-memory"`, then subtracts
 * every enabled child's `reg` range from `free` -- decoded with
 * `/reserved-memory`'s own `#address-cells`/`#size-cells` (default 2/1),
 * *not* root's, since that's the node that declares them for its own
 * children. */
template <std::size_t Capacity, typename PhysInt>
result<void> scan_reserved_memory(fdt_reader reader, region_set<Capacity, PhysInt> &free) noexcept {
  int depth = 0;
  bool in_reserved_memory = false;
  uint32_t address_cells = 2;
  uint32_t size_cells = 1;

  // Reused per depth-2 child of /reserved-memory -- only one is ever open at a time.
  bool tracking_child = false;
  bool child_disabled = false;
  optional<span<const std::byte>> child_pending_reg;

  for (auto ev : reader) {
    if (!ev)
      return unexpected(ev.error());
    switch (ev->kind) {
    case fdt_event_kind::begin_node: {
      const int own_depth = depth;
      ++depth;
      if (own_depth == 1) {
        in_reserved_memory = base_node_name(ev->node_name) == "reserved-memory";
        address_cells = 2;
        size_cells = 1;
      } else if (own_depth == 2 && in_reserved_memory) {
        tracking_child = true;
        child_disabled = false;
        child_pending_reg = nullopt;
      }
      break;
    }
    case fdt_event_kind::property: {
      const int own_depth = depth - 1;
      if (own_depth == 1 && in_reserved_memory) {
        if (ev->prop.name == "#address-cells") {
          auto v = ev->prop.try_as_u32();
          if (v)
            address_cells = *v;
        } else if (ev->prop.name == "#size-cells") {
          auto v = ev->prop.try_as_u32();
          if (v)
            size_cells = *v;
        }
      } else if (own_depth == 2 && in_reserved_memory && tracking_child) {
        if (ev->prop.name == "status") {
          auto s = ev->prop.try_as_string();
          if (s && *s == "disabled")
            child_disabled = true;
        } else if (ev->prop.name == "reg") {
          child_pending_reg = ev->prop.value;
        }
      }
      break;
    }
    case fdt_event_kind::end_node: {
      --depth;
      if (depth == 2 && in_reserved_memory && tracking_child) {
        if (!child_disabled && child_pending_reg.has_value()) {
          auto subtracted = for_each_reg_entry(
              *child_pending_reg, address_cells, size_cells, [&free](uint64_t base, uint64_t size) noexcept {
                return free.try_subtract(static_cast<PhysInt>(base), static_cast<PhysInt>(size));
              });
          if (!subtracted)
            return unexpected(subtracted.error());
        }
        tracking_child = false;
      } else if (depth == 1 && in_reserved_memory) {
        in_reserved_memory = false;
      }
      break;
    }
    }
  }
  return {};
}

} // namespace detail

/**
 * @brief Extracts a devicetree's physical memory description straight
 * off `reader` (not consumed -- every pass runs against an independent
 * copy of it) into two caller-provided `region_set`s.
 *
 * @param reader An `fdt_reader` over the devicetree; left unmodified.
 * @param full Populated with every `reg` range of every enabled
 * `/memory`-class direct child of the root, merging overlapping/adjacent
 * ranges. Not cleared first, so callers wanting a clean extraction
 * should pass a freshly-constructed, empty set.
 * @param free Populated with the same ranges as `full`, then has every
 * `/reserved-memory` child's `reg` range and every legacy
 * `/memreserve/`-table entry (`reader.mem_reserves()`) subtracted out.
 * @return `error::not_found` if no `/memory`-class node exists;
 * otherwise whatever error the first malformed `reg` property,
 * unsupported cell count, or `region_set` capacity overflow reports.
 */
template <std::size_t Capacity, typename PhysInt = uint64_t>
result<void> try_extract_memory(const fdt_reader &reader, region_set<Capacity, PhysInt> &full,
                                region_set<Capacity, PhysInt> &free) noexcept {
  auto pass1 = detail::scan_memory_nodes(reader, full);
  if (!pass1)
    return unexpected(pass1.error());
  if (full.empty())
    return unexpected(error::not_found);

  for (std::size_t i = 0; i < full.size(); ++i) {
    auto added = free.try_add(full[i].base, full[i].size);
    if (!added)
      return unexpected(added.error());
  }

  for (auto reserve : reader.mem_reserves()) {
    if (!reserve)
      return unexpected(reserve.error());
    auto subtracted = free.try_subtract(static_cast<PhysInt>(reserve->address), static_cast<PhysInt>(reserve->size));
    if (!subtracted)
      return unexpected(subtracted.error());
  }

  auto pass2 = detail::scan_reserved_memory(reader, free);
  if (!pass2)
    return unexpected(pass2.error());

  return {};
}

} // namespace structo::fdt
