// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#if !defined(_MSC_VER) && defined(__LP64__)
#include <gtest/gtest.h>
#include <reloco/inline_vector.hpp>
#include <structo/compat_sg.hpp>
#include <structo/slot_map_ptr.hpp>

#include <array>
#include <cstdint>

namespace {

using namespace structo;

struct guest_to_host_policy {
  using from_space = guest_phys_space;
  using to_space = host_phys_space;

  result<uint64_t> translate(uint64_t address, uint64_t size) const noexcept {
    if (address + size > 0x10000)
      return unexpected(error::out_of_range);
    return address + 0x100000;
  }
};

using pfn_field = bitfield<0, 32>;
using offset_field = bitfield<32, 12>;
using length_field = bitfield<44, 12>;
using last_field = bitfield<56, 1>;
using chain_field = bitfield<57, 1>;
using compact_layout = sg_descriptor_layout<uint64_t, pfn_field, offset_field, length_field, last_field>;
using compact_codec = compact_sg_codec<compact_layout, page_4k, dma_bus_space>;

TEST(SgTranslatorTest, SplitsScatterGatherEntriesAtPageBoundaries) {
  using input_entry = sg_entry<guest_phys_space, uint64_t>;
  using output_entry = sg_entry<host_phys_space, uint64_t>;
  sg_list<inline_vector<input_entry, 4>> input;
  sg_list<inline_vector<output_entry, 4>> output;
  ASSERT_TRUE(input.try_push_back(phys_addr<void, guest_phys_space>{0x1ff0}, 32));

  ASSERT_TRUE((sg_translator::translate<guest_to_host_policy>(input, output, 4096)));
  ASSERT_EQ(output.size(), 1u);
  EXPECT_EQ(output.begin()->addr.value, 0x101ff0u);
  EXPECT_EQ(output.begin()->length, 32u);
}

TEST(SgTranslatorTest, RejectsInvalidPageSizeAndPropagatesMappingErrors) {
  using input_entry = sg_entry<guest_phys_space, uint64_t>;
  using output_entry = sg_entry<host_phys_space, uint64_t>;
  sg_list<inline_vector<input_entry, 2>> input;
  sg_list<inline_vector<output_entry, 2>> output;
  ASSERT_TRUE(input.try_push_back(phys_addr<void, guest_phys_space>{0x10000}, 1));

  auto invalid_page_size = sg_translator::translate<guest_to_host_policy>(input, output, 3000);
  ASSERT_FALSE(invalid_page_size);
  EXPECT_EQ(invalid_page_size.error(), error::invalid_argument);

  auto mapping_failure = sg_translator::translate<guest_to_host_policy>(input, output, 4096);
  ASSERT_FALSE(mapping_failure);
  EXPECT_EQ(mapping_failure.error(), error::out_of_range);
  EXPECT_TRUE(output.empty());
}

TEST(CompactSgCodecTest, EncodesAndDecodesPageFragments) {
  using entry_type = compact_codec::entry_type;
  using packed_type = compact_codec::packed_type;
  sg_list<inline_vector<entry_type, 4>> input;
  ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{0x1ff0}, 32));

  inline_vector<packed_type, 8> descriptors;
  ASSERT_TRUE(compact_codec::encode(input, descriptors));
  ASSERT_EQ(descriptors.size(), 2u);
  EXPECT_EQ(descriptors[0].get<offset_field>(), 0xff0u);
  EXPECT_EQ(descriptors[0].get<length_field>(), 16u);
  EXPECT_EQ(descriptors[0].get<last_field>(), 0u);
  EXPECT_EQ(descriptors[1].get<offset_field>(), 0u);
  EXPECT_EQ(descriptors[1].get<length_field>(), 16u);
  EXPECT_EQ(descriptors[1].get<last_field>(), 1u);

  sg_list<inline_vector<entry_type, 4>> decoded;
  span<const packed_type> encoded_view{descriptors.data(), descriptors.size()};
  ASSERT_TRUE(compact_codec::decode(encoded_view, decoded));
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded.begin()->addr.value, 0x1ff0u);
  EXPECT_EQ(decoded.begin()->length, 32u);
}

