// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#if !defined(_MSC_VER) && defined(__LP64__)
#include <gtest/gtest.h>
#include <structo/compat_sg.hpp>
#include <reloco/inline_vector.hpp>

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
using compact_layout =
    sg_descriptor_layout<uint64_t, pfn_field, offset_field, length_field, last_field>;
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
  using layout =
      chained_sg_layout<uint64_t, pfn_field, offset_field, length_field, last_field, chain_field>;
  using codec = chained_sg_codec<layout, page_4k, dma_bus_space>;
  using packed_type = codec::packed_type;
  using paddr_type = codec::paddr_type;
  using entry_type = sg_entry<dma_bus_space, uint64_t>;
  using page_type = std::array<packed_type, page_4k::page_size / sizeof(packed_type)>;

  std::array<page_type, 2> pages{};
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
  using page_type = std::array<packed_type, page_4k::page_size / sizeof(packed_type)>;

  std::array<page_type, 2> pages{};
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

} // namespace
#endif
