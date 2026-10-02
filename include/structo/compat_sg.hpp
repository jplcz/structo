// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

#include "phys_addr.hpp"
#include "phys_page.hpp"
#include "sg_list.hpp"
#include "sg_translator.hpp"
#include <algorithm>
#include <reloco/lifetime.hpp>
#include <reloco/packed_bits.hpp>
#include <reloco/speculation_defense.hpp>

namespace structo {

using namespace reloco;

/**
 * @brief `Layout::length_field` stores a literal byte count (the default,
 * matching every pre-existing `sg_descriptor_layout`/`chained_sg_layout`).
 */
struct length_unit_bytes {};

/**
 * @brief `Layout::length_field` stores a count of whole `PageTraits::page_size`
 * pages rather than bytes (e.g. a hardware PFN + page-count descriptor).
 * Only meaningful alongside `OffsetField = void` (such descriptors are always
 * page-aligned; see `compact_sg_codec`).
 */
struct length_unit_pages {};

/**
 * @brief Defines the hardware bitfield layout for a compact SG descriptor.
 * Fields can be set to `void` to disable them (e.g. for pure PFN page sharing).
 */
template <typename StorageType, typename PfnField, typename OffsetField = void, typename LengthField = void,
          typename LastFlagField = void, size_t HeaderSize = 0, typename LengthUnit = length_unit_bytes>
struct sg_descriptor_layout {
  using storage_type = StorageType;
  using pfn_field = PfnField;
  using offset_field = OffsetField;
  using length_field = LengthField;
  using last_flag = LastFlagField;
  using length_unit = LengthUnit;
  static constexpr size_t header_size = HeaderSize;
};

/**
 * @brief Defines the hardware bitfield layout for a chained SG descriptor.
 */
template <typename StorageType, typename PfnField, typename OffsetField = void, typename LengthField = void,
          typename LastFlagField = void, typename ChainFlagField = void, size_t HeaderSize = 0>
struct chained_sg_layout {
  using storage_type = StorageType;
  using pfn_field = PfnField;
  using offset_field = OffsetField;
  using length_field = LengthField;
  using last_flag = LastFlagField;
  using chain_flag = ChainFlagField;
  static constexpr size_t header_size = HeaderSize;
};

/**
 * @brief Encodes and decodes between plain sg_list and bit-packed hardware descriptors.
 *
 * @tparam Layout The hardware descriptor layout (sg_descriptor_layout).
 * @tparam PageTraits The system page size configuration (e.g., page_4k).
 * @tparam SpaceTag The physical address domain (e.g., dma_bus_space).
 */
template <typename Layout, typename PageTraits, typename SpaceTag = dma_bus_space> class compact_sg_codec {
public:
  using storage_type = typename Layout::storage_type;
  using packed_type = packed_bits<storage_type>;
  using entry_type = sg_entry<SpaceTag, uint64_t>;

  static constexpr size_t header_elements = Layout::header_size / sizeof(packed_type);

  static_assert(std::is_trivially_copyable_v<packed_type> && std::is_standard_layout_v<packed_type>,
                "packed_bits must remain trivially copyable for DMA memory arrays");
  static_assert(Layout::header_size % sizeof(packed_type) == 0,
                "Header size must be a multiple of the descriptor storage size");

  /**
   * @brief Encodes an abstract SGL into an array of bit-packed hardware descriptors.
   * Handles page-boundary fragmentation and hardware length limits automatically.
   */
  template <typename InIterable, typename OutContainer>
  [[nodiscard]] static RELOCO_CONSTEXPR20 result<void> encode(const InIterable &input, OutContainer &output) noexcept {
    if (input.empty())
      return {};

    // If a header is required and the output is empty, reserve space for it.
    if constexpr (header_elements > 0) {
      if (output.empty()) {
        for (size_t i = 0; i < header_elements; ++i) {
          auto res = output.try_push_back(packed_type{0});
          if (!res)
            RELOCO_UNLIKELY
          return unexpected(res.error());
        }
      }
    }

    for (const auto &entry : input) {
      auto paddr = entry.addr;
      auto remaining = entry.length;

      while (remaining > 0) {
        uint64_t pfn_val = paddr.value >> PageTraits::page_shift;
        uint64_t offset = paddr.value & PageTraits::alignment_mask;
        uint64_t chunk = 0;

        // Enforce strict page alignment if Offset is disabled
        if constexpr (std::is_void_v<typename Layout::offset_field>) {
          if (offset != 0)
            RELOCO_UNLIKELY
          return unexpected(error::invalid_argument);
        }

        // Enforce full-page chunking if Length is disabled
        if constexpr (std::is_void_v<typename Layout::length_field>) {
          if (remaining < PageTraits::page_size)
            RELOCO_UNLIKELY
          return unexpected(error::invalid_argument);
          chunk = PageTraits::page_size;
        } else if constexpr (std::is_same_v<typename Layout::length_unit, length_unit_pages>) {
          // The length field counts whole pages, not bytes (e.g. a PFN +
          // page-count descriptor); every chunk must therefore itself be a
          // whole number of pages, same as the Length-disabled case above.
          if (remaining < PageTraits::page_size)
            RELOCO_UNLIKELY
          return unexpected(error::invalid_argument);
          constexpr uint64_t max_hw_pages = detail::generate_mask<Layout::length_field::bits, uint64_t>();
          uint64_t pages_remaining = static_cast<uint64_t>(remaining) / PageTraits::page_size;
          uint64_t pages_chunk = std::min(pages_remaining, max_hw_pages);
          chunk = pages_chunk * PageTraits::page_size;
        } else {
          constexpr uint64_t max_hw_len = detail::generate_mask<Layout::length_field::bits, uint64_t>();
          uint64_t page_remaining = PageTraits::page_size - offset;
          chunk = std::min({static_cast<uint64_t>(remaining), max_hw_len, page_remaining});
        }

        // Build descriptor
        packed_type desc;
        desc.template truncating_set<typename Layout::pfn_field>(pfn_val);

        if constexpr (!std::is_void_v<typename Layout::offset_field>) {
          desc.template truncating_set<typename Layout::offset_field>(offset);
        }
        if constexpr (!std::is_void_v<typename Layout::length_field>) {
          if constexpr (std::is_same_v<typename Layout::length_unit, length_unit_pages>) {
            desc.template truncating_set<typename Layout::length_field>(chunk / PageTraits::page_size);
          } else {
            desc.template truncating_set<typename Layout::length_field>(chunk);
          }
        }
        if constexpr (!std::is_void_v<typename Layout::last_flag>) {
          desc.template truncating_set<typename Layout::last_flag>(0);
        }

        auto res = output.try_push_back(desc);
        if (!res)
          RELOCO_UNLIKELY
        return unexpected(res.error());

        paddr.value += chunk;
        remaining -= chunk;
      }
    }

    if constexpr (!std::is_void_v<typename Layout::last_flag>) {
      if (output.size() > header_elements) {
        output.back().template saturating_set<typename Layout::last_flag>(1);
      }
    }
    return {};
  }

  template <typename OutContainer>
  [[nodiscard]] static RELOCO_CONSTEXPR20 result<void> decode(span<const packed_type> input,
                                                              sg_list<OutContainer> &output) noexcept {
    if (input.size() < header_elements)
      RELOCO_UNLIKELY
    return unexpected(error::invalid_argument);

    // Skip the header portion of the layout before decoding.
    for (size_t i = header_elements; i < input.size(); ++i) {
      // TOCTOU mitigation: always copy before reading
      const auto desc = input[i];

      uint64_t pfn_val = desc.template get<typename Layout::pfn_field>();

      uint64_t offset = 0;
      if constexpr (!std::is_void_v<typename Layout::offset_field>) {
        offset = desc.template get<typename Layout::offset_field>();
      }

      uint64_t length = PageTraits::page_size;
      if constexpr (!std::is_void_v<typename Layout::length_field>) {
        length = desc.template get<typename Layout::length_field>();
        if constexpr (std::is_same_v<typename Layout::length_unit, length_unit_pages>) {
          // The length field counts whole pages, not bytes; convert before
          // the bounds check below (which operates on byte lengths).
          length *= PageTraits::page_size;
        }
      }

      // --- Speculation Defenses ---
      // Evaluate all bounds in a single bitwise boolean expression.
      // A page-count length field (no Offset field, always page-aligned) can
      // legitimately span many pages, so the intra-page `offset + length`
      // bound below only applies to a byte-unit length field.
      bool is_safe = true;
      if constexpr (!std::is_same_v<typename Layout::length_unit, length_unit_pages>) {
        is_safe &= (offset < PageTraits::page_size) & ((offset + length) <= PageTraits::page_size);
      }

      if constexpr (!std::is_void_v<typename Layout::length_field>) {
        is_safe &= (length > 0);
      }

      // Architecturally mask the data BEFORE branching.
      // Speculative execution passing this line uses clamped, safe values
      offset = nospec::sanitize(offset, is_safe, UINT64_C(0));
      length = nospec::sanitize(length, is_safe, UINT64_C(0));

      if (!is_safe)
        RELOCO_UNLIKELY { return unexpected(error::security_violation); }

      phys_addr<void, SpaceTag, uint64_t> paddr{(pfn_val << PageTraits::page_shift) | offset};

      auto res = output.try_push_back(paddr, length);
      if (!res)
        RELOCO_UNLIKELY
      return unexpected(res.error());

      if constexpr (!std::is_void_v<typename Layout::last_flag>) {
        if (desc.template get<typename Layout::last_flag>())
          RELOCO_UNLIKELY
        break;
      }
    }
    return {};
  }
};

/**
 * @brief Encodes and decodes chained SG lists in physical memory.
 *
 * @tparam Layout The chained hardware descriptor layout (chained_sg_layout).
 * @tparam PageTraits The system page size configuration (e.g., page_4k).
 * @tparam SpaceTag The physical address domain (e.g., dma_bus_space).
 */
template <typename Layout, typename PageTraits, typename SpaceTag = dma_bus_space> class chained_sg_codec {
public:
  using storage_type = typename Layout::storage_type;
  using packed_type = packed_bits<storage_type>;
  using paddr_type = phys_addr<void, SpaceTag, uint64_t>;

  static constexpr size_t header_elements = Layout::header_size / sizeof(packed_type);
  static constexpr size_t entries_per_page = (PageTraits::page_size - Layout::header_size) / sizeof(packed_type);

  static_assert(std::is_trivially_copyable_v<packed_type> && std::is_standard_layout_v<packed_type>,
                "packed_bits must remain trivially copyable for DMA memory arrays");
  static_assert(Layout::header_size % sizeof(packed_type) == 0,
                "Header size must be a multiple of the descriptor storage size");
  static_assert(entries_per_page > 1, "Page size must hold at least two descriptors to form a chain");

  /**
   * @brief Encodes an abstract SGL into a chain of physical memory pages.
   *
   * @param input The plain SGL to encode.
   * @param alloc Callable `result<paddr_type>()`: Allocates a zeroed physical page.
   * @param mapper Callable `result<packed_type*>(paddr_type)`: Maps a page for CPU access.
   * @param allocated_pages Output container populated with every physical page allocated during encoding.
   * @return The physical address of the first page in the chain.
   */
  template <typename InIterable, typename Allocator, typename Mapper, typename OutPageContainer>
  [[nodiscard]] static RELOCO_CONSTEXPR20 result<paddr_type>
  encode(const InIterable &input, Allocator &&alloc, Mapper &&mapper, OutPageContainer &allocated_pages) noexcept {
    // Descriptor pages are raw hardware/DMA memory returned by `mapper()` as
    // `packed_type *`, not a bounds-checked span: the safety invariant here
    // is `entries_per_page`/`header_elements`, enforced manually below,
    // rather than anything the type system can prove.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    if (input.empty())
      return paddr_type{nullptr};

    auto root_page_res = alloc();
    if (!root_page_res)
      RELOCO_UNLIKELY
    return unexpected(root_page_res.error());

    // Transfer ownership of the allocated page to the caller.
    auto push_res = allocated_pages.try_push_back(*root_page_res);
    if (!push_res)
      RELOCO_UNLIKELY
    return unexpected(push_res.error());

    paddr_type current_page_paddr = *root_page_res;
    auto map_res = mapper(current_page_paddr);
    if (!map_res)
      RELOCO_UNLIKELY
    return unexpected(map_res.error());

    // Advance pointer past the header
    packed_type *current_page_vaddr = (*map_res) + header_elements;

    // Prefetch the first cache line of the newly mapped page
#if defined(__has_builtin) && __has_builtin(__builtin_prefetch)
    // rw = 0 -  prepare the prefetch for a read
    // locality = 3 - (default): High, L1 cache, leave the data in the L1, L2, and L3 cache levels after the access.
    __builtin_prefetch(current_page_vaddr, 0, 3);
#endif

    size_t current_idx = 0;

    for (const auto &entry : input) {
      auto paddr = entry.addr;
      auto remaining = entry.length;

      while (remaining > 0) {
        // Chain Link Generation
        // If we only have 1 slot left in the current page, it MUST be used for the chain link.
        if (current_idx == entries_per_page - 1) {
          auto next_page_res = alloc();
          if (!next_page_res)
            RELOCO_UNLIKELY
          return unexpected(next_page_res.error());

          auto push_res_next = allocated_pages.try_push_back(*next_page_res);
          if (!push_res_next)
            RELOCO_UNLIKELY
          return unexpected(push_res_next.error());

          uint64_t next_pfn = next_page_res->value >> PageTraits::page_shift;

          current_page_vaddr[current_idx].template truncating_set<typename Layout::pfn_field>(next_pfn);

          if constexpr (!std::is_void_v<typename Layout::chain_flag>) {
            current_page_vaddr[current_idx].template truncating_set<typename Layout::chain_flag>(1);
          }
          if constexpr (!std::is_void_v<typename Layout::last_flag>) {
            current_page_vaddr[current_idx].template truncating_set<typename Layout::last_flag>(0);
          }

          // Map the new page and reset the index
          current_page_paddr = *next_page_res;
          map_res = mapper(current_page_paddr);
          if (!map_res)
            RELOCO_UNLIKELY
          return unexpected(map_res.error());
          current_page_vaddr = (*map_res) + header_elements;
          current_idx = 0;
        }

        // Data Descriptor Evaluation
        uint64_t pfn_val = paddr.value >> PageTraits::page_shift;
        uint64_t offset = paddr.value & PageTraits::alignment_mask;
        uint64_t chunk = 0;

        if constexpr (std::is_void_v<typename Layout::offset_field>) {
          if (offset != 0)
            RELOCO_UNLIKELY
          return unexpected(error::invalid_argument);
        }

        if constexpr (std::is_void_v<typename Layout::length_field>) {
          if (remaining < PageTraits::page_size)
            RELOCO_UNLIKELY
          return unexpected(error::invalid_argument);
          chunk = PageTraits::page_size;
        } else {
          constexpr uint64_t max_hw_len = detail::generate_mask<Layout::length_field::bits, uint64_t>();
          uint64_t page_remaining = PageTraits::page_size - offset;
          chunk = std::min({static_cast<uint64_t>(remaining), max_hw_len, page_remaining});
        }

        // Write Data Descriptor
        current_page_vaddr[current_idx].template truncating_set<typename Layout::pfn_field>(pfn_val);

        if constexpr (!std::is_void_v<typename Layout::offset_field>) {
          current_page_vaddr[current_idx].template truncating_set<typename Layout::offset_field>(offset);
        }
        if constexpr (!std::is_void_v<typename Layout::length_field>) {
          current_page_vaddr[current_idx].template truncating_set<typename Layout::length_field>(chunk);
        }
        if constexpr (!std::is_void_v<typename Layout::chain_flag>) {
          current_page_vaddr[current_idx].template truncating_set<typename Layout::chain_flag>(0);
        }
        if constexpr (!std::is_void_v<typename Layout::last_flag>) {
          current_page_vaddr[current_idx].template truncating_set<typename Layout::last_flag>(0);
        }

        paddr.value += chunk;
        remaining -= chunk;
        current_idx++;
      }
    }

    // Terminate the chain on the final descriptor
    if constexpr (!std::is_void_v<typename Layout::last_flag>) {
      if (current_idx > 0) {
        current_page_vaddr[current_idx - 1].template saturating_set<typename Layout::last_flag>(1);
      }
    }

    return *root_page_res;
    RELOCO_END_UNSAFE_BUFFER_USAGE
  }

  /**
   * @brief Decodes a hardware physical descriptor chain back into a plain sg_list.
   *
   * @param root_page The physical address of the first descriptor page.
   * @param output The sg_list to populate.
   * @param mapper Callable `result<const packed_type*>(paddr_type)`: Maps a page for CPU read access.
   * @param max_pages The maximum number of pages to traverse (Mitigates cyclic-chain DoS attacks).
   */
  template <typename OutContainer, typename Mapper>
  [[nodiscard]] static RELOCO_CONSTEXPR20 result<void> decode(paddr_type root_page, sg_list<OutContainer> &output,
                                                              Mapper &&mapper, size_t max_pages) noexcept {
    // See the `encode()` note above: descriptor pages are raw hardware/DMA
    // memory, bounds-enforced manually via `entries_per_page`.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    if (root_page.is_null())
      RELOCO_UNLIKELY
    return {};

    paddr_type current_page_paddr = root_page;
    size_t pages_processed = 0;

    while (true) {
      // --- Cyclic Chain Defense ---
      if (++pages_processed > max_pages)
        RELOCO_UNLIKELY { return unexpected(error::security_violation); }

      auto map_res = mapper(current_page_paddr);
      if (!map_res)
        RELOCO_UNLIKELY
      return unexpected(map_res.error());
      const packed_type *current_page_vaddr = (*map_res) + header_elements;

#if defined(__has_builtin) && __has_builtin(__builtin_prefetch)
      // rw = 0 -  prepare the prefetch for a read
      // locality = 3 - (default): High, L1 cache, leave the data in the L1, L2, and L3 cache levels after the access.
      __builtin_prefetch(current_page_vaddr, 0, 3);
#endif

      for (size_t i = 0; i < entries_per_page; ++i) {
        // TOCTOU mitigation: always copy before reading
        const auto desc = current_page_vaddr[i];

        bool is_chain = false;
        if constexpr (!std::is_void_v<typename Layout::chain_flag>) {
          is_chain = desc.template get<typename Layout::chain_flag>() != 0;
        } else {
          is_chain = (i == entries_per_page - 1);
        }

        uint64_t pfn_val = desc.template get<typename Layout::pfn_field>();

        // A malicious guest might provide PFN 0 as a chain link to trigger null-derefs
        if (is_chain)
          RELOCO_UNLIKELY {
            if (pfn_val == 0)
              RELOCO_UNLIKELY
            return unexpected(error::security_violation);

            current_page_paddr = paddr_type{pfn_val << PageTraits::page_shift};
            break; // Map the next page in the outer loop
          }

        uint64_t offset = 0;
        if constexpr (!std::is_void_v<typename Layout::offset_field>) {
          offset = desc.template get<typename Layout::offset_field>();
        }

        uint64_t length = PageTraits::page_size;
        if constexpr (!std::is_void_v<typename Layout::length_field>) {
          length = desc.template get<typename Layout::length_field>();
        }

        // --- Speculation Defenses ---
        bool is_safe = (offset < PageTraits::page_size) & ((offset + length) <= PageTraits::page_size);

        if constexpr (!std::is_void_v<typename Layout::length_field>) {
          is_safe &= (length > 0);
        }

        offset = nospec::sanitize(offset, is_safe, UINT64_C(0));
        length = nospec::sanitize(length, is_safe, UINT64_C(0));

        if (!is_safe)
          RELOCO_UNLIKELY { return unexpected(error::security_violation); }

        paddr_type paddr{(pfn_val << PageTraits::page_shift) | offset};

        // If the guest provides a massive (but valid) non-cyclic chain,
        // try_push_back will eventually fail if output exceeds its container capacity,
        // safely returning the error instead of crashing
        auto push_res = output.try_push_back(paddr, length);
        if (!push_res)
          RELOCO_UNLIKELY
        return unexpected(push_res.error());

        if constexpr (!std::is_void_v<typename Layout::last_flag>) {
          if (desc.template get<typename Layout::last_flag>())
            RELOCO_UNLIKELY { return {}; }
        }
      }
    }
    RELOCO_END_UNSAFE_BUFFER_USAGE
  }
};

/**
 * @brief Encodes and decodes a 2-level hierarchical SG list.
 * The Level 1 (Root) table contains descriptors that point to Level 2 (Leaf) pages.
 * The Level 2 pages contain the actual data descriptors mapping physical memory.
 *
 * @tparam L1Layout Layout of the Root Table descriptors (often just a PFN).
 * @tparam L2Layout Layout of the Leaf Page data descriptors.
 * @tparam PageTraits The system page size configuration.
 * @tparam SpaceTag The physical address domain.
 * @tparam PhysInt Type of physical address
 */
template <typename L1Layout, typename L2Layout, typename PageTraits, typename SpaceTag = dma_bus_space,
          typename PhysInt = std::uint64_t>
class two_level_sg_codec {
public:
  using l1_storage_type = typename L1Layout::storage_type;
  using l2_storage_type = typename L2Layout::storage_type;

