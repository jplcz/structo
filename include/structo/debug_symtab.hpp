// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file debug_symtab.hpp
 * @brief `structo::debug_symtab_view`: an allocation-free, read-only
 * decoder for the `DSYM` (`"DSY1"`) compressed debug symbol table blob
 * format.
 *
 * A `DSYM` blob is built offline (`scripts/elf_symtab_to_blob.py`) from an
 * ELF's `.symtab`, encoding every kept symbol's address and a
 * (optionally truncated) name into a sorted, checkpoint-indexed,
 * delta/LEB128-compressed byte stream -- see
 * `docs/debug_symtab_format.md` for the full format specification this
 * header is a direct implementation of.
 *
 * The primary use case is recovering symbol names a size-optimized
 * release build already stripped from the shipped binary: the blob is
 * built from the pre-strip ELF and loaded separately (flashed to a
 * diagnostics partition, fetched over a debug transport, mapped in by
 * host-side postmortem tooling, ...) only when a crash handler/unwinder
 * actually needs to resolve an address. `try_create()` validates a
 * caller-owned `reloco::span<const std::byte>` blob (magic, address
 * width, bounds, optional CRC-32); `try_resolve()` then performs an
 * `O(log checkpoints) + O(group_size)` nearest-preceding-symbol lookup
 * against it, copying at most the blob's configured `max_name_len()`
 * bytes into a caller-supplied scratch buffer -- no allocation, no
 * exceptions, anywhere.
 *
 * `include/structo/debug_symtab_resolver.hpp` additionally adapts a
 * `debug_symtab_view` to microfmt's `symbol_resolver_traits<Tag>`
 * customization point, for direct use with
 * `microfmt::remote_symbol_view`/`make_remote_symbol`; that header is
 * kept separate so including `debug_symtab.hpp` alone never pulls in
 * microfmt.
 */

#include "detail/debug_symtab_format.hpp"
#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

// Raw byte-offset field extraction throughout (every access is bounds-
// checked against the validated header fields before use); a single
// checked boundary, matching detail/debug_symtab_format.hpp.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace structo {

/**
 * @brief Allocation-free, read-only decoder for a `DSYM` debug symbol
 * table blob. See the @file-level docs above for the format and intended
 * use.
 *
 * Never owns the blob it decodes: `try_create()` only borrows the
 * caller's `reloco::span<const std::byte>`, which must outlive every
 * `debug_symtab_view` built from it (and every `try_resolve()` call
 * against it).
 */
class RELOCO_POINTER debug_symtab_view {
public:
  /**
   * @brief One resolved lookup result.
   */
  struct resolved {
    /** @brief Absolute address of the matched symbol (the nearest symbol
     * at or below the resolved address). */
    std::uint64_t symbol_base{0};
    /** @brief Matched (possibly truncated) symbol name, pointing into the
     * caller's scratch buffer passed to @ref try_resolve. */
    reloco::string_view name{};
    /** @brief `true` when the match is exact (`addr == symbol_base`). */
    bool is_exact{false};
    /** @brief `true` when @ref name's last byte is the blob's
     * `truncation_marker` because the original name was cut to fit
     * `max_name_len()` -- advisory only (a legitimate name that happens
     * to end in the same byte at exactly `max_name_len()` bytes is
     * indistinguishable from a truncation). */
    bool name_truncated{false};
  };

  /**
   * @brief Constructs an empty (invalid) view.
   */
  constexpr debug_symtab_view() noexcept = default;

