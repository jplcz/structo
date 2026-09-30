// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fdt_writer.hpp
 * @brief `fdt_writer`: streams a Flattened Device Tree (DTB, `/dts-v1/`)
 * blob directly into a caller-owned `span<std::byte>`.
 *
 * Modeled after microfmt's `cbor.hpp` map/array writers (matching begin/end
 * scoping and the `RELOCO_CONSUMABLE`/`RELOCO_CALLABLE_WHEN` typestate
 * annotations that library borrows from `lifetime.hpp`), but unlike CBOR's
 * indefinite-length maps -- which never need to know a length up front --
 * an FDT blob's header stores fixed offsets/sizes (`off_dt_struct`,
 * `off_dt_strings`, `size_dt_struct`, `size_dt_strings`) that are only
 * known once the whole tree has been written. `fdt_writer` never
 * allocates and never moves data further than necessary to resolve that:
 * it treats the caller's span as a bump arena growing from *both* ends --
 * the structure block grows forward from just after the memory
 * reservation map, while the (deduplicated) string table grows backward
 * from `out.size()` -- so a `FDT_PROP` entry's `nameoff` can be written
 * immediately as an absolute byte position and patched into its final,
 * header-relative form during `finish()`, without a side table of patch
 * locations. `finish()` then compacts the string table down against the
 * end of the structure block (a single `std::memmove`) and returns the
 * tight, header-relative subspan actually used.
 *
 * All multi-byte fields are written big-endian, matching the DTB spec.
 * No floating point is used anywhere in this file.
 */

#include <cstdint>
#include <cstring>
#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/int_ops.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>
#include <structo/detail/fdt_format.hpp>

namespace structo::fdt {

using namespace reloco;

// This file writes/patches the DTB blob through raw std::byte* pointer
// arithmetic derived from checked span offsets throughout; treated as a
// single checked boundary like reloco/bytes.hpp and reloco/string_view.hpp.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

// Structure tokens, header layout, and the big-endian codec primitives
// (`detail::token_*`, `detail::header_size`, `detail::align4`,
// `detail::store_be32/64`, `detail::load_be32/64`) live in
// `detail/fdt_format.hpp`, shared with `fdt_reader.hpp`.

/**
 * @brief RAII, single-pass writer for an indefinite-nesting-depth,
 * streamed Flattened Device Tree blob.
 *
 * Consumed-state tracked: writes are only valid while `unconsumed`;
 * @ref finish transitions the writer to `consumed`, after which Clang's
 * `-Wconsumed` flags any further mutating call as a compile-time
 * diagnostic (mirroring `microfmt::cbor::map_writer`/`array_writer`).
 *
 * Follows reloco's fallible-construction convention (see
 * `docs/fallible-construction.md`): the span's minimum-size precondition
 * is a real runtime failure mode on the memory-constrained targets this
 * library serves (a caller-computed or configuration-derived buffer, not
 * always a compile-time constant), so it is reported through
 * `try_create()`'s `result<fdt_writer>` rather than an assert -- there is
 * no ordinary constructor to misuse.
 *
 * Usage:
 * @code
 * std::byte storage[4096];
 * auto made = structo::fdt::fdt_writer::try_create(reloco::span<std::byte>(storage, sizeof(storage)));
 * if (!made)
 *   return made.error();
 * auto w = std::move(made).value();
 * (void)w.add_mem_reserve(0, 0);
 * (void)w.begin_node("");
 * (void)w.property_u32("#address-cells", 2);
 * (void)w.begin_node("cpus");
 * (void)w.end_node();
 * (void)w.end_node();
 * auto blob = w.finish();
 * @endcode
 */
class RELOCO_POINTER RELOCO_CONSUMABLE(unconsumed) fdt_writer {
public:
  /**
   * @brief Fallible factory: validates that `out` has room for the fixed
   * header, the memory reservation map's zero terminator, and a trailing
   * `FDT_END` marker before any node/property is ever written into it.
   * `out` must outlive every call made through the returned writer and its
   * `finish()` result; it is written into directly, in place, and never
   * copied or reallocated.
   */
  [[nodiscard]] static result<fdt_writer> try_create(span<std::byte> out) noexcept {
    if (out.size() < detail::header_size + 16 + 4)
      return unexpected(error::allocation_failed);
    return fdt_writer(out);
  }

