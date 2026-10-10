// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file recursive_remapper.hpp
 * @brief `structo::arch::recursive_remapper`: an early (bootstrap) remapper that owns **one last-level
 * page table** which maps *itself* as a page ("page table pointing to itself"), so it can edit its own
 * entries through the MMU without any direct map of physical memory.
 *
 * ## How it works
 *
 * A last-level table covers `entry_count * page_size` bytes of virtual address space (the *window span*,
 * e.g. 2 MiB for 4 KiB pages with 512 entries). One of its entries (`self_index`) maps the table's own
 * physical page. The table is therefore visible at `base_va + self_index * page_size`, and the remapper
 * reads and writes the other entries there. Because the self mapping is an ordinary last-level *leaf*,
 * this works on every architecture (including RISC-V, where a pointer entry at the last level is invalid).
 *
 * ## Responsibilities
 *
 *  - **The caller builds every level above**: the upper-level entries must point at this table so that
 *    `[base_va, base_va + span)` translates through it. The remapper never touches them.
 *  - **The remapper owns the last-level table** and maps pages of the format's granule inside its span.
 *    Larger blocks, promotion and demotion belong to the upper levels the caller manages.
 *  - The slot `self_index` is reserved for the window; mapping it is `invalid_argument`.
 *
 * ## Usage
 *
 * @code
 * using format = structo::arch::x86::recursive_pte_format;  // x86-64 / PAE last level, 512 entries
 * using phys = format::phys_type;
 * using remapper = structo::arch::recursive_remapper<format, my_tlb>;
 *
 * // `table_phys`: physical address of one page that will become the last-level table (page aligned).
 * // `table_view`: how the CPU can write that page *right now* (identity map / early mapping),
 * //               before the new tables are live. Must have format::entry_count words.
 * // `base_va`:    canonical VA where this table's span starts; aligned to the span (2 MiB here).
 * // 3:            slot of the self entry; the window appears at base_va + 3 * 4096.
 * auto m = remapper::try_initialize(table_view, table_phys, 0xFFFF'FFFF'C000'0000, 3);
 *
 * // ... caller links table_phys into its upper-level tables and activates them ...
 *
 * // Map 16 KiB of RAM read/write, non-executable, write-back, at span offset 0x10000.
 * // `map_flags::replace` would overwrite existing entries instead of failing with already_exists.
 * (void)m->try_map(0xFFFF'FFFF'C001'0000, phys{0x20'0000}, 16 << 10, structo::arch::protection::kernel_data());
 *
 * auto q = m->query(0xFFFF'FFFF'C001'1234); // physical address, protection, page size
 * @endcode
 *
 * ## Format contract
 *
 * `Format` (see `x86/`, `arm64/`, `arm/` and `riscv/recursive_format.hpp`) provides: `word` (the entry
 * integer type), `phys_type`, `entry_count`, `page_size`, `flush_on_map`, `is_present(word)`,
 * `frame_addr(word)`, `make_leaf(phys, protection)` (legalizes the protection, may fail), `attrs(word)`
 * and `needs_bbm(old, new)`. Formats legalize the request into the nearest encoding that is never
 * more permissive, apart from the exceptions each format documents.
 *
 * ## Other contracts
 *
 *  - `Window`: `span<word> operator()(va, entry_count)` turning the window VA into a CPU view of the table
 *    (default `identity_window`: the VA is the pointer).
 *  - `Tlb`: static `flush(va, size)` and `complete()` (a barrier making table writes visible to the walker:
 *    `dsb ish; isb` on ARM, `sfence.vma` on RISC-V). See `tlb_binding`.
 *
 * ## Limits
 *
 *  - Break-before-make is used where the format needs it (ARM): clear, flush, barrier, write.
 *  - Not thread safe. On SMP, x86/RISC-V need an IPI shootdown inside the `Tlb` you pass in.
 */

#include <structo/arch/protection.hpp>
#include <structo/arch/tlb_flush.hpp>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch {

/** Flags for `recursive_remapper::try_map`. */
enum class map_flags : std::uint8_t {
  none = 0,
  replace = 1 //!< Overwrite existing mappings instead of failing with `already_exists`.
};
constexpr map_flags operator|(map_flags a, map_flags b) noexcept {
  return static_cast<map_flags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
constexpr bool has_flag(map_flags v, map_flags f) noexcept {
  return (static_cast<std::uint8_t>(v) & static_cast<std::uint8_t>(f)) != 0;
}

/** Result of `query`. */
template <typename Phys> struct mapping_info {
  std::uint64_t virt_base{0}; //!< VA of the start of the page containing the queried address.
  Phys frame{};               //!< Physical address of the start of the page.
  Phys physical{};            //!< Physical address the queried address translates to.
  std::uint64_t size{0};      //!< Page size in bytes.
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
 * @tparam Arch     TLB trait tag, e.g. `x86::tlb_tag`, `arm64::tlb_tag`, `riscv::tlb_tag`.
 * @tparam Space    TLB space whose entries to invalidate (untagged = every tag/ASID).
 * @tparam Barrier  type with static `complete()` (ARM: `dsb ish; isb`; RISC-V: `sfence.vma`).
 * @tparam PageSize bytes invalidated per `flush_page` (the format's granule).
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

/** `Window` that treats the window address as a plain pointer (right once the new tables are live). */
template <typename Word> struct identity_window {
  reloco::span<Word> operator()(std::uint64_t va, std::size_t entry_count) const noexcept {
    return reloco::span<Word>(reinterpret_cast<Word *>(static_cast<std::uintptr_t>(va)), entry_count);
  }
};

template <typename Format, typename Tlb = no_tlb, typename Window = identity_window<typename Format::word>>
class recursive_remapper {
public:
  using word = typename Format::word;
  using phys_type = typename Format::phys_type;
  using info_type = mapping_info<phys_type>;

  static constexpr std::size_t entry_count = Format::entry_count;
  static constexpr std::uint64_t page_size = Format::page_size;
  /** Bytes of virtual address space the owned table covers. */
  static constexpr std::uint64_t span_bytes = static_cast<std::uint64_t>(entry_count) * page_size;

  static_assert(entry_count != 0 && (entry_count & (entry_count - 1)) == 0, "entry_count must be a power of two");
  static_assert(page_size != 0 && (page_size & (page_size - 1)) == 0, "page_size must be a power of two");
  static_assert(entry_count * sizeof(word) <= page_size, "the table must fit in one page to map itself");

  /**
   * @brief Takes ownership of a last-level table: clears it and installs the self entry.
   * @param view       The CPU's current view of the table page (`entry_count` words); used before the window is live.
   * @param table_phys Physical address of the table; must be page aligned.
   * @param base_va    Start of the VA span the table translates; aligned to `span_bytes`.
   * @param self_index Slot that maps the table itself.
   */
  [[nodiscard]] static reloco::result<recursive_remapper> try_initialize(reloco::span<word> view, phys_type table_phys,
                                                                         std::uint64_t base_va,
                                                                         std::size_t self_index,
                                                                         Window window = {}) noexcept {
    if (view.size() != entry_count || self_index >= entry_count || table_phys.value % page_size != 0 ||
        base_va % span_bytes != 0) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    auto self = Format::make_leaf(table_phys, protection{}.with_kernel(kprot::write));
    if (!self) {
      return reloco::unexpected(self.error());
    }
    const word self_entry = *self;
    for (std::size_t i = 0; i < entry_count; ++i) {
      store(view, i, 0);
    }
    store(view, self_index, self_entry);
    return recursive_remapper(table_phys, base_va, self_index, window);
  }

  /** Adopts a table that already holds the self entry at `self_index`. */
  constexpr recursive_remapper(phys_type table_phys, std::uint64_t base_va, std::size_t self_index,
                               Window window = {}) noexcept
      : table_(table_phys), base_(base_va), self_(self_index), window_(window) {}

  /** Physical address of the owned table (link it from the upper level). */
  [[nodiscard]] constexpr phys_type table() const noexcept { return table_; }
  [[nodiscard]] constexpr std::uint64_t base_va() const noexcept { return base_; }
  [[nodiscard]] constexpr std::size_t self_index() const noexcept { return self_; }
  /** VA at which the table itself is visible. */
  [[nodiscard]] constexpr std::uint64_t window_va() const noexcept { return base_ + self_ * page_size; }

  /** Maps `[va, va+size)` to `[pa, pa+size)` with `prot`. Validates everything first, then writes. */
  [[nodiscard]] reloco::result<void> try_map(std::uint64_t va, phys_type pa, std::uint64_t size, protection prot,
                                             map_flags flags = map_flags::none) noexcept {
    std::size_t first = 0;
    std::size_t count = 0;
    if (auto r = check_range(va, size, first, count); !r) {
      return r;
    }
    if (pa.value % page_size != 0) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    const bool replace = has_flag(flags, map_flags::replace);
    // Legality does not depend on the frame except for range limits, so check both ends.
    const auto head = Format::make_leaf(pa, prot);
    if (!head) {
      return reloco::unexpected(head.error());
    }
    const auto tail = Format::make_leaf(phys_type{pa.value + (count - 1) * page_size}, prot);
    if (!tail) {
      return reloco::unexpected(tail.error());
    }
    auto tbl = view();
    if (tbl.empty()) {
      return reloco::unexpected(reloco::error::invalid_state);
    }
    if (!replace) {
      for (std::size_t i = 0; i < count; ++i) {
        if (Format::is_present(load(tbl, first + i))) {
          return reloco::unexpected(reloco::error::already_exists);
        }
      }
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto leaf = Format::make_leaf(phys_type{pa.value + i * page_size}, prot);
      write_entry(tbl, first + i, *leaf);
    }
    Tlb::complete();
    return {};
  }

  /** Unmaps `[va, va+size)`; holes are ignored. */
  [[nodiscard]] reloco::result<void> try_unmap(std::uint64_t va, std::uint64_t size) noexcept {
    std::size_t first = 0;
    std::size_t count = 0;
    if (auto r = check_range(va, size, first, count); !r) {
      return r;
    }
    auto tbl = view();
    if (tbl.empty()) {
      return reloco::unexpected(reloco::error::invalid_state);
    }
    for (std::size_t i = first; i < first + count; ++i) {
      if (Format::is_present(load(tbl, i))) {
        store(tbl, i, 0);
        Tlb::flush(va_of(i), page_size);
      }
    }
    Tlb::complete();
    return {};
  }

  /** Changes the protection of every page in `[va, va+size)`, keeping the frames. `not_found` for holes. */
  [[nodiscard]] reloco::result<void> try_protect(std::uint64_t va, std::uint64_t size, protection prot) noexcept {
    std::size_t first = 0;
    std::size_t count = 0;
    if (auto r = check_range(va, size, first, count); !r) {
      return r;
    }
    auto tbl = view();
    if (tbl.empty()) {
      return reloco::unexpected(reloco::error::invalid_state);
    }
    for (std::size_t i = first; i < first + count; ++i) {
      const word raw = load(tbl, i);
      if (!Format::is_present(raw)) {
        return reloco::unexpected(reloco::error::not_found);
      }
      if (auto r = Format::make_leaf(Format::frame_addr(raw), prot); !r) {
        return reloco::unexpected(r.error());
      }
    }
    for (std::size_t i = first; i < first + count; ++i) {
      const auto leaf = Format::make_leaf(Format::frame_addr(load(tbl, i)), prot);
      write_entry(tbl, i, *leaf);
    }
    Tlb::complete();
    return {};
  }

  /** Describes the page containing `va`; `not_found` if unmapped. */
  [[nodiscard]] reloco::result<info_type> query(std::uint64_t va) const noexcept {
    const std::uint64_t page_va = va & ~(page_size - 1);
    std::size_t first = 0;
    std::size_t count = 0;
    if (auto r = check_range(page_va, page_size, first, count); !r) {
      return reloco::unexpected(r.error());
    }
    auto tbl = view();
    if (tbl.empty()) {
      return reloco::unexpected(reloco::error::invalid_state);
    }
    const word raw = load(tbl, first);
    if (!Format::is_present(raw)) {
      return reloco::unexpected(reloco::error::not_found);
    }
    info_type info;
    info.virt_base = page_va;
    info.frame = Format::frame_addr(raw);
    info.physical = phys_type{info.frame.value + (va - page_va)};
    info.size = page_size;
    info.prot = Format::attrs(raw);
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
  reloco::span<word> view() const noexcept { return window_(window_va(), entry_count); }

  constexpr std::uint64_t va_of(std::size_t index) const noexcept { return base_ + index * page_size; }

  static word load(reloco::span<word> t, std::size_t i) noexcept { return __atomic_load_n(&t[i], __ATOMIC_RELAXED); }
  static void store(reloco::span<word> t, std::size_t i, word v) noexcept {
    __atomic_store_n(&t[i], v, __ATOMIC_RELEASE);
  }

  [[nodiscard]] reloco::result<void> check_range(std::uint64_t va, std::uint64_t size, std::size_t &first,
                                                 std::size_t &count) const noexcept {
    if (size == 0 || va % page_size != 0 || size % page_size != 0 || va < base_ || va - base_ >= span_bytes ||
        size > span_bytes - (va - base_)) {
      return reloco::unexpected(reloco::error::invalid_argument);
    }
    first = static_cast<std::size_t>((va - base_) / page_size);
    count = static_cast<std::size_t>(size / page_size);
    if (self_ >= first && self_ < first + count) {
      return reloco::unexpected(reloco::error::invalid_argument); // reserved for the window
    }
    return {};
  }

  // Stores `nv`, breaking the old mapping first when the format requires it.
  void write_entry(reloco::span<word> tbl, std::size_t i, word nv) noexcept {
    const word old = load(tbl, i);
    if (old == nv) {
      return;
    }
    if (Format::is_present(old)) {
      if (Format::needs_bbm(old, nv)) {
        store(tbl, i, 0);
        Tlb::flush(va_of(i), page_size);
        Tlb::complete();
        store(tbl, i, nv);
      } else {
        store(tbl, i, nv);
        Tlb::flush(va_of(i), page_size);
      }
    } else {
      store(tbl, i, nv);
      if (Format::flush_on_map) {
        Tlb::flush(va_of(i), page_size);
      }
    }
  }

  phys_type table_;
  std::uint64_t base_;
  std::size_t self_;
  Window window_;
};

} // namespace structo::arch