// Mirrors a hardware descriptor such as:
//   struct test_sglist_entry {
//     uint32_t phys_lo;
//     uint32_t phys_hi : 12;
//     uint32_t count   : 20; // whole 4 KB pages, not bytes
//   };
// which packs, little-endian, into a single 64-bit value: a contiguous
// 44-bit PFN (phys_lo's 32 bits followed by phys_hi's 12) in bits [43:0],
// and a 20-bit page count in bits [63:44] -- i.e. `bitfield<0, 44>` /
// `bitfield<44, 20>` over a `uint64_t`, with no Offset field (every
// descriptor is page-aligned) and `length_unit_pages` so the codec scales
// the field by `PageTraits::page_size` instead of treating it as bytes.
using test_sglist_pfn_field = bitfield<0, 44>;
using test_sglist_count_field = bitfield<44, 20>;
using test_sglist_layout =
    sg_descriptor_layout<uint64_t, test_sglist_pfn_field, void, test_sglist_count_field, void, 0, length_unit_pages>;
using test_sglist_codec = compact_sg_codec<test_sglist_layout, page_4k, dma_bus_space>;

TEST(CompactSgCodecTest, EncodesAndDecodesPageCountDescriptors) {
  using entry_type = test_sglist_codec::entry_type;
  using packed_type = test_sglist_codec::packed_type;
  sg_list<inline_vector<entry_type, 4>> input;
  // Three contiguous, page-aligned pages: a multi-page entry that a
  // byte-length field could not represent in a single descriptor at this
  // bit width, but a 20-bit page count represents trivially.
  ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{0x100000}, 3 * 4096));

  inline_vector<packed_type, 4> descriptors;
  ASSERT_TRUE(test_sglist_codec::encode(input, descriptors));
  ASSERT_EQ(descriptors.size(), 1u);
  EXPECT_EQ(descriptors[0].get<test_sglist_pfn_field>(), 0x100000u >> 12);
  EXPECT_EQ(descriptors[0].get<test_sglist_count_field>(), 3u);

  sg_list<inline_vector<entry_type, 4>> decoded;
  span<const packed_type> encoded_view{descriptors.data(), descriptors.size()};
  ASSERT_TRUE(test_sglist_codec::decode(encoded_view, decoded));
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded.begin()->addr.value, 0x100000u);
  EXPECT_EQ(decoded.begin()->length, 3u * 4096u);
}

TEST(CompactSgCodecTest, PageCountDescriptorsSplitAtHardwareCountLimitAndRejectSubPageRemainders) {
  using entry_type = test_sglist_codec::entry_type;
  using packed_type = test_sglist_codec::packed_type;

  // Rejects an entry whose length is not a whole number of pages.
  {
    sg_list<inline_vector<entry_type, 2>> input;
    ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{0x100000}, 4096 + 10));
    inline_vector<packed_type, 2> descriptors;
    auto res = test_sglist_codec::encode(input, descriptors);
    ASSERT_FALSE(res);
    EXPECT_EQ(res.error(), error::invalid_argument);
  }

  // A run longer than the 20-bit count field's max (2^20 - 1 pages) splits
  // into multiple descriptors, same as a byte-length field hitting its max.
  {
    sg_list<inline_vector<entry_type, 4>> input;
    constexpr uint64_t max_pages = (1u << 20) - 1;
    ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{uint64_t{0x10000}}, (max_pages + 2) * 4096));

    inline_vector<packed_type, 4> descriptors;
    ASSERT_TRUE(test_sglist_codec::encode(input, descriptors));
    ASSERT_EQ(descriptors.size(), 2u);
    EXPECT_EQ(descriptors[0].get<test_sglist_count_field>(), max_pages);
    EXPECT_EQ(descriptors[1].get<test_sglist_count_field>(), 2u);
  }
}

