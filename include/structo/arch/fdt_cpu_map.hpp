// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fdt_cpu_map.hpp
 * @brief `structo::arch::try_populate_hw_id_lut_from_fdt`: reads a
 * devicetree's `/cpus` node straight off a single-pass `fdt_reader` --
 * no `fdt_index` build step, no per-node index buffers -- registering
 * each enabled `cpu@N` child's `reg` (hardware ID) into a
 * caller-provided `hw_id_lut`, keyed by a sequential logical CPU index
 * assigned in devicetree order (0, 1, 2, ...).
 *
 * Deliberately independent of `fdt_index.hpp`, mirroring
 * `fdt_memory.hpp`'s `try_extract_memory` rationale: meant to run early
 * enough -- e.g. right after a bootloader hands off a raw DTB pointer,
 * before any allocator or index scratch memory exists yet to resolve
 * `hw_id_lut<Tag>` (or `cpu_index<Tag>`'s `Tag::current()`) against --
 * that building a random-access index isn't an option, so it walks the
 * struct block directly instead, the same way `fdt_reader` itself does.
 * Once a real index (or anything else heavier) becomes available later
 * in boot, `fdt_index`/`device_tree` are the better fit for repeated or
 * more elaborate devicetree queries; this header exists specifically for
 * the "index isn't present yet" early-boot window the file's name refers
 * to.
 *
 * A `cpu@N` node is registered if it is a direct child of a `/cpus` node
 * (the root's child whose name, ignoring any `@unit-address` suffix, is
 * exactly `"cpus"`) and its own `status` property is absent or `"okay"`
 * -- the same absent-defaults-to-active fallback every other
 * `structo::fdt` scanner uses. Its `reg` property is decoded with
 * `/cpus`'s own `#address-cells` (default 1, the devicetree spec's
 * default for `/cpus`); `/cpus`'s children have no `#size-cells` worth
 * of `reg` payload (a CPU `reg` is address-only), so only
 * `#address-cells` is read. Only the first 4 bytes of a 1-cell `reg`, or
 * the low 32 bits of a 2-cell `reg` are kept (matching `hw_id_lut`'s
 * default `uint32_t` `HwId`); pass a 64-bit `HwId` to `hw_id_lut` (and
 * this function resolves it generically through that `HwId`) to keep a
 * 2-cell ID's full width instead.
 *
 * @code
 * // A trivial single-cluster Cortex-A tree:
 * // /cpus { #address-cells = <1>;
 * //   cpu@0 { reg = <0x0>; };
 * //   cpu@1 { reg = <0x1>; }; };
 * structo::arch::hw_id_lut<uint32_t, 8> lut;
 * auto reader = structo::fdt::fdt_reader::try_create(dtb_blob).value();
 * auto count = structo::arch::try_populate_hw_id_lut_from_fdt(reader, lut);
 * // count == 2; lut.find(0x1) == 1
 * @endcode
 */

#include <structo/arch/hw_id_map.hpp>
#include <structo/fdt_reader.hpp>

#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

