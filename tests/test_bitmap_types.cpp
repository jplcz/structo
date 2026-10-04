// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/bitmap_view.hpp>
#include <structo/dynamic_bitmap.hpp>
#include <structo/fixed_bitmap.hpp>

#include <cstddef>
#include <utility>

using structo::bitmap_view;
using structo::dynamic_bitmap;
using structo::fixed_bitmap;

namespace {
constexpr std::size_t kBits = 70; // deliberately not a multiple of bits_per_word
} // namespace

// ---------------------------------------------------------------------------
// fixed_bitmap<N>
// ---------------------------------------------------------------------------

TEST(FixedBitmapTest, DefaultIsAllClear) {
  fixed_bitmap<kBits> bm;
  EXPECT_EQ(bm.size(), kBits);
  EXPECT_TRUE(bm.none());
  EXPECT_EQ(bm.count(), 0u);
}

TEST(FixedBitmapTest, EmptyFactoryIsAllClear) {
  auto bm = fixed_bitmap<kBits>::empty();
  EXPECT_TRUE(bm.none());
}

TEST(FixedBitmapTest, FilledFactorySetsEveryBitButNoTailPadding) {
  auto bm = fixed_bitmap<kBits>::filled();
  EXPECT_TRUE(bm.all());
  EXPECT_EQ(bm.count(), kBits);
}

TEST(FixedBitmapTest, SetClearTestToggleRoundTrip) {
  fixed_bitmap<kBits> bm;
  bm.set(5);
  EXPECT_TRUE(bm.test(5));
  bm.toggle(5);
  EXPECT_FALSE(bm.test(5));
  bm.toggle(5);
  EXPECT_TRUE(bm.test(5));
  bm.clear(5);
  EXPECT_TRUE(bm.none());
}

TEST(FixedBitmapTest, TryVariantsRejectOutOfRange) {
  fixed_bitmap<kBits> bm;
  EXPECT_FALSE(bm.try_set(kBits).has_value());
  EXPECT_FALSE(bm.try_test(kBits).has_value());
}

TEST(FixedBitmapTest, FindAndSetClaimsLowestFreeSlot) {
  fixed_bitmap<8> bm;
  bm.set(0);
  bm.set(1);
  auto slot = bm.find_and_set();
  ASSERT_TRUE(slot.has_value());
  EXPECT_EQ(*slot, 2u);
  EXPECT_TRUE(bm.test(2));
}

TEST(FixedBitmapTest, LowestClearFindsFreeSlot) {
  auto bm = fixed_bitmap<8>::filled();
  bm.clear(3);
  auto free_slot = bm.lowest_clear();
  ASSERT_TRUE(free_slot.has_value());
  EXPECT_EQ(*free_slot, 3u);
}

TEST(FixedBitmapTest, CopyableAndMovable) {
  fixed_bitmap<kBits> bm;
  bm.set(10);
  fixed_bitmap<kBits> copy = bm; // NOLINT(performance-unnecessary-copy-initialization)
  EXPECT_TRUE(copy.test(10));
  fixed_bitmap<kBits> moved = std::move(bm);
  EXPECT_TRUE(moved.test(10));
}

TEST(FixedBitmapTest, AtomicSetTestRoundTrip) {
  fixed_bitmap<kBits> bm;
  bm.atomic_set(5);
  EXPECT_TRUE(bm.atomic_test(5));
  bm.atomic_clear(5);
  EXPECT_FALSE(bm.atomic_test(5));
}

// ---------------------------------------------------------------------------
// dynamic_bitmap
// ---------------------------------------------------------------------------

TEST(DynamicBitmapTest, TryCreateAllocatesAllClearBitmap) {
  auto maker = dynamic_bitmap::try_create(kBits);
  ASSERT_TRUE(maker.has_value());
  dynamic_bitmap bm = std::move(maker.value());
  EXPECT_EQ(bm.size(), kBits);
  EXPECT_TRUE(bm.none());
}