// Mirrors an NVMe-style PRP (Physical Region Page) entry: a raw,
// page-aligned 64-bit physical address with the low 12 bits reserved
// (always zero on the wire), rather than a PFN right-justified at bit 0.
// Positioning the PFN field's `Offset` at `page_shift` instead of `0`
// reproduces that: the codec still does its PFN math in frame-number
// units internally, but `bitfield<12, 52>` places those bits back at
// their natural position in the 64-bit word, leaving bits [11:0] as the
// implicit, always-zero page offset. No Offset/Length field exists
// because a PRP entry always covers exactly one page (chaining into
// further pages is a `chained_sg_codec`-level concern, not this format).
using nvme_prp_pfn_field = bitfield<12, 52>;
using nvme_prp_layout = sg_descriptor_layout<uint64_t, nvme_prp_pfn_field>;
using nvme_prp_codec = compact_sg_codec<nvme_prp_layout, page_4k>;

TEST(CompactSgCodecTest, EncodesRawPageAlignedAddressesViaAnOffsetPfnField) {
  using entry_type = nvme_prp_codec::entry_type;
  using packed_type = nvme_prp_codec::packed_type;
  sg_list<inline_vector<entry_type, 4>> input;
  ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{0x200000}, 2 * 4096));

  inline_vector<packed_type, 4> descriptors;
  ASSERT_TRUE(nvme_prp_codec::encode(input, descriptors));
  ASSERT_EQ(descriptors.size(), 2u);
  // The wire value is the plain physical address itself (low 12 bits zero),
  // not a frame number sitting at bit 0.
  EXPECT_EQ(descriptors[0].value(), 0x200000u);
  EXPECT_EQ(descriptors[1].value(), 0x201000u);

  sg_list<inline_vector<entry_type, 4>> decoded;
  span<const packed_type> encoded_view{descriptors.data(), descriptors.size()};
  ASSERT_TRUE(nvme_prp_codec::decode(encoded_view, decoded));
  ASSERT_EQ(decoded.size(), 1u); // sg_list re-coalesces the two contiguous pages
  EXPECT_EQ(decoded.begin()->addr.value, 0x200000u);
  EXPECT_EQ(decoded.begin()->length, 2u * 4096u);
}

// Demonstrates `HeaderSize` on a non-chained `compact_sg_codec`: a fixed,
// opaque leading word (e.g. an entry-count/cookie a caller fills in once
// encoding is complete) that `encode()`/`decode()` reserve and skip,
// without the codec itself knowing or caring what it holds.
using header_pfn_field = bitfield<0, 52>;
using header_length_field = bitfield<52, 12>;
using headered_layout =
    sg_descriptor_layout<uint64_t, header_pfn_field, void, header_length_field, void, sizeof(uint64_t)>;
using headered_codec = compact_sg_codec<headered_layout, page_4k>;

TEST(CompactSgCodecTest, ReservesAndSkipsAnOpaqueLeadingHeaderWord) {
  using entry_type = headered_codec::entry_type;
  using packed_type = headered_codec::packed_type;
  sg_list<inline_vector<entry_type, 4>> input;
  ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{0x3000}, 100));

  inline_vector<packed_type, 4> descriptors;
  ASSERT_TRUE(headered_codec::encode(input, descriptors));
  ASSERT_EQ(descriptors.size(), 2u); // one reserved header word + one data descriptor
  EXPECT_EQ(descriptors[0].value(), 0u);

  // The caller is free to overwrite the reserved header with real metadata
  // after encoding; decode() always skips exactly `header_size` bytes
  // regardless of what's stored there.
  descriptors[0] = packed_type{0xdeadbeefu};

  sg_list<inline_vector<entry_type, 4>> decoded;
  span<const packed_type> encoded_view{descriptors.data(), descriptors.size()};
  ASSERT_TRUE(headered_codec::decode(encoded_view, decoded));
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded.begin()->addr.value, 0x3000u);
  EXPECT_EQ(decoded.begin()->length, 100u);
}

