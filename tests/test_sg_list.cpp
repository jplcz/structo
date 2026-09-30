// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#if !defined(_MSC_VER) && defined(__LP64__)
#include <gtest/gtest.h>
#include <structo/sg_list.hpp>

using dynamic_sg_list = structo::sg_list<>;

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using namespace structo;

TEST(SgListTest, PushBackDistinct) {
  dynamic_sg_list sgl;

  structo::phys_addr<void, structo::dma_bus_space> addr1(0x1000);
  structo::phys_addr<void, structo::dma_bus_space> addr2(0x8000); // Non-contiguous

  auto res1 = sgl.try_push_back(addr1, 4096);
  ASSERT_TRUE(res1.has_value());

  auto res2 = sgl.try_push_back(addr2, 8192);
  ASSERT_TRUE(res2.has_value());

  EXPECT_EQ(sgl.size(), 2u);

  auto it = sgl.begin();
  EXPECT_EQ(it->addr.value, 0x1000);
  EXPECT_EQ(it->length, 4096u);

  ++it;
  EXPECT_EQ(it->addr.value, 0x8000);
  EXPECT_EQ(it->length, 8192u);
}

TEST(SgListTest, AutomaticCoalescing) {
  dynamic_sg_list sgl;

  phys_addr<void, dma_bus_space> addr1(0x1000);
  phys_addr<void, dma_bus_space> addr2(0x2000); // Contiguous with addr1 + 4096
  phys_addr<void, dma_bus_space> addr3(0x3000); // Contiguous with addr2 + 4096

  EXPECT_TRUE(sgl.try_push_back(addr1, 4096).has_value());
  EXPECT_EQ(sgl.size(), 1u);

  // Pushing contiguous memory should NOT increase the vector size
  EXPECT_TRUE(sgl.try_push_back(addr2, 4096).has_value());
  EXPECT_EQ(sgl.size(), 1u);

  EXPECT_TRUE(sgl.try_push_back(addr3, 8192).has_value());
  EXPECT_EQ(sgl.size(), 1u);

  auto it = sgl.begin();
  EXPECT_EQ(it->addr.value, 0x1000);
  EXPECT_EQ(it->length, 16384u); // 4096 + 4096 + 8192
}

TEST(SgListTest, RejectNullAndZeroLength) {
  dynamic_sg_list sgl;

  // Zero length should succeed but do nothing
  EXPECT_TRUE(sgl.try_push_back(phys_addr<void, dma_bus_space>(0x1000), 0).has_value());
  EXPECT_TRUE(sgl.empty());

  // Null address should fail
  auto res = sgl.try_push_back(nullptr, 4096);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::invalid_argument);
}

RELOCO_END_UNSAFE_BUFFER_USAGE
#endif