  /**
   * @brief Validates and wraps a `DSYM` blob.
   *
   * Checks `blob.size() >= 48`, the magic, that `addr_width` is `4` or
   * `8`, and that every offset/size field in the header stays within
   * `blob`'s bounds, before trusting any of them.
   *
   * @param blob Caller-owned blob; must outlive the returned view.
   * @param verify_crc When `true` (the default), also recomputes and
   * checks `payload_crc32` over `blob[48:]` -- appropriate the first time
   * a blob is loaded over an unreliable transport; a caller re-wrapping
   * an already-verified, still-resident blob may pass `false` to skip
   * the rescan.
   * @return A validated view, or `error::invalid_argument` (malformed
   * header/out-of-bounds offsets) / `error::security_violation` (CRC
   * mismatch).
   */
  [[nodiscard]] static reloco::result<debug_symtab_view> try_create(reloco::span<const std::byte> blob,
                                                                     bool verify_crc = true) noexcept {
    namespace fmt = debug_symtab::detail;

    if (blob.size() < fmt::header_size)
      return reloco::unexpected(reloco::error::invalid_argument);

    const std::byte *base = blob.data();
    if (fmt::load_le32(base + fmt::header_offset::magic) != fmt::magic)
      return reloco::unexpected(reloco::error::invalid_argument);

    const auto addr_width = static_cast<std::uint8_t>(base[fmt::header_offset::addr_width]);
    if (addr_width != 4 && addr_width != 8)
      return reloco::unexpected(reloco::error::invalid_argument);

    const auto max_name_len = static_cast<std::uint8_t>(base[fmt::header_offset::max_name_len]);
    const auto truncation_marker = static_cast<std::uint8_t>(base[fmt::header_offset::truncation_marker]);
    const auto group_size = fmt::load_le16(base + fmt::header_offset::group_size);
    const auto symbol_count = fmt::load_le32(base + fmt::header_offset::symbol_count);
    const auto checkpoint_count = fmt::load_le32(base + fmt::header_offset::checkpoint_count);
    const auto checkpoint_table_offset = fmt::load_le32(base + fmt::header_offset::checkpoint_table_offset);
    const auto entry_stream_offset = fmt::load_le32(base + fmt::header_offset::entry_stream_offset);
    const auto entry_stream_size = fmt::load_le32(base + fmt::header_offset::entry_stream_size);
    const auto build_id_offset = fmt::load_le32(base + fmt::header_offset::build_id_offset);
    const auto build_id_size = static_cast<std::uint8_t>(base[fmt::header_offset::build_id_size]);
    const auto payload_crc32 = fmt::load_le32(base + fmt::header_offset::payload_crc32);

    if (group_size == 0 || (symbol_count == 0) != (checkpoint_count == 0))
      return reloco::unexpected(reloco::error::invalid_argument);

    const std::size_t blob_size = blob.size();
    const std::size_t checkpoint_record_size = fmt::checkpoint_record_size(addr_width);
    const std::size_t checkpoint_table_bytes = static_cast<std::size_t>(checkpoint_count) * checkpoint_record_size;

    if (!fits(checkpoint_table_offset, checkpoint_table_bytes, blob_size) ||
        !fits(entry_stream_offset, entry_stream_size, blob_size) ||
        !fits(build_id_offset, build_id_size, blob_size))
      return reloco::unexpected(reloco::error::invalid_argument);

    if (verify_crc) {
      auto payload = blob.subspan(fmt::header_size);
      if (fmt::crc32_ieee(payload) != payload_crc32)
        return reloco::unexpected(reloco::error::security_violation);
    }

    debug_symtab_view view;
    view.blob_ = blob;
    view.addr_width_ = addr_width;
    view.max_name_len_ = max_name_len;
    view.truncation_marker_ = truncation_marker;
    view.group_size_ = group_size;
    view.symbol_count_ = symbol_count;
    view.checkpoint_count_ = checkpoint_count;
    view.checkpoint_table_offset_ = checkpoint_table_offset;
    view.entry_stream_offset_ = entry_stream_offset;
    view.entry_stream_size_ = entry_stream_size;
    view.build_id_offset_ = build_id_offset;
    view.build_id_size_ = build_id_size;
    return view;
  }