TEST(CompactSgCodecTest, RejectsDescriptorCrossingPageBoundary) {
  using packed_type = compact_codec::packed_type;
  inline_vector<packed_type, 4> descriptors;
  packed_type descriptor;
  descriptor.truncating_set<pfn_field>(1);
  descriptor.truncating_set<offset_field>(0xff0);
  descriptor.truncating_set<length_field>(32);
  ASSERT_TRUE(descriptors.try_push_back(descriptor));

  sg_list<inline_vector<compact_codec::entry_type, 2>> decoded;
  span<const packed_type> encoded_view{descriptors.data(), descriptors.size()};
  auto result = compact_codec::decode(encoded_view, decoded);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), error::security_violation);
}

TEST(ChainedSgCodecTest, LinksPagesWhenDescriptorPageFills) {
  using layout = chained_sg_layout<uint64_t, pfn_field, offset_field, length_field, last_field, chain_field>;
  using codec = chained_sg_codec<layout, page_4k, dma_bus_space>;
  using packed_type = codec::packed_type;
  using paddr_type = codec::paddr_type;
  using entry_type = sg_entry<dma_bus_space, uint64_t>;
  using page_type = reloco::array<packed_type, page_4k::page_size / sizeof(packed_type)>;

  reloco::array<page_type, 2> pages{};
  uint64_t next_page_address = 0x1000;
  auto allocate = [&]() -> result<paddr_type> {
    const auto address = paddr_type{next_page_address};
    next_page_address += page_4k::page_size;
    return address;
  };
  auto map_for_write = [&](paddr_type address) -> result<packed_type *> {
    if (address.value == 0x1000)
      return pages[0].data();
    if (address.value == 0x2000)
      return pages[1].data();
    return unexpected(error::not_found);
  };

  sg_list<inline_vector<entry_type, 512>> input;
  for (uint64_t i = 0; i < 512; ++i) {
    ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{0x2100 + i * 0x2000}, 1));
  }
  inline_vector<paddr_type, 2> allocated_pages;
  auto root = codec::encode(input, allocate, map_for_write, allocated_pages);
  ASSERT_TRUE(root);
  EXPECT_EQ(root->value, 0x1000u);
  ASSERT_EQ(allocated_pages.size(), 2u);

  auto map_for_read = [&](paddr_type address) -> result<const packed_type *> {
    if (address.value == 0x1000)
      return pages[0].data();
    if (address.value == 0x2000)
      return pages[1].data();
    return unexpected(error::not_found);
  };
  sg_list<inline_vector<entry_type, 512>> decoded;
  ASSERT_TRUE(codec::decode(*root, decoded, map_for_read, 2));
  ASSERT_EQ(decoded.size(), 512u);
  EXPECT_EQ(decoded.begin()->addr.value, 0x2100u);
  EXPECT_EQ(decoded.begin()->length, 1u);
  EXPECT_EQ(decoded.base().back().addr.value, 0x2100u + 511 * 0x2000u);
}

TEST(TwoLevelSgCodecTest, RoundTripsRootAndLeafTables) {
  using root_layout = sg_descriptor_layout<uint64_t, pfn_field, void, void, last_field>;
  using codec = two_level_sg_codec<root_layout, compact_layout, page_4k, dma_bus_space>;
  using packed_type = codec::l1_packed_type;
  using paddr_type = codec::paddr_type;
  using entry_type = sg_entry<dma_bus_space, uint64_t>;
  using page_type = reloco::array<packed_type, page_4k::page_size / sizeof(packed_type)>;

  reloco::array<page_type, 2> pages{};
  uint64_t next_page_address = 0x1000;
  auto allocate = [&]() -> result<paddr_type> {
    const auto address = paddr_type{next_page_address};
    next_page_address += page_4k::page_size;
    return address;
  };
  auto map_page = [&](paddr_type address) -> result<void *> {
    if (address.value == 0x1000)
      return static_cast<void *>(pages[0].data());
    if (address.value == 0x2000)
      return static_cast<void *>(pages[1].data());
    return unexpected(error::not_found);
  };

  sg_list<inline_vector<entry_type, 2>> input;
  ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{0x3450}, 48));
  inline_vector<paddr_type, 3> allocated_pages;
  auto root = codec::encode(input, allocate, map_page, allocated_pages);
  ASSERT_TRUE(root);
  EXPECT_EQ(root->value, 0x1000u);
  ASSERT_EQ(allocated_pages.size(), 2u);

  sg_list<inline_vector<entry_type, 2>> decoded;
  ASSERT_TRUE(codec::decode(*root, decoded, map_page, 4));
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded.begin()->addr.value, 0x3450u);
  EXPECT_EQ(decoded.begin()->length, 48u);
}