  fdt_writer(const fdt_writer &) = delete;
  fdt_writer &operator=(const fdt_writer &) = delete;

  // Move-only: the moved-from writer is poisoned via the same sticky-error
  // latch `fail()` uses, so any further call on it (including `finish()`)
  // reports `invalid_argument` instead of racing the moved-to writer over
  // the same cursors/span.
  fdt_writer(fdt_writer &&other) noexcept RELOCO_RETURN_TYPESTATE(unconsumed)
      : out_(other.out_), mem_rsvmap_cursor_(other.mem_rsvmap_cursor_), struct_start_(other.struct_start_),
        struct_cursor_(other.struct_cursor_), strings_cursor_(other.strings_cursor_), depth_(other.depth_),
        mem_rsvmap_open_(other.mem_rsvmap_open_), finished_(other.finished_), failed_(other.failed_),
        failed_error_(other.failed_error_), final_total_size_(other.final_total_size_) {
    other.failed_ = true;
    other.failed_error_ = error::invalid_argument;
  }

  /**
   * @brief Appends a `/memreserve/`-style physical memory reservation
   * entry. Must be called before the first `begin_node`/`property_*` call
   * (the reservation map is closed with its zero-terminator on first use
   * of the structure block).
   */
  [[nodiscard]] result<void> add_mem_reserve(uint64_t address, uint64_t size) & noexcept
      RELOCO_CALLABLE_WHEN("unconsumed") {
    if (auto r = check_ok(); !r)
      return r;
    if (!mem_rsvmap_open_)
      return fail(error::invalid_argument);
    if (mem_rsvmap_cursor_ + 16 > out_.size())
      return fail(error::allocation_failed);
    detail::store_be64(out_.data() + mem_rsvmap_cursor_, address);
    detail::store_be64(out_.data() + mem_rsvmap_cursor_ + 8, size);
    mem_rsvmap_cursor_ += 16;
    return {};
  }

  /** @brief Opens a node (`FDT_BEGIN_NODE`); must be matched by @ref end_node. */
  [[nodiscard]] result<void> begin_node(string_view name) & noexcept RELOCO_CALLABLE_WHEN("unconsumed") {
    if (auto r = ensure_struct_started(); !r)
      return r;
    const std::size_t name_len = name.size() + 1; // + NUL
    const std::size_t padded = detail::align4(name_len);
    if (auto r = reserve(4 + padded); !r)
      return r;
    std::byte *p = out_.data() + struct_cursor_;
    detail::store_be32(p, detail::token_begin_node);
    p += 4;
    if (!name.empty())
      std::memcpy(p, name.data(), name.size());
    for (std::size_t i = name.size(); i < padded; ++i)
      p[i] = std::byte{0};
    struct_cursor_ += 4 + padded;
    ++depth_;
    return {};
  }

  /** @brief Closes the innermost still-open node (`FDT_END_NODE`). */
  [[nodiscard]] result<void> end_node() & noexcept RELOCO_CALLABLE_WHEN("unconsumed") {
    if (auto r = ensure_struct_started(); !r)
      return r;
    if (depth_ == 0)
      return fail(error::invalid_argument);
    if (auto r = reserve(4); !r)
      return r;
    detail::store_be32(out_.data() + struct_cursor_, detail::token_end_node);
    struct_cursor_ += 4;
    --depth_;
    return {};
  }