namespace structo::arch {

namespace detail {

/** @brief `name`, ignoring any trailing `"@unit-address"`. Mirrors
 * `structo::fdt::detail::base_node_name` (duplicated here rather than
 * shared, to keep this header's only dependencies `fdt_reader.hpp` and
 * `hw_id_map.hpp` -- not `fdt_memory.hpp`/`region_set.hpp`, which this
 * CPU-only scan has no use for). */
[[nodiscard]] inline reloco::string_view fdt_cpu_base_node_name(reloco::string_view name) noexcept {
  const std::size_t at = name.find('@');
  if (at == reloco::string_view::npos)
    return name;
  // See structo::fdt::detail::base_node_name for why this is built via the
  // raw data pointer rather than `name.substr(0, at)`.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  reloco::string_view result(reinterpret_cast<const char *>(name.data()), at);
  RELOCO_END_UNSAFE_BUFFER_USAGE
  return result;
}

/** @brief Decodes a big-endian, `cells`-word (1 or 2 supported) value
 * starting at byte offset 0 within `value`. Mirrors
 * `structo::fdt::detail::read_be_cells`, duplicated for the same reason
 * as `fdt_cpu_base_node_name` above. */
[[nodiscard]] inline reloco::result<uint64_t> fdt_cpu_read_be_cells(reloco::span<const std::byte> value,
                                                                    uint32_t cells) noexcept {
  if (cells == 0 || cells > 2)
    return reloco::unexpected(reloco::error::unsupported_operation);
  if (value.size() < static_cast<std::size_t>(cells) * 4)
    return reloco::unexpected(reloco::error::invalid_argument);
  uint64_t out = 0;
  for (uint32_t i = 0; i < cells; ++i) {
    auto word = fdt::detail::read_u32_at(value, static_cast<std::size_t>(i) * 4);
    if (!word)
      return reloco::unexpected(word.error());
    out = (out << 32) | static_cast<uint64_t>(*word);
  }
  return out;
}

} // namespace detail

/**
 * @brief Walks `reader`'s `/cpus` node and registers each enabled
 * `cpu@N` child's hardware ID into `lut`, keyed by a sequential logical
 * CPU index assigned in devicetree order.
 *
 * @param reader An `fdt_reader` over the devicetree; left unmodified
 * (iterated via its own copy).
 * @param lut Populated with one `insert(hw_id, logical_index)` call per
 * enabled `cpu@N` child found, `logical_index` starting at 0 and
 * incrementing per node in devicetree order. Not cleared first, so
 * callers wanting a clean population should pass a freshly-constructed,
 * empty `hw_id_lut`.
 * @return The number of CPUs registered (`lut.size()`'s *increase*, not
 * its absolute value, if `lut` was non-empty on entry). `error::not_found`
 * if no `/cpus` node exists; otherwise whatever error the first
 * malformed `reg` property, unsupported cell count, or `hw_id_lut`
 * capacity overflow (`insert()` returning `false`, surfaced as
 * `error::capacity_exceeded`) reports.
 */
template <typename HwId, std::size_t MaxCpus, std::size_t L1Size, typename Hash>
reloco::result<std::size_t> try_populate_hw_id_lut_from_fdt(fdt::fdt_reader reader,
                                                            hw_id_lut<HwId, MaxCpus, L1Size, Hash> &lut) noexcept {
  int depth = 0;
  bool in_cpus = false;
  uint32_t cpus_address_cells = 1;
  bool found_cpus_node = false;
  std::size_t registered = 0;

  // Reused per depth-2 child of /cpus -- only one is ever open at a time.
  bool tracking_child = false;
  bool child_disabled = false;
  bool child_has_reg = false;
  reloco::span<const std::byte> child_pending_reg;

  for (auto ev : reader) {
    if (!ev)
      return reloco::unexpected(ev.error());
    switch (ev->kind) {
    case fdt::fdt_event_kind::begin_node: {
      const int own_depth = depth;
      ++depth;
      if (own_depth == 1) {
        in_cpus = detail::fdt_cpu_base_node_name(ev->node_name) == "cpus";
        if (in_cpus) {
          found_cpus_node = true;
          cpus_address_cells = 1;
        }
      } else if (own_depth == 2 && in_cpus) {
        tracking_child = true;
        child_disabled = false;
        child_has_reg = false;
        child_pending_reg = {};
      }
      break;
    }
    case fdt::fdt_event_kind::property: {
      const int own_depth = depth - 1;
      if (own_depth == 1 && in_cpus) {
        if (ev->prop.name == "#address-cells") {
          auto v = ev->prop.try_as_u32();
          if (v)
            cpus_address_cells = *v;
        }
      } else if (own_depth == 2 && in_cpus && tracking_child) {
        if (ev->prop.name == "status") {
          auto s = ev->prop.try_as_string();
          if (s && *s != "okay")
            child_disabled = true;
        } else if (ev->prop.name == "reg") {
          child_has_reg = true;
          child_pending_reg = ev->prop.value;
        }
      }
      break;
    }
    case fdt::fdt_event_kind::end_node: {
      --depth;
      if (depth == 2 && in_cpus && tracking_child) {
        if (!child_disabled && child_has_reg) {
          auto hw_id_wide = detail::fdt_cpu_read_be_cells(child_pending_reg, cpus_address_cells);
          if (!hw_id_wide)
            return reloco::unexpected(hw_id_wide.error());
          if (registered >= hw_id_lut<HwId, MaxCpus, L1Size, Hash>::max_cpus)
            return reloco::unexpected(reloco::error::capacity_exceeded);
          const auto hw_id = static_cast<HwId>(*hw_id_wide);
          if (!lut.insert(hw_id, static_cast<uint8_t>(registered)))
            return reloco::unexpected(reloco::error::capacity_exceeded);
          ++registered;
        }
        tracking_child = false;
      } else if (depth == 1 && in_cpus) {
        in_cpus = false;
      }
      break;
    }
    }
  }

  if (!found_cpus_node)
    return reloco::unexpected(reloco::error::not_found);

  return registered;
}

} // namespace structo::arch