// ============================================================================
// Proves the codecs' `Mapper` contract is satisfied not just by bare
// pointers (above) but also by a move-only RAII handle such as
// `slot_map_ptr<T, M>::guard` -- i.e. that `sg_mapper_detail::release_handle`
// is called early enough that the codecs work even with pools far smaller
// than the number of physical pages visited.
// ============================================================================

// A software-only ArchHooks: "slot_size"-aligned windows into a flat backing
// buffer, indexed directly by the (small, test-only) physical address used
// as the offset -- enough to prove slot acquire/release pairing without a
// real MMU. `NumSlots` is part of the type so each (count, buffer) pairing
// used below gets its own independent static pool/storage.
template <std::size_t NumSlots, std::size_t BufSize> struct fake_page_hooks {
  static constexpr std::size_t slot_size = page_4k::page_size;
  static_assert(BufSize % slot_size == 0);

  alignas(16) static inline std::byte backing[BufSize]{};
  static inline std::size_t phys_offset[NumSlots] = {};
  static inline int program_calls = 0;
  static inline int unprogram_calls = 0;

  static void reset_counters() noexcept {
    program_calls = 0;
    unprogram_calls = 0;
  }

  static void *slot_base(std::size_t slot) noexcept { return backing + phys_offset[slot]; }

  static result<void> program(std::size_t slot, std::uint64_t phys_aligned) noexcept {
    if (phys_aligned >= BufSize)
      return unexpected(error::out_of_range);
    phys_offset[slot] = static_cast<std::size_t>(phys_aligned);
    ++program_calls;
    return {};
  }

  static void unprogram(std::size_t /*slot*/) noexcept { ++unprogram_calls; }
};

TEST(ChainedSgCodecTest, WorksWithASingleSlotRaiiMapper) {
  using layout = chained_sg_layout<uint64_t, pfn_field, offset_field, length_field, last_field, chain_field>;
  using codec = chained_sg_codec<layout, page_4k, dma_bus_space>;
  using packed_type = codec::packed_type;
  using paddr_type = codec::paddr_type;
  using entry_type = sg_entry<dma_bus_space, uint64_t>;

  // Only a *single* slot: proves `chained_sg_codec` releases the previous
  // page's handle before mapping the next one, rather than needing one
  // slot per linked descriptor page.
  using hooks = fake_page_hooks<1, 0x3000>;
  using mapper_type = slot_map_mapper<1, hooks, dma_bus_space>;
  hooks::reset_counters();

  uint64_t next_page_address = 0x1000;
  auto allocate = [&]() -> result<paddr_type> {
    const auto address = paddr_type{next_page_address};
    next_page_address += page_4k::page_size;
    return address;
  };
  auto map_for_write = [](paddr_type address) -> result<slot_map_ptr<packed_type, mapper_type>::guard> {
    auto ptr = slot_map_ptr<packed_type, mapper_type>::from_paddr(
        phys_addr<packed_type, dma_bus_space>{address.value});
    if (!ptr)
      return unexpected(ptr.error());
    return ptr->try_map(page_4k::page_size);
  };

  sg_list<inline_vector<entry_type, 512>> input;
  for (uint64_t i = 0; i < 512; ++i) {
    ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{0x2100 + i * 0x2000}, 1));
  }
  inline_vector<paddr_type, 2> allocated_pages;
  auto root = codec::encode(input, allocate, map_for_write, allocated_pages);
  ASSERT_TRUE(root);
  EXPECT_EQ(root->value, 0x1000u);
  ASSERT_EQ(allocated_pages.size(), 2u);
  // Both pages were mapped, and every mapping but the one still held when
  // encode() returns was released -- i.e. no slot leak across the 2 pages
  // despite only ever having 1 slot available.
  EXPECT_EQ(hooks::program_calls, 2);
  EXPECT_EQ(hooks::unprogram_calls, 2);

  auto map_for_read = [](paddr_type address) -> result<slot_map_ptr<const packed_type, mapper_type>::guard> {
    auto ptr = slot_map_ptr<const packed_type, mapper_type>::from_paddr(
        phys_addr<const packed_type, dma_bus_space>{address.value});
    if (!ptr)
      return unexpected(ptr.error());
    return ptr->try_map(page_4k::page_size);
  };

  sg_list<inline_vector<entry_type, 512>> decoded;
  ASSERT_TRUE(codec::decode(*root, decoded, map_for_read, 2));
  ASSERT_EQ(decoded.size(), 512u);
  EXPECT_EQ(decoded.begin()->addr.value, 0x2100u);
  EXPECT_EQ(decoded.begin()->length, 1u);
  EXPECT_EQ(decoded.base().back().addr.value, 0x2100u + 511 * 0x2000u);
  EXPECT_EQ(hooks::program_calls, 4); // 2 more, for decode()'s own re-mapping
  EXPECT_EQ(hooks::unprogram_calls, 4);
}