  /** @brief Writes a raw-byte property value (`FDT_PROP`). */
  [[nodiscard]] result<void> property(string_view name, span<const std::byte> value) & noexcept
      RELOCO_CALLABLE_WHEN("unconsumed") {
    if (auto r = ensure_struct_started(); !r)
      return r;
    auto nameoff = intern_string(name);
    if (!nameoff)
      return unexpected(nameoff.error());
    const std::size_t padded = detail::align4(value.size());
    if (auto r = reserve(12 + padded); !r)
      return r;
    std::byte *p = out_.data() + struct_cursor_;
    detail::store_be32(p, detail::token_prop);
    detail::store_be32(p + 4, static_cast<uint32_t>(value.size()));
    detail::store_be32(p + 8, static_cast<uint32_t>(*nameoff)); // Absolute; patched by finish().
    p += 12;
    if (!value.empty())
      std::memcpy(p, value.data(), value.size());
    for (std::size_t i = value.size(); i < padded; ++i)
      p[i] = std::byte{0};
    struct_cursor_ += 12 + padded;
    return {};
  }

  /** @brief Writes a boolean/presence property with no value (e.g. `dma-coherent`). */
  [[nodiscard]] result<void> property_empty(string_view name) & noexcept RELOCO_CALLABLE_WHEN("unconsumed") {
    return property(name, span<const std::byte>());
  }

  /** @brief Writes a single big-endian `<u32>` cell property. */
  [[nodiscard]] result<void> property_u32(string_view name, uint32_t value) & noexcept
      RELOCO_CALLABLE_WHEN("unconsumed") {
    std::byte buf[4];
    detail::store_be32(buf, value);
    return property(name, span<const std::byte>(buf, 4));
  }

  /** @brief Writes a single big-endian `<u64>` (two-cell) property. */
  [[nodiscard]] result<void> property_u64(string_view name, uint64_t value) & noexcept
      RELOCO_CALLABLE_WHEN("unconsumed") {
    std::byte buf[8];
    detail::store_be64(buf, value);
    return property(name, span<const std::byte>(buf, 8));
  }

  /** @brief Writes an array of big-endian `<u32>` cells (e.g. `reg`, `ranges`). */
  [[nodiscard]] result<void> property_u32_array(string_view name, span<const uint32_t> cells) & noexcept
      RELOCO_CALLABLE_WHEN("unconsumed") {
    if (auto r = ensure_struct_started(); !r)
      return r;
    auto nameoff = intern_string(name);
    if (!nameoff)
      return unexpected(nameoff.error());
    const std::size_t value_size = cells.size() * 4;
    if (auto r = reserve(12 + value_size); !r) // Already 4-byte aligned.
      return r;
    std::byte *p = out_.data() + struct_cursor_;
    detail::store_be32(p, detail::token_prop);
    detail::store_be32(p + 4, static_cast<uint32_t>(value_size));
    detail::store_be32(p + 8, static_cast<uint32_t>(*nameoff));
    p += 12;
    for (std::size_t i = 0; i < cells.size(); ++i, p += 4)
      detail::store_be32(p, cells[i]);
    struct_cursor_ += 12 + value_size;
    return {};
  }

  /** @brief Writes a single NUL-terminated string property. */
  [[nodiscard]] result<void> property_string(string_view name, string_view value) & noexcept
      RELOCO_CALLABLE_WHEN("unconsumed") {
    // std::string_view::size() excludes the terminator; encode it explicitly.
    if (auto r = ensure_struct_started(); !r)
      return r;
    auto nameoff = intern_string(name);
    if (!nameoff)
      return unexpected(nameoff.error());
    const std::size_t value_len = value.size() + 1;
    const std::size_t padded = detail::align4(value_len);
    if (auto r = reserve(12 + padded); !r)
      return r;
    std::byte *p = out_.data() + struct_cursor_;
    detail::store_be32(p, detail::token_prop);
    detail::store_be32(p + 4, static_cast<uint32_t>(value_len));
    detail::store_be32(p + 8, static_cast<uint32_t>(*nameoff));
    p += 12;
    if (!value.empty())
      std::memcpy(p, value.data(), value.size());
    for (std::size_t i = value.size(); i < padded; ++i)
      p[i] = std::byte{0};
    struct_cursor_ += 12 + padded;
    return {};
  }