TEST(DynamicBitmapTest, EmptyBitmapNeverAllocates) {
  auto maker = dynamic_bitmap::try_create(0);
  ASSERT_TRUE(maker.has_value());
  dynamic_bitmap bm = std::move(maker.value());
  EXPECT_EQ(bm.size(), 0u);
  EXPECT_TRUE(bm.none());
}

TEST(DynamicBitmapTest, SetClearTestToggleRoundTrip) {
  auto maker = dynamic_bitmap::try_create(kBits);
  ASSERT_TRUE(maker.has_value());
  dynamic_bitmap bm = std::move(maker.value());
  bm.set(42);
  EXPECT_TRUE(bm.test(42));
  bm.toggle(42);
  EXPECT_FALSE(bm.test(42));
  bm.clear(42);
  EXPECT_TRUE(bm.none());
}

TEST(DynamicBitmapTest, MoveTransfersOwnershipAndNullsSource) {
  auto maker = dynamic_bitmap::try_create(kBits);
  ASSERT_TRUE(maker.has_value());
  dynamic_bitmap bm = std::move(maker.value());
  bm.set(7);

  dynamic_bitmap moved = std::move(bm);
  EXPECT_TRUE(moved.test(7));
  EXPECT_EQ(bm.size(), 0u); // moved-from: inert, safe to destroy

  dynamic_bitmap move_assigned;
  move_assigned = std::move(moved);
  EXPECT_TRUE(move_assigned.test(7));
}

TEST(DynamicBitmapTest, TryCloneProducesIndependentCopy) {
  auto maker = dynamic_bitmap::try_create(kBits);
  ASSERT_TRUE(maker.has_value());
  dynamic_bitmap bm = std::move(maker.value());
  bm.set(13);

  auto clone_res = bm.try_clone();
  ASSERT_TRUE(clone_res.has_value());
  dynamic_bitmap clone = std::move(clone_res.value());
  EXPECT_TRUE(clone.test(13));

  bm.clear(13);
  EXPECT_FALSE(bm.test(13));
  EXPECT_TRUE(clone.test(13)); // unaffected by mutating the original
}

TEST(DynamicBitmapTest, FindAndSetClaimsLowestFreeSlot) {
  auto maker = dynamic_bitmap::try_create(8);
  ASSERT_TRUE(maker.has_value());
  dynamic_bitmap bm = std::move(maker.value());
  bm.set(0);
  auto slot = bm.find_and_set();
  ASSERT_TRUE(slot.has_value());
  EXPECT_EQ(*slot, 1u);
}

// ---------------------------------------------------------------------------
// bitmap_view
// ---------------------------------------------------------------------------

TEST(BitmapViewTest, WrapsExternalStorageWithoutOwningIt) {
  unsigned long words[structo::bitmap_utils::word_count_for(kBits)]{};
  bitmap_view view(reloco::span<unsigned long>(words), kBits);
  EXPECT_EQ(view.size(), kBits);
  EXPECT_TRUE(view.none());

  view.set(20);
  EXPECT_TRUE(view.test(20));
  // The mutation is visible directly through the wrapped backing array,
  // since the view owns no storage of its own.
  constexpr std::size_t bpw = structo::bitmap_utils::bits_per_word;
  EXPECT_NE(words[20 / bpw] & (1ul << (20 % bpw)), 0ul);
}

TEST(BitmapViewTest, FindAndSetClaimsLowestFreeSlot) {
  unsigned long words[structo::bitmap_utils::word_count_for(8)]{};
  bitmap_view view(reloco::span<unsigned long>(words), 8);
  view.set(0);
  auto slot = view.find_and_set();
  ASSERT_TRUE(slot.has_value());
  EXPECT_EQ(*slot, 1u);
}

TEST(BitmapViewTest, TwoViewsOverSameStorageObserveEachOthersWrites) {
  unsigned long words[structo::bitmap_utils::word_count_for(kBits)]{};
  bitmap_view a(reloco::span<unsigned long>(words), kBits);
  bitmap_view b(reloco::span<unsigned long>(words), kBits);
  a.set(30);
  EXPECT_TRUE(b.test(30));
}