  using l1_packed_type = packed_bits<l1_storage_type>;
  using l2_packed_type = packed_bits<l2_storage_type>;

  using paddr_type = phys_addr<void, SpaceTag, PhysInt>;

  static constexpr size_t l1_header_elements = L1Layout::header_size / sizeof(l1_packed_type);
  static constexpr size_t l2_header_elements = L2Layout::header_size / sizeof(l2_packed_type);

  static constexpr size_t l1_entries_per_page =
      (PageTraits::page_size - L1Layout::header_size) / sizeof(l1_packed_type);
  static constexpr size_t l2_entries_per_page =
      (PageTraits::page_size - L2Layout::header_size) / sizeof(l2_packed_type);

  static_assert(std::is_trivially_copyable_v<l1_packed_type> && std::is_trivially_copyable_v<l2_packed_type> &&
                    std::is_standard_layout_v<l1_packed_type> && std::is_standard_layout_v<l2_packed_type>,
                "packed_bits must remain trivially copyable for DMA memory arrays");
  static_assert(L1Layout::header_size % sizeof(l1_packed_type) == 0 &&
                    L2Layout::header_size % sizeof(l2_packed_type) == 0,
                "Header sizes must be a multiple of the respective descriptor storage size");
  static_assert(l1_entries_per_page > 0 && l2_entries_per_page > 0,
                "Page size is too small to accommodate the header and at least one descriptor");