  /**
   * @brief Finalizes the blob: closes any still-open memory reservation
   * map, writes `FDT_END`, patches every property's `nameoff` from an
   * absolute position to its header-relative offset, compacts the string
   * table down against the end of the structure block, and writes the
   * header. Fails if any prior call on this writer failed (that error is
   * latched and returned here instead of producing a silently-incomplete
   * blob), if any node was left open, or if the span had no room for the
   * trailing `FDT_END` marker.
   */
  [[nodiscard]] result<span<const std::byte>> finish() & noexcept RELOCO_CALLABLE_WHEN("unconsumed", "consumed")
      RELOCO_SET_TYPESTATE(consumed) {
    if (finished_)
      return out_.subspan(0, final_total_size_); // Idempotent: mirrors microfmt::cbor's writers' closed_ guard.
    if (auto r = check_ok(); !r)
      return unexpected(r.error());
    if (auto r = ensure_struct_started(); !r)
      return unexpected(r.error());
    if (depth_ != 0)
      return unexpected(fail(error::invalid_argument).error()); // Unbalanced begin_node()/end_node().
    if (auto r = reserve(4); !r)
      return unexpected(r.error());
    detail::store_be32(out_.data() + struct_cursor_, detail::token_end);
    struct_cursor_ += 4;

    const std::size_t struct_end = struct_cursor_;
    const std::size_t strings_size = out_.size() - strings_cursor_;
    const std::size_t strings_region_start = strings_cursor_; // Pre-compaction; offsets are relative to this.

    patch_nameoffs(strings_region_start);

    if (strings_size > 0 && strings_cursor_ != struct_end)
      std::memmove(out_.data() + struct_end, out_.data() + strings_cursor_, strings_size);

    const std::size_t off_dt_strings = struct_end;
    const std::size_t total_size = struct_end + strings_size;

    std::byte *h = out_.data();
    detail::store_be32(h + 0, magic);
    detail::store_be32(h + 4, static_cast<uint32_t>(total_size));
    detail::store_be32(h + 8, static_cast<uint32_t>(struct_start_));
    detail::store_be32(h + 12, static_cast<uint32_t>(off_dt_strings));
    detail::store_be32(h + 16, static_cast<uint32_t>(detail::header_size));
    detail::store_be32(h + 20, version);
    detail::store_be32(h + 24, last_comp_version);
    detail::store_be32(h + 28, 0); // boot_cpuid_phys
    detail::store_be32(h + 32, static_cast<uint32_t>(strings_size));
    detail::store_be32(h + 36, static_cast<uint32_t>(struct_end - struct_start_));

    finished_ = true;
    final_total_size_ = total_size;
    return out_.subspan(0, total_size);
  }

private:
  // Only reachable through `try_create()`, once the span has already been
  // validated as large enough to hold the fixed header, the mem_rsvmap
  // terminator, and a trailing `FDT_END` marker.
  explicit fdt_writer(span<std::byte> out) noexcept RELOCO_RETURN_TYPESTATE(unconsumed)
      : out_(out), mem_rsvmap_cursor_(detail::header_size), struct_start_(detail::header_size),
        struct_cursor_(detail::header_size), strings_cursor_(out.size()) {}

  // Latches the first error hit by any mutating call so that a caller who
  // ignores one `result<void>` and keeps writing (or goes straight to
  // `finish()`) gets that same error back from `finish()` instead of a
  // silently-incomplete blob.
  [[nodiscard]] result<void> fail(error e) & noexcept {
    if (!failed_) {
      failed_ = true;
      failed_error_ = e;
    }
    return unexpected(e);
  }

  [[nodiscard]] result<void> check_ok() & noexcept {
    if (failed_)
      return unexpected(failed_error_);
    return {};
  }

