// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file fdt_reader.hpp
 * @brief `fdt_reader`: a Rust-style, bounds-checked iterable view over a
 * Flattened Device Tree (DTB, `/dts-v1/`) blob held in a caller-owned
 * `span<const std::byte>`.
 *
 * Unlike `fdt_writer.hpp`'s producer, the bytes a reader walks are, by
 * definition, untrusted input (a bootloader-supplied blob, a file read
 * from disk, bytes off a wire) -- every offset/length/token here is
 * attacker- or corruption-controlled, so *nothing* here is allowed to
 * trap. `fdt_reader::try_create` validates the fixed header once; after
 * that, iterating the structure block (`begin_node`/`end_node`/`property`
 * events, via `reloco::iterator_adaptor`'s Rust-style `next()`/range-for)
 * bounds-checks every single token, name, and property length against the
 * span on every step, surfacing any out-of-bounds offset or malformed
 * token as a `reloco::error` item instead of undefined behavior -- exactly
 * once, then the iterator is permanently exhausted (`optional<Item>`
 * empty forever after), matching `iterator_adaptor`'s "never polled again
 * once it has returned empty" contract.
 *
 * All multi-byte fields are big-endian, matching the DTB spec (see
 * `detail/fdt_format.hpp`, shared with `fdt_writer.hpp`). No floating
 * point is used anywhere in this file.
 */

#include <reloco/detail/fdt_format.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/iterator.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>
#include <cstdint>

namespace structo::fdt {

using namespace reloco;

// This file walks the DTB blob through raw std::byte* pointer arithmetic
// derived from checked span offsets throughout; treated as a single
// checked boundary like reloco/bytes.hpp and reloco/string_view.hpp.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

/** @brief One physical memory reservation entry (`/memreserve/`-style),
 * yielded by `fdt_reader::mem_reserves()`. */
struct mem_reserve_entry {
  uint64_t address{};
  uint64_t size{};
};

/** @brief What kind of structure-block token an `fdt_event` describes. */
enum class fdt_event_kind : uint8_t { begin_node, end_node, property };

/** @brief A property's name and raw value bytes, borrowed directly from
 * the span the owning `fdt_reader` was created from. Valid only while
 * that span (and the `fdt_reader`/event that produced it) is alive. */
struct fdt_property_view {
  string_view name;
  span<const std::byte> value;

  /** @brief Interprets `value` as a single big-endian `uint32_t`
   * (`<u32>`-style property), failing with `error::invalid_argument`
   * unless `value.size() == 4`. */
  [[nodiscard]] result<uint32_t> try_as_u32() const noexcept {
    if (value.size() != 4)
      return unexpected(error::invalid_argument);
    return detail::load_be32(value.data());
  }

  /** @brief Interprets `value` as a single big-endian `uint64_t`
   * (`<u64>`-style property), failing with `error::invalid_argument`
   * unless `value.size() == 8`. */
  [[nodiscard]] result<uint64_t> try_as_u64() const noexcept {
    if (value.size() != 8)
      return unexpected(error::invalid_argument);
    return detail::load_be64(value.data());
  }

  /** @brief Interprets `value` as a single NUL-terminated string
   * (a plain `"..."`-style property), failing with
   * `error::invalid_argument` unless it ends in exactly one trailing NUL. */
  [[nodiscard]] result<string_view> try_as_string() const noexcept {
    if (value.empty() || value[value.size() - 1] != std::byte{0})
      return unexpected(error::invalid_argument);
    return string_view(reinterpret_cast<const char *>(value.data()), value.size() - 1);
  }
};

/** @brief One structure-block event yielded while iterating an
 * `fdt_reader`: entering a node, leaving a node, or a property attached to
 * the node currently open. `node_name` is meaningful only when
 * `kind == begin_node`; `prop` only when `kind == property`. Both borrow
 * directly from the span the owning `fdt_reader` was created from. */
struct fdt_event {
  fdt_event_kind kind{};
  string_view node_name;
  fdt_property_view prop;
};

/**
 * @brief Rust-style pull/range iterator over an `fdt_reader`'s
 * `/memreserve/`-style memory reservation list -- see
 * `fdt_reader::mem_reserves()`.
 */
class RELOCO_POINTER mem_reserve_iterator : public iterator_adaptor<mem_reserve_iterator, result<mem_reserve_entry>> {
public:
  using item_type = result<mem_reserve_entry>;

  explicit mem_reserve_iterator(span<const std::byte> region) noexcept : region_(region) {}

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    if (done_)
      return nullopt;
    // The region's length was already validated (in `fdt_reader::try_create`)
    // to be a multiple of 16, so running past its end without having seen a
    // {0, 0} terminator entry means the caller-supplied blob is corrupt.
    auto entry = region_.try_subspan(cursor_, 16);
    if (!entry) {
      done_ = true;
      return optional<item_type>(item_type(unexpected(error::invalid_argument)));
    }
    const uint64_t address = detail::load_be64(entry->data());
    const uint64_t size = detail::load_be64(entry->data() + 8);
    cursor_ += 16;
    if (address == 0 && size == 0) {
      done_ = true;
      return nullopt;
    }
    return optional<item_type>(item_type(mem_reserve_entry{address, size}));
  }

private:
  span<const std::byte> region_;
  std::size_t cursor_{0};
  bool done_{false};
};

/**
 * @brief Bounds-checked, single-pass, Rust-style iterable view over a
 * Flattened Device Tree blob held in a caller-owned `span<const std::byte>`.
 *
 * Obtained only through `try_create()`, which validates the fixed 40-byte
 * header (magic, version, and every offset/size field) once, up front.
 * From there, `fdt_reader` *is* a `reloco::iterator_adaptor`: `next()` and
 * range-for both walk the structure block one token at a time, yielding
 * `result<fdt_event>` -- `begin_node`/`end_node`/`property` on success, or
 * a `reloco::error` the first time a token, name, or length turns out to
 * be out of bounds or malformed (every subsequent poll then returns empty,
 * never re-parsing). `mem_reserves()` returns a second, independent
 * iterator over the `/memreserve/`-style memory reservation list.
 *
 * A pure, read-only, non-owning view: unlike `fdt_writer` (which mutates
 * shared external memory through its cursors), copying an `fdt_reader`
 * just forks an independent cursor over the same immutable bytes, so it
 * is left ordinarily copyable and movable, matching `span`/`string_view`.
 *
 * Usage:
 * @code
 * auto made = reloco::fdt::fdt_reader::try_create(blob);
 * if (!made)
 *   return made.error();
 * auto r = std::move(made).value();
 * for (auto reserve : r.mem_reserves()) {
 *   if (!reserve)
 *     return reserve.error();
 *   // reserve->address, reserve->size
 * }
 * for (auto ev : r) {
 *   if (!ev)
 *     return ev.error();
 *   switch (ev->kind) {
 *   case reloco::fdt::fdt_event_kind::begin_node: // ev->node_name
 *   case reloco::fdt::fdt_event_kind::end_node:
 *   case reloco::fdt::fdt_event_kind::property: // ev->prop.name / ev->prop.value
 *     break;
 *   }
 * }
 * @endcode
 */
class RELOCO_POINTER fdt_reader : public iterator_adaptor<fdt_reader, result<fdt_event>> {
public:
  using item_type = result<fdt_event>;

  /**
   * @brief Fallible factory: validates the header (magic, version, and
   * every offset/size field against `blob`'s actual size) before any
   * token is ever parsed. `blob` must outlive the returned reader and
   * every `fdt_event`/`mem_reserve_entry` it yields.
   */
  [[nodiscard]] static result<fdt_reader> try_create(span<const std::byte> blob) noexcept;

  /**
   * @brief Peeks at just the header's magic/version/`totalsize` fields to
   * determine how large a buffer this blob actually needs, without
   * requiring the whole blob to be mapped/available yet.
   *
   * `try_create` validates the *whole* `totalsize`-length span up front,
   * so the caller must already know that size before calling it --
   * exactly the problem this solves. Typical use: a bootloader/firmware
   * hands over a DTB pointer with no separately-known length (only the
   * fixed 40-byte header is guaranteed readable up front); call this on
   * just that header prefix to learn `totalsize`, then map/copy/allocate
   * exactly that many bytes and pass the full span to `try_create`.
   *
   * @param header The first `detail::header_size` (40) bytes of the
   * blob; a longer span is accepted (only the prefix is read).
   * @return The blob's declared `totalsize`, or `error::out_of_bounds` if
   * `header` is shorter than the fixed header, or `error::invalid_argument`
   * if the magic number or format version doesn't match a valid DTB.
   */
  [[nodiscard]] static result<std::size_t> try_probe_size(span<const std::byte> header) noexcept;

  /** @brief DTB format version this blob declares itself as. */
  [[nodiscard]] uint32_t version() const noexcept { return version_; }
  /** @brief Oldest format version a reader must support to parse this blob. */
  [[nodiscard]] uint32_t last_comp_version() const noexcept { return last_comp_version_; }
  /** @brief Physical CPU ID the boot CPU was seated on, as recorded in the header. */
  [[nodiscard]] uint32_t boot_cpuid_phys() const noexcept { return boot_cpuid_phys_; }

  /** @brief The validated, header-relative logical prefix of the span
   * passed to `try_create` (i.e. `blob.subspan(0, totalsize)`). */
  [[nodiscard]] span<const std::byte> blob() const noexcept RELOCO_LIFETIMEBOUND { return blob_; }

  /** @brief A fresh, independent iterator over this blob's
   * `/memreserve/`-style memory reservation list. */
  [[nodiscard]] mem_reserve_iterator mem_reserves() const noexcept { return mem_reserve_iterator(mem_rsvmap_region_); }

  /** @brief The raw struct block region (`off_dt_struct`/`size_dt_struct`),
   * for code that needs to decode arbitrary struct-block offsets directly
   * rather than through this reader's own sequential cursor -- see
   * `fdt_index.hpp`, which walks this region independently to build a
   * random-access index. */
  [[nodiscard]] span<const std::byte> struct_region() const noexcept RELOCO_LIFETIMEBOUND { return struct_region_; }

  /** @brief The raw strings block region (`off_dt_strings`/`size_dt_strings`),
   * needed alongside `struct_region()` to resolve property name offsets
   * when decoding struct-block tokens directly. */
  [[nodiscard]] span<const std::byte> strings_region() const noexcept RELOCO_LIFETIMEBOUND { return strings_region_; }

  /** @brief `Derived::next_impl()` primitive required by
   * `reloco::iterator_adaptor`; use `next()`/range-for instead of calling
   * this directly. */
  [[nodiscard]] optional<item_type> next_impl() noexcept;

private:
  fdt_reader(span<const std::byte> blob, span<const std::byte> mem_rsvmap_region, span<const std::byte> struct_region,
             span<const std::byte> strings_region, uint32_t version, uint32_t last_comp_version,
             uint32_t boot_cpuid_phys) noexcept
      : blob_(blob), mem_rsvmap_region_(mem_rsvmap_region), struct_region_(struct_region),
        strings_region_(strings_region), version_(version), last_comp_version_(last_comp_version),
        boot_cpuid_phys_(boot_cpuid_phys) {}

  [[nodiscard]] optional<item_type> fail(error e) noexcept {
    done_ = true;
    return optional<item_type>(item_type(unexpected(e)));
  }

  span<const std::byte> blob_;
  span<const std::byte> mem_rsvmap_region_;
  span<const std::byte> struct_region_;
  span<const std::byte> strings_region_;
  uint32_t version_{};
  uint32_t last_comp_version_{};
  uint32_t boot_cpuid_phys_{};
  std::size_t cursor_{0}; // Offset within struct_region_.
  int depth_{0};
  bool done_{false};
};

[[nodiscard]] inline result<std::size_t> fdt_reader::try_probe_size(span<const std::byte> header) noexcept {
  if (header.size() < detail::header_size)
    return unexpected(error::out_of_bounds);

  auto magic_r = detail::read_u32_at(header, 0);
  auto totalsize_r = detail::read_u32_at(header, 4);
  auto version_r = detail::read_u32_at(header, 20);
  if (!magic_r || !totalsize_r || !version_r)
    return unexpected(error::out_of_bounds); // unreachable: header.size() >= header_size already guarantees this.

  if (*magic_r != magic)
    return unexpected(error::invalid_argument);
  if (*version_r < reloco::fdt::last_comp_version)
    return unexpected(error::invalid_argument);

  const auto totalsize = static_cast<std::size_t>(*totalsize_r);
  if (totalsize < detail::header_size)
    return unexpected(error::invalid_argument); // Can't be smaller than the header it's embedded in.
  return totalsize;
}

[[nodiscard]] inline result<fdt_reader> fdt_reader::try_create(span<const std::byte> blob) noexcept {
  if (blob.size() < detail::header_size)
    return unexpected(error::out_of_bounds);

  auto magic_r = detail::read_u32_at(blob, 0);
  auto totalsize_r = detail::read_u32_at(blob, 4);
  auto off_struct_r = detail::read_u32_at(blob, 8);
  auto off_strings_r = detail::read_u32_at(blob, 12);
  auto off_mem_rsvmap_r = detail::read_u32_at(blob, 16);
  auto version_r = detail::read_u32_at(blob, 20);
  auto last_comp_version_r = detail::read_u32_at(blob, 24);
  auto boot_cpuid_r = detail::read_u32_at(blob, 28);
  auto size_strings_r = detail::read_u32_at(blob, 32);
  auto size_struct_r = detail::read_u32_at(blob, 36);
  if (!magic_r || !totalsize_r || !off_struct_r || !off_strings_r || !off_mem_rsvmap_r || !version_r ||
      !last_comp_version_r || !boot_cpuid_r || !size_strings_r || !size_struct_r)
    return unexpected(error::out_of_bounds); // unreachable: blob.size() >= header_size already guarantees this.

  if (*magic_r != magic)
    return unexpected(error::invalid_argument);
  // Every DTB in the wild (u-boot, QEMU, `dtc`) emits version 17; accept
  // anything back to the last format revision that uses this fixed
  // 10-field, 40-byte header layout (see `detail/fdt_format.hpp`).
  if (*version_r < reloco::fdt::last_comp_version)
    return unexpected(error::invalid_argument);

  const auto totalsize = static_cast<std::size_t>(*totalsize_r);
  auto logical_r = blob.try_subspan(0, totalsize);
  if (!logical_r)
    return unexpected(error::out_of_bounds);
  const span<const std::byte> logical = *logical_r;

  const auto off_mem_rsvmap = static_cast<std::size_t>(*off_mem_rsvmap_r);
  const auto off_struct = static_cast<std::size_t>(*off_struct_r);
  const auto off_strings = static_cast<std::size_t>(*off_strings_r);
  const auto size_struct = static_cast<std::size_t>(*size_struct_r);
  const auto size_strings = static_cast<std::size_t>(*size_strings_r);

  if (off_mem_rsvmap < detail::header_size || off_struct < off_mem_rsvmap)
    return unexpected(error::invalid_argument);
  if (off_struct % 4 != 0)
    return unexpected(error::invalid_argument); // FDT_BEGIN_NODE/etc. tokens must be 4-byte aligned.

  const std::size_t mem_rsvmap_len = off_struct - off_mem_rsvmap;
  if (mem_rsvmap_len % 16 != 0)
    return unexpected(error::invalid_argument); // Every mem_rsvmap entry is a fixed 16 bytes.

  auto mem_rsvmap_region_r = logical.try_subspan(off_mem_rsvmap, mem_rsvmap_len);
  auto struct_region_r = logical.try_subspan(off_struct, size_struct);
  auto strings_region_r = logical.try_subspan(off_strings, size_strings);
  if (!mem_rsvmap_region_r || !struct_region_r || !strings_region_r)
    return unexpected(error::out_of_bounds);

  return fdt_reader(logical, *mem_rsvmap_region_r, *struct_region_r, *strings_region_r, *version_r,
                     *last_comp_version_r, *boot_cpuid_r);
}

[[nodiscard]] inline optional<fdt_reader::item_type> fdt_reader::next_impl() noexcept {
  if (done_)
    return nullopt;
  for (;;) {
    auto tok_r = detail::read_u32_at(struct_region_, cursor_);
    if (!tok_r)
      return fail(tok_r.error());
    const uint32_t tok = *tok_r;

    if (tok == detail::token_nop) {
      cursor_ += 4;
      continue;
    }

    if (tok == detail::token_begin_node) {
      auto name_r = detail::read_cstring(struct_region_, cursor_ + 4);
      if (!name_r)
        return fail(name_r.error());
      cursor_ = detail::align4(name_r->second);
      ++depth_;
      fdt_event ev{};
      ev.kind = fdt_event_kind::begin_node;
      ev.node_name = name_r->first;
      return optional<item_type>(item_type(ev));
    }

    if (tok == detail::token_end_node) {
      if (depth_ == 0)
        return fail(error::invalid_argument); // FDT_END_NODE without a matching FDT_BEGIN_NODE.
      --depth_;
      cursor_ += 4;
      fdt_event ev{};
      ev.kind = fdt_event_kind::end_node;
      return optional<item_type>(item_type(ev));
    }

    if (tok == detail::token_prop) {
      auto len_r = detail::read_u32_at(struct_region_, cursor_ + 4);
      if (!len_r)
        return fail(len_r.error());
      auto nameoff_r = detail::read_u32_at(struct_region_, cursor_ + 8);
      if (!nameoff_r)
        return fail(nameoff_r.error());
      auto value_r = struct_region_.try_subspan(cursor_ + 12, static_cast<std::size_t>(*len_r));
      if (!value_r)
        return fail(error::out_of_bounds);
      auto name_r = detail::read_cstring(strings_region_, static_cast<std::size_t>(*nameoff_r));
      if (!name_r)
        return fail(name_r.error());
      // `value_r`'s success already proves `cursor_ + 12 + *len_r` fits within
      // `struct_region_`, so this addition cannot overflow.
      cursor_ = detail::align4(cursor_ + 12 + static_cast<std::size_t>(*len_r));
      fdt_event ev{};
      ev.kind = fdt_event_kind::property;
      ev.prop.name = name_r->first;
      ev.prop.value = *value_r;
      return optional<item_type>(item_type(ev));
    }

    if (tok == detail::token_end) {
      if (depth_ != 0)
        return fail(error::invalid_argument); // FDT_END while nodes are still open.
      done_ = true;
      return nullopt;
    }

    return fail(error::invalid_argument); // Unknown token.
  }
}

RELOCO_END_UNSAFE_BUFFER_USAGE

} // namespace structo::fdt
