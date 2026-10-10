// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <structo/page_index_range.hpp>

#include <reloco/array.hpp>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using range = structo::page_index_range<std::uint64_t>;

TEST(PageIndexRange, BasicsAndNormalization) {
  EXPECT_TRUE(range{}.empty());
  EXPECT_TRUE((range{10, 5}).empty());
  EXPECT_EQ((range{3, 9}).size(), 6u);
  EXPECT_EQ(range::from_inclusive(4, 7), (range{4, 8}));
  EXPECT_TRUE(range::from_inclusive(7, 4).empty());
  EXPECT_EQ(range::from_count(UINT64_MAX - 1, 10).end(), UINT64_MAX);
  EXPECT_TRUE((range{3, 9}).contains(3));
  EXPECT_FALSE((range{3, 9}).contains(9));
}

TEST(PageIndexRange, OverlapIntersectMerge) {
  const range a{10, 20};
  EXPECT_TRUE(a.overlaps(range{19, 30}));
  EXPECT_FALSE(a.overlaps(range{20, 30}));
  EXPECT_TRUE(a.mergeable(range{20, 30}));
  EXPECT_EQ(a.intersect(range{15, 40}), (range{15, 20}));
  EXPECT_TRUE(a.intersect(range{20, 40}).empty());
  EXPECT_EQ(a.merge(range{20, 30}), (range{10, 30}));
  EXPECT_TRUE(a.contains(range{12, 18}));
}

TEST(PageIndexRange, SubtractAndSplit) {
  const range a{10, 20};
  auto mid = a.subtract(range{13, 16});
  EXPECT_EQ(mid.lower, (range{10, 13}));
  EXPECT_EQ(mid.upper, (range{16, 20}));
  auto all = a.subtract(range{0, 100});
  EXPECT_TRUE(all.lower.empty());
  EXPECT_TRUE(all.upper.empty());
  auto none = a.subtract(range{30, 40});
  EXPECT_EQ(none.lower, a);
  EXPECT_TRUE(none.upper.empty());
  auto s = a.split_at(14);
  EXPECT_EQ(s.lower, (range{10, 14}));
  EXPECT_EQ(s.upper, (range{14, 20}));
  EXPECT_TRUE(a.split_at(5).lower.empty());
}

TEST(PageIndexRange, IndicesAndAlignedChunks) {
  std::uint64_t sum = 0;
  auto it = range{3, 7}.indices();
  for (auto i : it)
    sum += i;
  EXPECT_EQ(sum, 3u + 4u + 5u + 6u);

  reloco::array<range, 8> got{};
  std::size_t n = 0;
  auto chunks = range{6, 21}.chunks(8);
  for (auto c : chunks)
    got[n++] = c;
  ASSERT_EQ(n, 3u);
  EXPECT_EQ(got[0], (range{6, 8}));
  EXPECT_EQ(got[1], (range{8, 16}));
  EXPECT_EQ(got[2], (range{16, 21}));

  auto whole = range{6, 21}.chunks(0);
  auto one = whole.next();
  ASSERT_TRUE(one.has_value());
  EXPECT_EQ(*one, (range{6, 21}));
  EXPECT_FALSE(whole.next().has_value());
}

TEST(PageIndexRange, ChunksNearMaxIndexDoNotWrap) {
  auto c = range{UINT64_MAX - 3, UINT64_MAX}.chunks(8);
  auto first = c.next();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first, (range{UINT64_MAX - 3, UINT64_MAX}));
  EXPECT_FALSE(c.next().has_value());
}

TEST(PageIndexRange, SliceIsBoundsChecked) {
  reloco::array<int, 8> data{};
  reloco::span<int> s{data};
  auto ok = (range{102, 106}).slice(s, 100);
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(ok->size(), 4u);
  EXPECT_FALSE((range{99, 103}).slice(s, 100).has_value());
  EXPECT_FALSE((range{104, 109}).slice(s, 100).has_value());
  EXPECT_TRUE((range{}).slice(s, 100).has_value());
}

} // namespace
