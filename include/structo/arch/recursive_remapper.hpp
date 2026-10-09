// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file recursive_remapper.hpp
 * @brief `structo::arch::recursive_remapper`: an early (bootstrap) page-table
 * remapper that edits the *live* tables through the "page table pointing to
 * itself" (recursive mapping) trick, so it needs no direct map of physical memory.
 *
 * ## How the trick works
 *
 * One root slot `R` holds a descriptor that points to the root table itself.
 * Walking a virtual address whose first `k` indices are `R` makes the walker
 * "stay" at the root `k` times, so the last `k` levels of the walk land on the
 * *tables themselves* as if they were data pages. A table at tree level `L`
 * (0 = root) is therefore reachable at the address made of `N-L` copies of `R`
 * followed by the indices that lead to it (`N` = number of levels). The whole
 * region under root slot `R` becomes a window onto the page tables, and the
 * remapper can edit any table with plain loads/stores.
 *
 * The consequences, all handled here:
 *  - every table descriptor must also be a valid *page* descriptor (it gets used
 *    as the last-level entry of a window walk): the formats set the page-view
 *    attributes (kernel RW, write-back, no execute) in `make_table`;
 *  - slot `R` of the root is reserved for the window: nothing else can be mapped
 *    under it (`invalid_argument`);
 *  - the self entry is kernel-only and execute-never on x86-64, which makes the
 *    whole window supervisor-only and non-executable;
 *  - each table must be one granule long (`entry_count * 8 == page_size`);
 *  - **RISC-V cannot use the trick**: a pointer PTE at the last level is
 *    invalid. Use a direct map there.
 *
 * ## Usage
 *
 * @code
 * using format = structo::arch::x86::recursive_pte_format<>;  // x86-64, 4-level
 * using remapper = structo::arch::recursive_remapper<format, arena_type, my_tlb, my_window>;
 *
 * // Before enabling (or while still identity-mapped), create a root whose slot 510
 * // points back to itself. The arena is the bootstrap table pool (see page_table_memory.hpp).
 * auto r = remapper::try_create(arena, 510, my_window{});
 *
 * // Load CR3 with r->root(); from now on tables are reachable through the window,
 * // which `my_window` must turn into a CPU pointer (default: the address itself).
 *
 * // Map 4 MiB of RAM, kernel RW write-back, at 0xFFFF'8000'0000'0000. The mapper
 * // picks 2 MiB blocks / 4 KiB pages itself; `no_huge` forbids 1 GiB blocks (CPU without Page1GB).
 * (void)r->try_map(0xFFFF'8000'0000'0000, phys_type{0x20'0000}, 4 << 20,
 *                  structo::arch::protection::kernel_data(), structo::arch::map_flags::no_huge);
 *
 * // Query what is mapped there (physical address, effective protection, block size).
 * auto q = r->query(0xFFFF'8000'0000'1000);
 * @endcode
 *
 * ## Format contract
 *
 * `Format` (see `x86/recursive_format.hpp`, `arm64/recursive_format.hpp`) provides:
 * `levels`, `phys_type`, `is_present(raw)`, `is_leaf(raw, level)` (only asked for
 * levels that allow block leaves), `table_addr(raw)`, `frame_addr(raw, level)`,
 * `make_table(phys)`, `make_self(phys)`, `make_leaf(phys, protection, level)`
 * (legalizes the protection, may fail), `attrs(raw, level)`, `needs_bbm(old, new)`
 * and `canonicalize(low_va)`.
 *
 * ## Other contracts
 *
 *  - `Arena`: `try_allocate_table(entry_count)`, `table(phys, entry_count)` (used while the tables are
 *    still reachable physically), e.g. `table_arena`.
 *  - `Window`: `span<uint64_t> operator()(va, entry_count)` converting a window virtual address into a CPU view
 *    of the table. The default reinterprets the address as a pointer.
 *  - `Tlb`: static `flush(va, size)` (invalidate, may be a no-op) and `complete()` (barrier making
 *    table writes visible to the walker: `dsb ish; isb` on ARM). See `tlb_binding`.
 *
 * ## Limits (it is a bootstrap tool)
 *
 *  - No demotion: `try_unmap` / `try_protect` need ranges aligned to the existing blocks (`invalid_argument` otherwise).
 *  - Empty tables are not reclaimed.
 *  - A failed `try_map` is rolled back; `try_unmap`/`try_protect` validate first and then cannot fail.
 *  - Not thread safe.
 */

#include <structo/arch/protection.hpp>
#include <structo/arch/tlb_flush.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace structo::arch {

/** Flags for `recursive_remapper::try_map`. */
enum class map_flags : std::uint8_t {
  none = 0,
  replace = 1,  //!< Overwrite existing leaf mappings of the same size instead of failing with `already_exists`.
  no_large = 2, //!< Only map last-level pages.
  no_huge = 4   //!< Do not use blocks above the second-to-last level (e.g. 1 GiB; needs a CPU feature on x86).
};
constexpr map_flags operator|(map_flags a, map_flags b) noexcept {
  return static_cast<map_flags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool has_flag(map_flags v, map_flags f) noexcept {
  return (static_cast<std::uint8_t>(v) & static_cast<std::uint8_t>(f)) != 0;
}

/** Result of `query`. */
template <typename Phys> struct mapping_info {
  std::uint64_t virt_base{0}; //!< Canonical VA of the start of the leaf containing the queried address.
  Phys frame{};               //!< Physical address of the start of the leaf.
  Phys physical{};            //!< Physical address the queried address translates to.
  std::uint64_t size{0};      //!< Leaf size in bytes (page or block).
  std::size_t level{0};       //!< Tree level of the leaf (0 = root).
  protection prot{};          //!< Effective (legalized) protection.
};

/** `Tlb` that does nothing (tests, or when the MMU is still off). */
struct no_tlb {
  static void flush(std::uint64_t, std::uint64_t) noexcept {}
  static void complete() noexcept {}
};

/** Barrier policy for `tlb_binding` that does nothing. */
struct no_tlb_barrier {
  static void complete() noexcept {}
};

/**
 * @brief `Tlb` built on `tlb_flusher<Arch>` (see `tlb_flush.hpp`).
 * @tparam Arch    TLB trait tag, e.g. `x86::tlb_tag` or `arm64::tlb_tag`.
 * @tparam Space   TLB space whose entries to invalidate (untagged = every tag/ASID).
 * @tparam Barrier type with static `complete()` (ARM: `dsb ish; isb`).
 * @tparam PageSize bytes invalidated per `flush_page`.
 * Ranges larger than 64 pages fall back to `flush_all`.
 */
template <typename Arch, typename Space = untagged_tlb_space, typename Barrier = no_tlb_barrier,
          std::uint64_t PageSize = 4096>
struct tlb_binding {
  static void flush(std::uint64_t va, std::uint64_t size) noexcept {
    if (size > 64 * PageSize) {
      tlb_flusher<Arch>::template flush_all<Space>();
      return;
    }
    for (std::uint64_t off = 0; off < size; off += PageSize) {
      tlb_flusher<Arch>::template flush_page<Space>(va + off);
    }
  }
  static void complete() noexcept { Barrier::complete(); }
};

/** `Window` that treats the window address as a plain pointer (use once the MMU runs the tables). */
struct identity_window {
  reloco::span<std::uint64_t> operator()(std::uint64_t va, std::size_t entry_count) const noexcept {
    return reloco::span<std::uint64_t>(reinterpret_cast<std::uint64_t *>(static_cast<std::uintptr_t>(va)),
                                       entry_count);
  }
};

namespace detail {
template <typename Levels, std::size_t... I> constexpr bool all_tables_one_page(std::index_sequence<I...>) noexcept {
  return ((Levels::template level<I>::entry_count * 8 == Levels::leaf_page_traits::page_size) && ...);
}
} // namespace detail

template <typename Format, typename Arena, typename Tlb = no_tlb, typename Window = identity_window>
class recursive_remapper {
public:
  using levels = typename Format::levels;
  using phys_type = typename Format::phys_type;
  using info_type = mapping_info<phys_type>;

  static constexpr std::size_t level_count = levels::level_count;
  static constexpr std::size_t last_level = level_count - 1;
  static constexpr std::uint64_t page_size = levels::leaf_page_traits::page_size;

  static_assert(levels::va_bits < 64, "VA must be narrower than 64 bits");
  static_assert(level_count >= 2, "recursion needs at least two levels");
  static_assert(detail::all_tables_one_page<levels>(std::make_index_sequence<level_count>{}),
                "recursive mapping needs every table to be exactly one granule long");

  /** Allocates a root from `arena`, installs the self entry in slot `self_index`. */
  [[nodiscard]] static reloco::result<recursive_remapper> try_create(Arena &arena, std::size_t self_index,
                                                                     Window window = {}) noexcept {
    if (self_index >= entries_at(0)) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    auto root = arena.try_allocate_table(entries_at(0));
    if (!root) {
      return reloco::unexpected(root.error());
    }
    phys_type root_phys = *root;
    auto view = arena.table(root_phys, entries_at(0));
    if (view.empty()) {
      return reloco::unexpected(reloco::error::invalid_state);
    }
    store(view, self_index, Format::make_self(root_phys));
    return recursive_remapper(arena, root_phys, self_index, window);
  }

  /** Adopts an existing root whose slot `self_index` already holds the self entry. */
  constexpr recursive_remapper(Arena &arena, phys_type root, std::size_t self_index, Window window = {}) noexcept
      : arena_(&arena), root_(root), self_(self_index), window_(window) {}

  /** Physical address of the root table (load it into CR3 / TTBRx). */
  [[nodiscard]] constexpr phys_type root() const noexcept { return root_; }
  /** Root slot that points to the root itself. */
  [[nodiscard]] constexpr std::size_t self_index() const noexcept { return self_; }

  /**
   * @brief Writes the self entry into `root_view[self_index]` of a root that is not managed by a remapper yet.
   * @return `already_exists` if the slot is occupied.
   */
  [[nodiscard]] static reloco::result<void> install_self(reloco::span<std::uint64_t> root_view, phys_type root,
                                                         std::size_t self_index) noexcept {
    if (self_index >= entries_at(0) || root_view.size() != entries_at(0)) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    if (load(root_view, self_index) != 0) {
      return reloco::unexpected(reloco::error::already_exists);
    }
    store(root_view, self_index, Format::make_self(root));
    return {};
  }

  /**
   * @brief Canonical virtual address of the window onto the table at `level` that translates `va`
   * (level 0 = root, whatever `va` is).
   */
  [[nodiscard]] constexpr std::uint64_t table_window_va(std::size_t level, std::uint64_t va) const noexcept {
    return window_va(level, va & va_mask());
  }

  /** Maps `[va, va+size)` to `[pa, pa+size)`; uses the largest blocks the alignment allows. */
  [[nodiscard]] reloco::result<void> try_map(std::uint64_t va, phys_type pa, std::uint64_t size, protection prot,
                                             map_flags flags = map_flags::none) noexcept {
    std::uint64_t low = 0;
    if (auto r = check_range(va, size, low); !r) {
      return r;
    }
    if (pa.value % page_size != 0) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    const std::uint64_t end = low + size;
    const bool replace = has_flag(flags, map_flags::replace);

    // Pass 1: validate every chunk (overlap and legality) without touching anything.
    for (std::uint64_t cur = low; cur < end;) {
      const std::size_t t = pick_level(cur, pa.value + (cur - low), end - cur, flags);
      if (auto r = Format::make_leaf(phys_type{(pa.value + (cur - low)) & ~(block_size(t) - 1)}, prot, t); !r) {
        return reloco::unexpected(r.error());
      }
      const slot s = lookup(cur);
      if (s.level > t || (s.present && !(s.level == t && replace))) {
        return reloco::unexpected(reloco::error::already_exists);
      }
      cur += block_size(t);
    }

    // Pass 2: build.
    std::uint64_t cur = low;
    while (cur < end) {
      const std::size_t t = pick_level(cur, pa.value + (cur - low), end - cur, flags);
      auto done = map_chunk(cur, phys_type{pa.value + (cur - low)}, prot, t);
      if (!done) {
        // Out of table memory: undo what this call mapped.
        if (cur > low) {
          unmap_unchecked(low, cur);
        }
        Tlb::complete();
        return done;
      }
      cur += block_size(t);
    }
    Tlb::complete();
    return {};
  }

  /** Unmaps `[va, va+size)`; holes are ignored. Existing blocks must lie entirely inside the range. */
  [[nodiscard]] reloco::result<void> try_unmap(std::uint64_t va, std::uint64_t size) noexcept {
    std::uint64_t low = 0;
    if (auto r = check_range(va, size, low); !r) {
      return r;
    }
    const std::uint64_t end = low + size;
    for (std::uint64_t cur = low; cur < end;) {
      const slot s = lookup(cur);
      const std::uint64_t b = block_size(s.level);
      const std::uint64_t base = cur & ~(b - 1);
      if (s.present && (base < low || base + b > end)) {
        return reloco::unexpected(reloco::error::invalid_argument);
      }
      cur = base + b;
    }
    unmap_unchecked(low, end);
    Tlb::complete();
    return {};
  }

  /** Changes the protection of every mapping in `[va, va+size)`, keeping the frames. `not_found` for holes. */
  [[nodiscard]] reloco::result<void> try_protect(std::uint64_t va, std::uint64_t size, protection prot) noexcept {
    std::uint64_t low = 0;
    if (auto r = check_range(va, size, low); !r) {
      return r;
    }
    const std::uint64_t end = low + size;
    for (std::uint64_t cur = low; cur < end;) {
      const slot s = lookup(cur);
      if (!s.present) {
        return reloco::unexpected(reloco::error::not_found);
      }
      const std::uint64_t b = block_size(s.level);
      const std::uint64_t base = cur & ~(b - 1);
      if (base < low || base + b > end) {
        return reloco::unexpected(reloco::error::invalid_argument);
      }
      if (auto r = Format::make_leaf(frame_of(s), prot, s.level); !r) {
        return reloco::unexpected(r.error());
      }
      cur = base + b;
    }
    for (std::uint64_t cur = low; cur < end;) {
      const slot s = lookup(cur);
      const std::uint64_t b = block_size(s.level);
      const std::uint64_t base = cur & ~(b - 1);
      auto nv = Format::make_leaf(frame_of(s), prot, s.level);
      replace_leaf(s, *nv, base);
      cur = base + b;
    }
    Tlb::complete();
    return {};
  }

  /** Describes the mapping containing `va`; `not_found` if unmapped. */
  [[nodiscard]] reloco::result<info_type> query(std::uint64_t va) const noexcept {
    std::uint64_t low = 0;
    const std::uint64_t page_va = va & ~(page_size - 1);
    if (auto r = check_range(page_va, page_size, low); !r) {
      return reloco::unexpected(r.error());
    }
    low += va - page_va;
    const slot s = lookup(low);
    if (!s.present) {
      return reloco::unexpected(reloco::error::not_found);
    }
    const std::uint64_t b = block_size(s.level);
    info_type info;
    info.virt_base = Format::canonicalize(low & ~(b - 1));
    info.frame = frame_of(s);
    info.physical = phys_type{info.frame.value + (low & (b - 1))};
    info.size = b;
    info.level = s.level;
    info.prot = Format::attrs(s.raw, s.level);
    return info;
  }

  /** Physical address `va` translates to. */
  [[nodiscard]] reloco::result<phys_type> translate(std::uint64_t va) const noexcept {
    auto q = query(va);
    if (!q) {
      return reloco::unexpected(q.error());
    }
    info_type info = *q;
    return info.physical;
  }

private:
  struct slot {
    std::size_t level{0};
    reloco::span<std::uint64_t> table{};
    std::size_t index{0};
    std::uint64_t raw{0};
    bool present{false};
  };

  template <std::size_t... I>
  static constexpr std::size_t shift_impl(std::size_t l, std::index_sequence<I...>) noexcept {
    std::size_t r = 0;
    ((l == I ? (r = levels::template level<I>::shift, 0) : 0), ...);
    return r;
  }
  template <std::size_t... I> static constexpr bool leaf_impl(std::size_t l, std::index_sequence<I...>) noexcept {
    bool r = false;
    ((l == I ? (r = levels::template level<I>::allows_leaf, 0) : 0), ...);
    return r;
  }
  template <std::size_t... I>
  static constexpr std::size_t entries_impl(std::size_t l, std::index_sequence<I...>) noexcept {
    std::size_t r = 0;
    ((l == I ? (r = levels::template level<I>::entry_count, 0) : 0), ...);
    return r;
  }

  static constexpr std::size_t shift_at(std::size_t l) noexcept {
    return shift_impl(l, std::make_index_sequence<level_count>{});
  }
  static constexpr std::size_t entries_at(std::size_t l) noexcept {
    return entries_impl(l, std::make_index_sequence<level_count>{});
  }
  static constexpr bool allows_leaf_at(std::size_t l) noexcept {
    return l == last_level || leaf_impl(l, std::make_index_sequence<level_count>{});
  }
  static constexpr std::uint64_t block_size(std::size_t l) noexcept { return std::uint64_t{1} << shift_at(l); }
  static constexpr std::uint64_t va_mask() noexcept { return (std::uint64_t{1} << levels::va_bits) - 1; }
  static constexpr std::size_t index_at(std::uint64_t low, std::size_t l) noexcept {
    return static_cast<std::size_t>((low >> shift_at(l)) & (entries_at(l) - 1));
  }

  static std::uint64_t load(reloco::span<std::uint64_t> t, std::size_t i) noexcept {
    return __atomic_load_n(&t[i], __ATOMIC_RELAXED);
  }
  static void store(reloco::span<std::uint64_t> t, std::size_t i, std::uint64_t v) noexcept {
    __atomic_store_n(&t[i], v, __ATOMIC_RELEASE);
  }

  constexpr std::uint64_t window_va(std::size_t level, std::uint64_t low) const noexcept {
    const std::size_t copies = level_count - level;
    std::uint64_t va = 0;
    for (std::size_t j = 0; j < level_count; ++j) {
      const std::size_t idx = j < copies ? self_ : index_at(low, j - copies);
      va |= static_cast<std::uint64_t>(idx) << shift_at(j);
    }
    return Format::canonicalize(va);
  }

  reloco::span<std::uint64_t> table_view(std::size_t level, std::uint64_t low) const noexcept {
    return window_(window_va(level, low), entries_at(level));
  }

  [[nodiscard]] reloco::result<void> check_range(std::uint64_t va, std::uint64_t size, std::uint64_t &low) const
      noexcept {
    if (size == 0 || va % page_size != 0 || size % page_size != 0) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    const std::uint64_t last = va + size - 1;
    low = va & va_mask();
    if (last < va || Format::canonicalize(low) != va || Format::canonicalize(last & va_mask()) != last ||
        (last & va_mask()) < low) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    for (std::uint64_t cur = low; cur <= (last & va_mask());) {
      if (index_at(cur, 0) == self_) {
        return reloco::unexpected(reloco::error::invalid_argument); // reserved for the window
      }
      const std::uint64_t next = (cur | (block_size(0) - 1)) + 1;
      if (next <= cur) {
        break;
      }
      cur = next;
    }
    return {};
  }

  // Descends to the leaf covering `low`, or to the first non-present entry.
  slot lookup(std::uint64_t low) const noexcept {
    slot s;
    for (std::size_t l = 0; l < level_count; ++l) {
      s.level = l;
      s.table = table_view(l, low);
      s.index = index_at(low, l);
      s.raw = load(s.table, s.index);
      s.present = Format::is_present(s.raw);
      if (!s.present || l == last_level || (allows_leaf_at(l) && Format::is_leaf(s.raw, l))) {
        return s;
      }
    }
    return s;
  }

  static phys_type frame_of(const slot &s) noexcept {
    return phys_type{Format::frame_addr(s.raw, s.level).value & ~(block_size(s.level) - 1)};
  }

  static std::size_t pick_level(std::uint64_t low, std::uint64_t pa, std::uint64_t remaining, map_flags f) noexcept {
    for (std::size_t l = 0; l < last_level; ++l) {
      const std::uint64_t b = block_size(l);
      if (!allows_leaf_at(l) || has_flag(f, map_flags::no_large) ||
          (has_flag(f, map_flags::no_huge) && l + 2 < level_count)) {
        continue;
      }
      if (remaining >= b && (low & (b - 1)) == 0 && (pa & (b - 1)) == 0) {
        return l;
      }
    }
    return last_level;
  }

  reloco::result<void> map_chunk(std::uint64_t low, phys_type pa, protection prot, std::size_t t) noexcept {
    for (std::size_t l = 0; l < t; ++l) {
      auto tbl = table_view(l, low);
      const std::size_t idx = index_at(low, l);
      if (!Format::is_present(load(tbl, idx))) {
        auto fresh = arena_->try_allocate_table(entries_at(l + 1));
        if (!fresh) {
          return reloco::unexpected(fresh.error());
        }
        store(tbl, idx, Format::make_table(*fresh));
        Tlb::complete(); // the new table must be visible before the next level is accessed through the window
      }
    }
    const slot s = lookup_to(low, t);
    auto nv = Format::make_leaf(phys_type{pa.value & ~(block_size(t) - 1)}, prot, t);
    if (!nv) {
      return reloco::unexpected(nv.error());
    }
    replace_leaf(s, *nv, low & ~(block_size(t) - 1));
    return {};
  }

  slot lookup_to(std::uint64_t low, std::size_t t) const noexcept {
    slot s;
    s.level = t;
    s.table = table_view(t, low);
    s.index = index_at(low, t);
    s.raw = load(s.table, s.index);
    s.present = Format::is_present(s.raw);
    return s;
  }

  // Writes a leaf into `s`, breaking the old mapping first when the format requires it.
  void replace_leaf(const slot &s, std::uint64_t nv, std::uint64_t base_low) noexcept {
    if (s.present) {
      if (Format::needs_bbm(s.raw, nv)) {
        store(s.table, s.index, 0);
        Tlb::flush(Format::canonicalize(base_low), block_size(s.level));
        Tlb::complete();
        store(s.table, s.index, nv);
      } else {
        store(s.table, s.index, nv);
        Tlb::flush(Format::canonicalize(base_low), block_size(s.level));
      }
    } else {
      store(s.table, s.index, nv);
    }
  }

  void unmap_unchecked(std::uint64_t low, std::uint64_t end) noexcept {
    for (std::uint64_t cur = low; cur < end;) {
      const slot s = lookup(cur);
      const std::uint64_t b = block_size(s.level);
      const std::uint64_t base = cur & ~(b - 1);
      if (s.present) {
        store(s.table, s.index, 0);
        Tlb::flush(Format::canonicalize(base), b);
      }
      cur = base + b;
    }
  }

  Arena *arena_;
  phys_type root_;
  std::size_t self_;
  Window window_;
};

} // namespace structo::arch