  /**
   * @brief Reports whether the view holds a validated blob.
   */
  [[nodiscard]] constexpr bool valid() const noexcept { return blob_.data() != nullptr; }
  [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

  /** @brief Number of symbol entries in the blob. */
  [[nodiscard]] constexpr std::uint32_t symbol_count() const noexcept { return symbol_count_; }
  /** @brief Configured truncation cap (bytes) a resolved name never exceeds. */
  [[nodiscard]] constexpr std::uint8_t max_name_len() const noexcept { return max_name_len_; }
  /** @brief Per-blob address width in bytes (`4` or `8`). */
  [[nodiscard]] constexpr std::uint8_t addr_width() const noexcept { return addr_width_; }

  /**
   * @brief Advisory check that the blob's copied GNU build-ID matches the
   * currently-running binary's, so a resolved name is only trusted once
   * the two are confirmed to describe the same build (see
   * `docs/debug_symtab_format.md`'s "Build-ID correlation" section).
   * @param running_build_id Raw build-ID bytes of the binary currently
   * executing (e.g. read from its own `.note.gnu.build-id`).
   * @return `true` when the blob carries no build-ID at all (nothing to
   * check against -- advisory, not a hard requirement), or when both
   * build-IDs are present, equal in length, and byte-for-byte identical;
   * `false` otherwise.
   */
  [[nodiscard]] bool try_matches_build_id(reloco::span<const std::byte> running_build_id) const noexcept {
    if (build_id_size_ == 0)
      return true;
    if (running_build_id.size() != build_id_size_)
      return false;
    const std::byte *stored = blob_.data() + build_id_offset_;
    for (std::size_t i = 0; i < build_id_size_; ++i) {
      if (stored[i] != running_build_id[i])
        return false;
    }
    return true;
  }

  /**
   * @brief Resolves `addr` to its nearest preceding symbol.
   *
   * `O(log checkpoint_count())` binary search plus a bounded
   * `O(group_size)` scan/decode; never allocates. Copies the matched
   * name into `scratch` (bounded by both `max_name_len()` and
   * `scratch.size()`); the returned `resolved::name` aliases `scratch`,
   * not the blob, so it remains valid independent of the blob's
   * lifetime (but only as long as `scratch` itself does).
   *
   * @param addr Address to resolve.
   * @param scratch Caller-owned storage the matched name is copied into.
   * @return The resolved symbol, or an empty `optional` if `addr`
   * precedes every symbol in the table, the view is invalid, or the
   * entry stream is malformed past the point a partial match could
   * already be recovered.
   */
  [[nodiscard]] reloco::optional<resolved> try_resolve(std::uint64_t addr,
                                                       reloco::span<char> scratch) const noexcept {
    if (!valid() || symbol_count_ == 0)
      return reloco::nullopt;

    const std::size_t record_size = debug_symtab::detail::checkpoint_record_size(addr_width_);
    const std::byte *checkpoints = blob_.data() + checkpoint_table_offset_;

    // Binary search for the last checkpoint with address <= addr.
    std::size_t lo = 0;
    std::size_t hi = checkpoint_count_;
    while (lo < hi) {
      const std::size_t mid = lo + (hi - lo) / 2;
      const std::uint64_t mid_addr =
          debug_symtab::detail::load_le_addr(checkpoints + mid * record_size, addr_width_);
      if (mid_addr <= addr)
        lo = mid + 1;
      else
        hi = mid;
    }
    if (lo == 0)
      return reloco::nullopt; // addr precedes every symbol.
    const std::size_t checkpoint_index = lo - 1;

    const std::byte *checkpoint_record = checkpoints + checkpoint_index * record_size;
    const std::uint64_t group_start_addr = debug_symtab::detail::load_le_addr(checkpoint_record, addr_width_);
    const std::uint32_t group_stream_offset =
        debug_symtab::detail::load_le32(checkpoint_record + addr_width_);

    const std::uint32_t entries_in_group =
        (checkpoint_index + 1 == checkpoint_count_)
            ? (symbol_count_ - static_cast<std::uint32_t>(checkpoint_index) * group_size_)
            : group_size_;

    auto stream = blob_.subspan(entry_stream_offset_, entry_stream_size_);
    std::size_t pos = group_stream_offset;

    // Thread "previous decoded name in this scan" through explicit local
    // state (not member state) so concurrent try_resolve() calls on the
    // same view from different threads never race or leak state into
    // each other.
    reloco::span<const std::byte> last_name{};
    bool last_name_truncated = false;
    bool last_name_valid = false;

    // Entry 0: name record only, address == group_start_addr.
    auto entry0 = read_name_record(stream, pos, last_name, last_name_truncated, last_name_valid);
    if (!entry0)
      return reloco::nullopt;

    std::uint64_t current_addr = group_start_addr;
    reloco::span<const std::byte> best_name = entry0->name;
    bool best_truncated = entry0->truncated;
    std::uint64_t best_addr = current_addr;
    bool is_exact = (addr == current_addr);

    if (!is_exact) {
      for (std::uint32_t i = 1; i < entries_in_group; ++i) {
        auto delta = debug_symtab::detail::read_uleb128(stream, pos);
        if (!delta)
          break; // Malformed/truncated stream: stop, keep best-so-far.
        current_addr += delta.value();

        reloco::optional<name_record> record;
        if (current_addr <= addr) {
          record = read_name_record(stream, pos, last_name, last_name_truncated, last_name_valid);
          if (!record)
            break;
        } else {
          // Still need to skip over this entry's name bytes could matter
          // for later entries, but since current_addr already exceeds
          // addr and addresses only increase, no later entry in this
          // group can match either -- stop without decoding it.
          break;
        }

        best_name = record->name;
        best_truncated = record->truncated;
        best_addr = current_addr;
        if (current_addr == addr) {
          is_exact = true;
          break;
        }
      }
    }

    const std::size_t copy_len = best_name.size() < scratch.size() ? best_name.size() : scratch.size();
    for (std::size_t i = 0; i < copy_len; ++i)
      scratch[i] = static_cast<char>(best_name[i]);

    resolved out{};
    out.symbol_base = best_addr;
    out.name = reloco::string_view(scratch.data(), copy_len);
    out.is_exact = is_exact;
    out.name_truncated = best_truncated && copy_len == best_name.size();
    return out;
  }

private:
  struct name_record {
    reloco::span<const std::byte> name;
    bool truncated{false};
  };

  /** @brief Decodes one name record at `*pos` within `stream`, advancing
   * `*pos` past it (or past the single control byte when repeating).
   * `last_name`/`last_name_truncated`/`last_name_valid` are the calling
   * scan's own local "previous entry in this group" state, threaded
   * through explicitly rather than held in `this` so this method stays
   * safely callable concurrently from several threads sharing one
   * `debug_symtab_view`. */
  [[nodiscard]] reloco::optional<name_record>
  read_name_record(reloco::span<const std::byte> stream, std::size_t &pos, reloco::span<const std::byte> &last_name,
                   bool &last_name_truncated, bool &last_name_valid) const noexcept {
    if (pos >= stream.size())
      return reloco::nullopt;
    const auto control = static_cast<std::uint8_t>(stream[pos]);
    ++pos;

    if ((control & debug_symtab::detail::name_control_repeat_flag) != 0) {
      if (!last_name_valid)
        return reloco::nullopt; // Malformed: entry 0 may never repeat.
      return name_record{last_name, last_name_truncated};
    }

    const std::uint8_t length = control & debug_symtab::detail::name_control_length_mask;
    auto bytes = stream.try_subspan(pos, length);
    if (!bytes)
      return reloco::nullopt;
    pos += length;

    last_name = *bytes;
    last_name_truncated =
        length > 0 && length == max_name_len_ && static_cast<std::uint8_t>((*bytes)[length - 1]) == truncation_marker_;
    last_name_valid = true;
    return name_record{*bytes, last_name_truncated};
  }

  [[nodiscard]] static constexpr bool fits(std::uint32_t offset, std::size_t size, std::size_t blob_size) noexcept {
    return static_cast<std::size_t>(offset) <= blob_size && size <= blob_size - offset;
  }

  reloco::span<const std::byte> blob_{};
  std::uint32_t symbol_count_{0};
  std::uint32_t checkpoint_count_{0};
  std::uint32_t checkpoint_table_offset_{0};
  std::uint32_t entry_stream_offset_{0};
  std::uint32_t entry_stream_size_{0};
  std::uint32_t build_id_offset_{0};
  std::uint16_t group_size_{0};
  std::uint8_t addr_width_{0};
  std::uint8_t max_name_len_{0};
  std::uint8_t truncation_marker_{0};
  std::uint8_t build_id_size_{0};
};

} // namespace structo

RELOCO_END_UNSAFE_BUFFER_USAGE