  /**
   * @brief Encodes an abstract SGL into a 2-level physical memory structure.
   *
   * @param input The plain SGL to encode.
   * @param alloc Allocates zeroed physical pages (used for both L1 and L2).
   * @param mapper Maps a physical page to a void* for CPU write access.
   * @param allocated_pages Output container populated with every physical page allocated during encoding.
   * @return The physical address of the L1 Root Page.
   */
  template <typename InIterable, typename Allocator, typename Mapper, typename OutPageContainer>
  [[nodiscard]] static RELOCO_CONSTEXPR20 result<paddr_type>
  encode(const InIterable &input, Allocator &&alloc, Mapper &&mapper, OutPageContainer &allocated_pages) noexcept {
    // See `chained_sg_codec::encode()`: L1/L2 pages are raw hardware/DMA
    // memory, bounds-enforced manually via `l1_/l2_entries_per_page`.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    if (input.empty())
      return paddr_type{nullptr};

    // Allocate and map the L1 Root Table
    auto l1_page_res = alloc();
    if (!l1_page_res)
      RELOCO_UNLIKELY
    return unexpected(l1_page_res.error());

    auto push_res1 = allocated_pages.try_push_back(*l1_page_res);
    if (!push_res1)
      RELOCO_UNLIKELY
    return unexpected(push_res1.error());

    paddr_type l1_paddr = *l1_page_res;
    auto l1_map_res = mapper(l1_paddr);
    if (!l1_map_res)
      RELOCO_UNLIKELY
    return unexpected(l1_map_res.error());

    auto *l1_vaddr = static_cast<l1_packed_type *>(*l1_map_res) + l1_header_elements;

    size_t l1_idx = 0;
    size_t l2_idx = 0;

    paddr_type current_l2_paddr{nullptr};
    l2_packed_type *current_l2_vaddr = nullptr;

    for (const auto &entry : input) {
      auto paddr = entry.addr;
      auto remaining = entry.length;

      while (remaining > 0) {
        // L2 Page Generation & L1 Linkage
        if (current_l2_vaddr == nullptr || l2_idx == l2_entries_per_page) {
          if (l1_idx == l1_entries_per_page)
            RELOCO_UNLIKELY { return unexpected(error::out_of_range); }

          auto l2_page_res = alloc();
          if (!l2_page_res)
            RELOCO_UNLIKELY
          return unexpected(l2_page_res.error());

          auto push_res2 = allocated_pages.try_push_back(*l2_page_res);
          if (!push_res2)
            RELOCO_UNLIKELY
          return unexpected(push_res2.error());

          current_l2_paddr = *l2_page_res;

          auto l2_map_res = mapper(current_l2_paddr);
          if (!l2_map_res)
            RELOCO_UNLIKELY
          return unexpected(l2_map_res.error());

          current_l2_vaddr = static_cast<l2_packed_type *>(*l2_map_res) + l2_header_elements;

          // Write L1 Entry pointing to the new L2 page
          l1_vaddr[l1_idx].template truncating_set<typename L1Layout::pfn_field>(current_l2_paddr.value >>
                                                                                 PageTraits::page_shift);

          if constexpr (!std::is_void_v<typename L1Layout::last_flag>) {
            l1_vaddr[l1_idx].template truncating_set<typename L1Layout::last_flag>(0);
          }
          if constexpr (!std::is_void_v<typename L1Layout::offset_field>) {
            l1_vaddr[l1_idx].template truncating_set<typename L1Layout::offset_field>(0);
          }
          if constexpr (!std::is_void_v<typename L1Layout::length_field>) {
            l1_vaddr[l1_idx].template truncating_set<typename L1Layout::length_field>(PageTraits::page_size);
          }

          l1_idx++;
          l2_idx = 0;
        }

        // Data Descriptor Evaluation
        uint64_t pfn_val = paddr.value >> PageTraits::page_shift;
        uint64_t offset = paddr.value & PageTraits::alignment_mask;
        uint64_t chunk = 0;

        if constexpr (std::is_void_v<typename L2Layout::offset_field>) {
          if (offset != 0)
            RELOCO_UNLIKELY
          return unexpected(error::invalid_argument);
        }

        if constexpr (std::is_void_v<typename L2Layout::length_field>) {
          if (remaining < PageTraits::page_size)
            RELOCO_UNLIKELY
          return unexpected(error::invalid_argument);
          chunk = PageTraits::page_size;
        } else {
          constexpr uint64_t max_hw_len = detail::generate_mask<L2Layout::length_field::bits, uint64_t>();
          uint64_t page_remaining = PageTraits::page_size - offset;
          chunk = std::min({static_cast<uint64_t>(remaining), max_hw_len, page_remaining});
        }

        // Write L2 Data Descriptor
        current_l2_vaddr[l2_idx].template truncating_set<typename L2Layout::pfn_field>(pfn_val);

        if constexpr (!std::is_void_v<typename L2Layout::offset_field>) {
          current_l2_vaddr[l2_idx].template truncating_set<typename L2Layout::offset_field>(offset);
        }
        if constexpr (!std::is_void_v<typename L2Layout::length_field>) {
          current_l2_vaddr[l2_idx].template truncating_set<typename L2Layout::length_field>(chunk);
        }
        if constexpr (!std::is_void_v<typename L2Layout::last_flag>) {
          current_l2_vaddr[l2_idx].template truncating_set<typename L2Layout::last_flag>(0);
        }

        paddr.value += chunk;
        remaining -= chunk;
        l2_idx++;
      }
    }

    // Terminate the Structures
    if constexpr (!std::is_void_v<typename L1Layout::last_flag>) {
      if (l1_idx > 0)
        l1_vaddr[l1_idx - 1].template saturating_set<typename L1Layout::last_flag>(1);
    }
    if constexpr (!std::is_void_v<typename L2Layout::last_flag>) {
      if (l2_idx > 0)
        current_l2_vaddr[l2_idx - 1].template saturating_set<typename L2Layout::last_flag>(1);
    }

    return l1_paddr;
    RELOCO_END_UNSAFE_BUFFER_USAGE
  }