TEST(TwoLevelSgCodecTest, WorksWithATwoSlotRaiiMapper) {
  using root_layout = sg_descriptor_layout<uint64_t, pfn_field, void, void, last_field>;
  using codec = two_level_sg_codec<root_layout, compact_layout, page_4k, dma_bus_space>;
  using packed_type = codec::l1_packed_type;
  using paddr_type = codec::paddr_type;
  using entry_type = sg_entry<dma_bus_space, uint64_t>;

  // 2 slots: one held for the L1 Root Table for the whole call, one for
  // whichever L2 Leaf page is currently being written/read.
  using hooks = fake_page_hooks<2, 0x3000>;
  using mapper_type = slot_map_mapper<2, hooks, dma_bus_space>;
  hooks::reset_counters();

  uint64_t next_page_address = 0x1000;
  auto allocate = [&]() -> result<paddr_type> {
    const auto address = paddr_type{next_page_address};
    next_page_address += page_4k::page_size;
    return address;
  };
  auto map_page = [](paddr_type address) -> result<slot_map_ptr<packed_type, mapper_type>::guard> {
    auto ptr =
        slot_map_ptr<packed_type, mapper_type>::from_paddr(phys_addr<packed_type, dma_bus_space>{address.value});
    if (!ptr)
      return unexpected(ptr.error());
    return ptr->try_map(page_4k::page_size);
  };

  sg_list<inline_vector<entry_type, 2>> input;
  ASSERT_TRUE(input.try_push_back(phys_addr<void, dma_bus_space>{0x3450}, 48));
  inline_vector<paddr_type, 3> allocated_pages;
  auto root = codec::encode(input, allocate, map_page, allocated_pages);
  ASSERT_TRUE(root);
  EXPECT_EQ(root->value, 0x1000u);
  ASSERT_EQ(allocated_pages.size(), 2u);
  EXPECT_EQ(hooks::program_calls, 2);
  EXPECT_EQ(hooks::unprogram_calls, 2);

  sg_list<inline_vector<entry_type, 2>> decoded;
  ASSERT_TRUE(codec::decode(*root, decoded, map_page, 4));
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded.begin()->addr.value, 0x3450u);
  EXPECT_EQ(decoded.begin()->length, 48u);
  EXPECT_EQ(hooks::program_calls, 4);
  EXPECT_EQ(hooks::unprogram_calls, 4);
}

} // namespace
#endif