  // Closes the memory reservation map (writing its zero terminator) and
  // starts the structure block on first use, in case the caller wrote no
  // properties/nodes at all.
  [[nodiscard]] result<void> ensure_struct_started() & noexcept {
    if (auto r = check_ok(); !r)
      return r;
    if (!mem_rsvmap_open_)
      return {};
    if (mem_rsvmap_cursor_ + 16 > out_.size())
      return fail(error::allocation_failed);
    detail::store_be64(out_.data() + mem_rsvmap_cursor_, 0);
    detail::store_be64(out_.data() + mem_rsvmap_cursor_ + 8, 0);
    mem_rsvmap_cursor_ += 16;
    struct_start_ = mem_rsvmap_cursor_;
    struct_cursor_ = struct_start_;
    mem_rsvmap_open_ = false;
    return {};
  }

  // Ensures `n` more bytes are available in the structure block without
  // colliding with the (backward-growing) string table.
  [[nodiscard]] result<void> reserve(std::size_t n) & noexcept {
    auto end = checked_add(struct_cursor_, n);
    if (!end || *end > strings_cursor_)
      return fail(error::allocation_failed);
    return {};
  }

  // Finds an existing copy of `name` in the (backward-growing) string
  // table, appending a new entry only if none matches. Returns the
  // string's absolute byte position within `out_`.
  [[nodiscard]] result<std::size_t> intern_string(string_view name) & noexcept {
    for (std::size_t pos = strings_cursor_; pos < out_.size();) {
      const std::byte *start = out_.data() + pos;
      std::size_t len = 0;
      while (pos + len < out_.size() && start[len] != std::byte{0})
        ++len;
      if (len == name.size() && (len == 0 || std::memcmp(start, name.data(), len) == 0))
        return pos;
      pos += len + 1; // Skip the NUL terminator.
    }
    const std::size_t needed = name.size() + 1;
    if (needed > strings_cursor_ || strings_cursor_ - needed < struct_cursor_)
      return unexpected(fail(error::allocation_failed).error());
    const std::size_t new_pos = strings_cursor_ - needed;
    if (!name.empty())
      std::memcpy(out_.data() + new_pos, name.data(), name.size());
    out_[new_pos + name.size()] = std::byte{0};
    strings_cursor_ = new_pos;
    return new_pos;
  }

  // Walks the already-written, well-formed structure block and rewrites
  // every FDT_PROP's nameoff field from the absolute position intern_string()
  // stored to its final offset relative to `strings_region_start`.
  void patch_nameoffs(std::size_t strings_region_start) & noexcept {
    std::size_t pos = struct_start_;
    while (pos < struct_cursor_) {
      const uint32_t tok = detail::load_be32(out_.data() + pos);
      pos += 4;
      if (tok == detail::token_begin_node) {
        std::size_t name_len = 0;
        while (pos + name_len < struct_cursor_ && out_[pos + name_len] != std::byte{0})
          ++name_len;
        pos += detail::align4(name_len + 1);
      } else if (tok == detail::token_prop) {
        const uint32_t len = detail::load_be32(out_.data() + pos);
        const uint32_t nameoff = detail::load_be32(out_.data() + pos + 4);
        detail::store_be32(out_.data() + pos + 4, nameoff - static_cast<uint32_t>(strings_region_start));
        pos += 8 + detail::align4(len);
      } else if (tok == detail::token_end_node || tok == detail::token_nop) {
        // No payload.
      } else {
        return; // token_end or corrupt data; nothing left to patch.
      }
    }
  }

  span<std::byte> out_;
  std::size_t mem_rsvmap_cursor_;
  std::size_t struct_start_;
  std::size_t struct_cursor_;
  std::size_t strings_cursor_;
  unsigned depth_ = 0;
  bool mem_rsvmap_open_ = true;
  bool finished_ = false;
  bool failed_ = false;
  error failed_error_ = error::invalid_argument;
  std::size_t final_total_size_ = 0;
};

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::fdt