  /**
   * @brief Decodes a 2-level hardware physical descriptor structure back into a plain sg_list.
   *
   * @param root_page The physical address of the L1 Root Page.
   * @param output The sg_list to populate.
   * @param mapper Maps a physical page to a void* for CPU read access.
   * @param l1_entry_limit The maximum number of L1 entries to process (mitigates infinite loops / out-of-bounds).
   */
  template <typename OutContainer, typename Mapper>
  [[nodiscard]] static RELOCO_CONSTEXPR20 result<void> decode(paddr_type root_page, sg_list<OutContainer> &output,
                                                              Mapper &&mapper, size_t l1_entry_limit) noexcept {
    // See `chained_sg_codec::decode()`: L1/L2 pages are raw hardware/DMA
    // memory, bounds-enforced manually via `l1_/l2_entries_per_page`.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    if (root_page.is_null())
      RELOCO_UNLIKELY
    return {};

    auto l1_map_res = mapper(root_page);
    if (!l1_map_res)
      RELOCO_UNLIKELY
    return unexpected(l1_map_res.error());

    const auto *l1_vaddr = static_cast<const l1_packed_type *>(*l1_map_res) + l1_header_elements;

#if defined(__has_builtin) && __has_builtin(__builtin_prefetch)
    // rw = 0 -  prepare the prefetch for a read
    // locality = 3 - (default): High, L1 cache, leave the data in the L1, L2, and L3 cache levels after the access.
    __builtin_prefetch(l1_vaddr, 0, 3);
#endif

    const size_t actual_l1_limit = std::min(l1_entry_limit, l1_entries_per_page);

    for (size_t i = 0; i < actual_l1_limit; ++i) {

      // TOCTOU Defense Level 1
      const auto l1_desc = l1_vaddr[i];

      uint64_t l2_pfn = l1_desc.template get<typename L1Layout::pfn_field>();

      // Guest provided a null PFN in the Root Table
      if (l2_pfn == 0)
        RELOCO_UNLIKELY
      return unexpected(error::security_violation);

      paddr_type l2_paddr{l2_pfn << PageTraits::page_shift};

      auto l2_map_res = mapper(l2_paddr);
      if (!l2_map_res)
        RELOCO_UNLIKELY
      return unexpected(l2_map_res.error());

      const auto *l2_vaddr = static_cast<const l2_packed_type *>(*l2_map_res) + l2_header_elements;

#if defined(__has_builtin) && __has_builtin(__builtin_prefetch)
      // rw = 0 -  prepare the prefetch for a read
      // locality = 3 - (default): High, L1 cache, leave the data in the L1, L2, and L3 cache levels after the access.
      __builtin_prefetch(l2_vaddr, 0, 3);
#endif

      for (size_t j = 0; j < l2_entries_per_page; ++j) {

        // TOCTOU Defense Level 2
        const auto l2_desc = l2_vaddr[j];

        uint64_t pfn_val = l2_desc.template get<typename L2Layout::pfn_field>();

        uint64_t offset = 0;
        if constexpr (!std::is_void_v<typename L2Layout::offset_field>) {
          offset = l2_desc.template get<typename L2Layout::offset_field>();
        }

        uint64_t length = PageTraits::page_size;
        if constexpr (!std::is_void_v<typename L2Layout::length_field>) {
          length = l2_desc.template get<typename L2Layout::length_field>();
        }

        // --- Speculation Defenses ---
        bool is_safe = (offset < PageTraits::page_size) & ((offset + length) <= PageTraits::page_size);

        if constexpr (!std::is_void_v<typename L2Layout::length_field>) {
          is_safe &= (length > 0);
        }

        offset = nospec::sanitize(offset, is_safe, UINT64_C(0));
        length = nospec::sanitize(length, is_safe, UINT64_C(0));

        if (!is_safe)
          RELOCO_UNLIKELY { return unexpected(error::security_violation); }

        paddr_type paddr{(pfn_val << PageTraits::page_shift) | offset};

        auto push_res = output.try_push_back(paddr, length);
        if (!push_res)
          RELOCO_UNLIKELY
        return unexpected(push_res.error());

        if constexpr (!std::is_void_v<typename L2Layout::last_flag>) {
          if (l2_desc.template get<typename L2Layout::last_flag>())
            RELOCO_UNLIKELY { return {}; }
        }
      }

      if constexpr (!std::is_void_v<typename L1Layout::last_flag>) {
        if (l1_desc.template get<typename L1Layout::last_flag>())
          RELOCO_UNLIKELY { return {}; }
      }
    }

    return {};
    RELOCO_END_UNSAFE_BUFFER_USAGE
  }
};

} // namespace structo
