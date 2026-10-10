// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file page_table_memory.hpp
 * @brief `structo::arch::table_arena<Phys>`: a small, allocation-free
 * page-table memory provider for `page_mapper` (see `page_mapper.hpp`),
 * carving tables out of one caller-supplied, physically-contiguous region.
 *
 * ## The "table memory" contract `page_mapper` expects
 *
 * Any type with these three members can back a `page_mapper`; `table_arena`
 * is merely the ready-made one for bootloaders / early kernels / tests.
 *
 * @code
 * struct my_table_memory {
 *   // Physical-address type of the tables (the format's `phys_type`).
 *   using phys_type = structo::phys_addr<void, structo::default_phys_space>;
 *
 *   // A zero-filled table of `entry_count` 64-bit entries, naturally aligned to
 *   // `entry_count * 8` bytes (hardware requires this for every architecture).
 *   reloco::result<phys_type> try_allocate_table(std::size_t entry_count) noexcept;
 *
 *   // Return a table obtained from try_allocate_table() (same `entry_count`).
 *   void free_table(phys_type table, std::size_t entry_count) noexcept;
 *
 *   // The CPU's view of a table: the entries as 64-bit words. An empty span means
 *   // "not a table this provider knows" and makes the mapper report invalid_state.
 *   // `where` says which level/VA range the table covers (ignore it if you can address physical memory).
 *   reloco::span<std::uint64_t> table(phys_type table, std::size_t entry_count, table_location where) noexcept;
 * };
 * @endcode
 *
 * ## `table_arena` (bootstrap only)
 *
 * Intended for the window before a real physical allocator exists (early
 * boot, bootloader, unit tests). A running kernel/hypervisor should implement
 * the contract above on top of its own page allocator (e.g. `buddy_allocator`)
 * and the direct map, and hand *that* to `page_mapper`.
 *
 * @code
 * // Caller storage: any memory the CPU can write that the MMU will later read
 * // as page tables. Here a static array placed at a known physical address.
 * alignas(4096) static std::uint64_t pool[512 * 64]; // 64 tables of 4 KiB
 *
 * // `base` is the physical address of pool[0]. It must be aligned to the largest
 * // table you will allocate (4 KiB for 4K-granule formats, 16/64 KiB for 16K/64K
 * // granules), because the arena aligns tables by *physical* address.
 * structo::arch::table_arena<phys_type> arena(pool, phys_type{0x8010'0000});
 *
 * auto t = arena.try_allocate_table(512);  // 4 KiB table, zeroed
 * arena.free_table(*t, 512);               // goes on a per-size free list for reuse
 * @endcode
 *
 * ## Walker safety of freed tables
 *
 * A freed table keeps a free-list link in its first word. The link is stored
 * shifted left by 12, so bit 0 (the valid/present bit of every supported
 * format, and the read bit of EPT) is always clear: a hardware walker that
 * still holds a stale pointer to the table sees a not-present entry, never a
 * bogus mapping.
 */

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch {

/**
 * @brief Where a table sits in the tree, passed to `table()` so providers that reach tables *by path*
 * (see `recursive_table_memory`) can compute the table's address. Providers that map physical memory
 * directly (like `table_arena`) ignore it.
 */
struct table_location {
  std::size_t level{0};  //!< 0 = root.
  std::uint64_t base{0}; //!< First (table-relative, canonical-bits-stripped) virtual address the table covers.
};

/**
 * @brief Bootstrap table provider over one caller-supplied region (no heap, bump + per-size free lists).
 * @tparam Phys Physical-address type with a public `value` member (e.g. `phys_addr<void, Space>`).
 * @tparam MaxSizeClasses Maximum number of distinct table sizes it can recycle (4 covers every
 * supported format: ordinary tables plus an enlarged root).
 */
template <typename Phys, std::size_t MaxSizeClasses = 4> class table_arena {
public:
  using phys_type = Phys;

  constexpr table_arena() noexcept = default;

  /** @brief `storage` is the table pool; `base` is the physical address of `storage[0]`. */
  constexpr table_arena(reloco::span<std::uint64_t> storage, Phys base) noexcept : storage_(storage), base_(base) {}

  /** @brief Allocates a zeroed, naturally aligned table. `allocation_failed` when the pool is exhausted. */
  [[nodiscard]] reloco::result<Phys> try_allocate_table(std::size_t entry_count) noexcept {
    if (!is_pow2(entry_count)) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    if (size_class *cls = find_class(entry_count); cls != nullptr && cls->head != 0) {
      const std::size_t word = (cls->head >> 12) - 1;
      auto words = storage_.subspan(word, entry_count);
      cls->head = words[0]; // link left behind by free_table()
      zero(words);
      ++in_use_;
      return Phys{base_.value + word * 8};
    }

    const std::uint64_t bytes = static_cast<std::uint64_t>(entry_count) * 8;
    const std::uint64_t addr = base_.value + static_cast<std::uint64_t>(next_word_) * 8;
    const std::uint64_t pad = (bytes - (addr % bytes)) % bytes;
    const std::size_t start = next_word_ + static_cast<std::size_t>(pad / 8);
    if (start > storage_.size() || storage_.size() - start < entry_count) {
      return reloco::unexpected(reloco::error::allocation_failed);
    }
    zero(storage_.subspan(start, entry_count));
    next_word_ = start + entry_count;
    ++in_use_;
    return Phys{base_.value + static_cast<std::uint64_t>(start) * 8};
  }

  /** @brief Returns a table to its size class's free list. Foreign or misaligned tables are ignored. */
  void free_table(Phys table, std::size_t entry_count) noexcept {
    auto words = this->table(table, entry_count);
    if (words.empty()) {
      return;
    }
    size_class *cls = find_class(entry_count);
    if (cls == nullptr) {
      cls = add_class(entry_count);
    }
    if (cls == nullptr) {
      return; // no class slot left: the table is simply not recycled
    }
    const std::size_t word = static_cast<std::size_t>((table.value - base_.value) / 8);
    zero(words);
    words[0] = cls->head;
    cls->head = (static_cast<std::uint64_t>(word) + 1) << 12;
    if (in_use_ != 0) {
      --in_use_;
    }
  }

  /** @brief The CPU's view of a table; empty if `table` is outside the pool or misaligned. */
  [[nodiscard]] reloco::span<std::uint64_t> table(Phys table, std::size_t entry_count, table_location = {}) noexcept {
    const std::uint64_t bytes = static_cast<std::uint64_t>(entry_count) * 8;
    if (table.value < base_.value || !is_pow2(entry_count) || table.value % bytes != 0) {
      return {};
    }
    const std::uint64_t off = table.value - base_.value;
    if (off % 8 != 0 || off / 8 > storage_.size() || storage_.size() - off / 8 < entry_count) {
      return {};
    }
    return storage_.subspan(static_cast<std::size_t>(off / 8), entry_count);
  }

  /** @brief Tables currently handed out. */
  [[nodiscard]] constexpr std::size_t tables_in_use() const noexcept { return in_use_; }
  /** @brief 64-bit words never yet handed out by the bump pointer (free-listed tables not included). */
  [[nodiscard]] constexpr std::size_t words_unallocated() const noexcept { return storage_.size() - next_word_; }

private:
  struct size_class {
    std::size_t entries{0};
    std::uint64_t head{0}; // ((word offset + 1) << 12) of the first free table, 0 = empty
  };

  [[nodiscard]] static constexpr bool is_pow2(std::size_t n) noexcept { return n != 0 && (n & (n - 1)) == 0; }

  static void zero(reloco::span<std::uint64_t> words) noexcept {
    for (std::size_t i = 0; i < words.size(); ++i) {
      words[i] = 0;
    }
  }

  [[nodiscard]] size_class *find_class(std::size_t entries) noexcept {
    for (std::size_t i = 0; i < classes_.size(); ++i) {
      if (classes_[i].entries == entries) {
        return &classes_[i];
      }
    }
    return nullptr;
  }

  [[nodiscard]] size_class *add_class(std::size_t entries) noexcept {
    for (std::size_t i = 0; i < classes_.size(); ++i) {
      if (classes_[i].entries == 0) {
        classes_[i].entries = entries;
        return &classes_[i];
      }
    }
    return nullptr;
  }

  reloco::span<std::uint64_t> storage_{};
  Phys base_{};
  std::size_t next_word_{0};
  std::size_t in_use_{0};
  reloco::array<size_class, MaxSizeClasses> classes_{};
};

} // namespace structo::arch
