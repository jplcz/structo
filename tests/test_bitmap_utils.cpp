// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/lifetime.hpp>
#include <structo/bitmap_utils.hpp>

#include <cstddef>

using structo::bitmap_utils;

namespace {

constexpr std::size_t kBits = 70; // deliberately not a multiple of bits_per_word

/** @brief Fixture for `bitmap_utils` tests: a fresh, zeroed backing array per test. */
class BitmapUtilsTest : public ::testing::Test {
protected:
  unsigned long words_[bitmap_utils::word_count_for(kBits)]{};
  reloco::span<unsigned long> bits_{words_};
  reloco::span<const unsigned long> cbits_{words_};
};

} // namespace

TEST_F(BitmapUtilsTest, WordCountForRoundsUp) {
  EXPECT_EQ(bitmap_utils::word_count_for(0), 0u);
  EXPECT_EQ(bitmap_utils::word_count_for(1), 1u);
  EXPECT_EQ(bitmap_utils::word_count_for(bitmap_utils::bits_per_word), 1u);
  EXPECT_EQ(bitmap_utils::word_count_for(bitmap_utils::bits_per_word + 1), 2u);
}

TEST_F(BitmapUtilsTest, DefaultIsAllClear) {
  EXPECT_TRUE(bitmap_utils::none(cbits_));
  EXPECT_FALSE(bitmap_utils::any(cbits_));
  EXPECT_FALSE(bitmap_utils::all(cbits_, kBits));
  EXPECT_EQ(bitmap_utils::count(cbits_), 0u);
}

TEST_F(BitmapUtilsTest, SetClearTestToggleRoundTrip) {
  bitmap_utils::set(bits_, kBits, 5);
  EXPECT_TRUE(bitmap_utils::test(cbits_, kBits, 5));
  bitmap_utils::toggle(bits_, kBits, 5);
  EXPECT_FALSE(bitmap_utils::test(cbits_, kBits, 5));
  bitmap_utils::toggle(bits_, kBits, 5);
  EXPECT_TRUE(bitmap_utils::test(cbits_, kBits, 5));
  bitmap_utils::clear(bits_, kBits, 5);
  EXPECT_FALSE(bitmap_utils::test(cbits_, kBits, 5));
  EXPECT_TRUE(bitmap_utils::none(cbits_));
}

TEST_F(BitmapUtilsTest, TryVariantsRejectOutOfRange) {
  EXPECT_FALSE(bitmap_utils::try_set(bits_, kBits, kBits).has_value());
  EXPECT_TRUE(bitmap_utils::try_set(bits_, kBits, kBits - 1).has_value());
  EXPECT_FALSE(bitmap_utils::try_test(cbits_, kBits, kBits).has_value());
  EXPECT_FALSE(bitmap_utils::try_clear(bits_, kBits, kBits).has_value());
  EXPECT_FALSE(bitmap_utils::try_toggle(bits_, kBits, kBits).has_value());
}

#if !defined(NDEBUG) || defined(RELOCO_ENABLE_ASSERTS)
TEST_F(BitmapUtilsTest, SetTrapsOnOutOfRangeIndex) {
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ bitmap_utils::set(bits_, kBits, kBits); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}
#endif

TEST_F(BitmapUtilsTest, FillSetsEveryBitInRangeAndMasksTailPadding) {
  bitmap_utils::fill(bits_, kBits);
  EXPECT_TRUE(bitmap_utils::all(cbits_, kBits));
  EXPECT_EQ(bitmap_utils::count(cbits_), kBits);
  for (std::size_t i = 0; i < kBits; ++i) {
    EXPECT_TRUE(bitmap_utils::test(cbits_, kBits, i)) << i;
  }
  // The last word only has (kBits % bits_per_word) valid bits; the rest
  // must have been masked back to zero by fill()/mask_tail_padding(), or
  // a subsequent lowest_clear_from() would incorrectly see them as free.
  EXPECT_FALSE(bitmap_utils::lowest_clear(cbits_, kBits).has_value());
}

TEST_F(BitmapUtilsTest, ClearAllClearsEveryWordRegardlessOfNbits) {
  bitmap_utils::fill(bits_, kBits);
  bitmap_utils::clear_all(bits_);
  EXPECT_TRUE(bitmap_utils::none(cbits_));
}

TEST_F(BitmapUtilsTest, LowestSetFromScansAcrossWordBoundary) {
  bitmap_utils::set(bits_, kBits, bitmap_utils::bits_per_word + 2); // forces a 2nd-word scan
  auto found = bitmap_utils::lowest_set(cbits_, kBits);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found.value(), bitmap_utils::bits_per_word + 2);

  auto found_from_mid = bitmap_utils::lowest_set_from(cbits_, kBits, 10);
  ASSERT_TRUE(found_from_mid.has_value());
  EXPECT_EQ(found_from_mid.value(), bitmap_utils::bits_per_word + 2);
}

TEST_F(BitmapUtilsTest, LowestClearFromFindsFirstFreeBit) {
  bitmap_utils::fill(bits_, kBits);
  bitmap_utils::clear(bits_, kBits, 40);
  auto found = bitmap_utils::lowest_clear(cbits_, kBits);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found.value(), 40u);

  // Nothing free at/after 41.
  EXPECT_FALSE(bitmap_utils::lowest_clear_from(cbits_, kBits, 41).has_value());
}

TEST_F(BitmapUtilsTest, HighestSetFindsLastBitWithinNbits) {
  bitmap_utils::set(bits_, kBits, kBits - 1);
  auto found = bitmap_utils::highest_set(cbits_, kBits);
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found.value(), kBits - 1);
}

TEST_F(BitmapUtilsTest, FindAndSetClaimsLowestClearBit) {
  bitmap_utils::fill(bits_, kBits);
  bitmap_utils::clear(bits_, kBits, 3);
  bitmap_utils::clear(bits_, kBits, 7);

  auto claimed = bitmap_utils::find_and_set(bits_, kBits);
  ASSERT_TRUE(claimed.has_value());
  EXPECT_EQ(claimed.value(), 3u);
  EXPECT_TRUE(bitmap_utils::test(cbits_, kBits, 3)); // now claimed/set

  auto claimed2 = bitmap_utils::find_and_set(bits_, kBits);
  ASSERT_TRUE(claimed2.has_value());
  EXPECT_EQ(claimed2.value(), 7u);

  EXPECT_FALSE(bitmap_utils::find_and_set(bits_, kBits).has_value()); // pool exhausted
}

// ---------------------------------------------------------------------------
// Atomic variants
// ---------------------------------------------------------------------------

TEST_F(BitmapUtilsTest, AtomicSetClearTestToggleRoundTrip) {
  bitmap_utils::atomic_set(bits_, kBits, 12);
  EXPECT_TRUE(bitmap_utils::atomic_test(cbits_, kBits, 12));
  bitmap_utils::atomic_toggle(bits_, kBits, 12);
  EXPECT_FALSE(bitmap_utils::atomic_test(cbits_, kBits, 12));
  bitmap_utils::atomic_clear(bits_, kBits, 12);
  EXPECT_FALSE(bitmap_utils::atomic_test(cbits_, kBits, 12));
}

TEST_F(BitmapUtilsTest, AtomicTestAndSetClearToggleReturnPreviousValue) {
  EXPECT_FALSE(bitmap_utils::unsafe_atomic_test_and_set(bits_, 9));
  EXPECT_TRUE(bitmap_utils::unsafe_atomic_test_and_set(bits_, 9)); // already set now
  EXPECT_TRUE(bitmap_utils::unsafe_atomic_test_and_clear(bits_, 9));
  EXPECT_FALSE(bitmap_utils::unsafe_atomic_test_and_clear(bits_, 9)); // already clear now
  EXPECT_FALSE(bitmap_utils::unsafe_atomic_test_and_toggle(bits_, 9));
  EXPECT_TRUE(bitmap_utils::atomic_test(cbits_, kBits, 9));
}

TEST_F(BitmapUtilsTest, AtomicLowestSetMatchesNonAtomic) {
  bitmap_utils::set(bits_, kBits, bitmap_utils::bits_per_word + 5);
  auto atomic_found = bitmap_utils::atomic_lowest_set(cbits_, kBits);
  auto plain_found = bitmap_utils::lowest_set(cbits_, kBits);
  ASSERT_TRUE(atomic_found.has_value());
  EXPECT_EQ(atomic_found.value(), plain_found.value());
}

TEST_F(BitmapUtilsTest, AtomicFindAndSetClaimsLowestClearBitAcrossWords) {
  bitmap_utils::fill(bits_, kBits);
  bitmap_utils::clear(bits_, kBits, bitmap_utils::bits_per_word + 1);

  auto claimed = bitmap_utils::atomic_find_and_set(bits_, kBits);
  ASSERT_TRUE(claimed.has_value());
  EXPECT_EQ(claimed.value(), bitmap_utils::bits_per_word + 1);
  EXPECT_TRUE(bitmap_utils::atomic_test(cbits_, kBits, bitmap_utils::bits_per_word + 1));
  EXPECT_FALSE(bitmap_utils::atomic_find_and_set(bits_, kBits).has_value()); // exhausted
}

TEST_F(BitmapUtilsTest, AtomicSnapshotCopiesWordByWord) {
  bitmap_utils::set(bits_, kBits, 1);
  bitmap_utils::set(bits_, kBits, bitmap_utils::bits_per_word + 2);

  unsigned long dst_words[bitmap_utils::word_count_for(kBits)]{};
  reloco::span<unsigned long> dst(dst_words);
  bitmap_utils::atomic_snapshot(cbits_, dst);

  reloco::span<const unsigned long> cdst(dst_words);
  EXPECT_TRUE(bitmap_utils::test(cdst, kBits, 1));
  EXPECT_TRUE(bitmap_utils::test(cdst, kBits, bitmap_utils::bits_per_word + 2));
  EXPECT_EQ(bitmap_utils::count(cdst), 2u);
}

// ---------------------------------------------------------------------------
// Range operations
// ---------------------------------------------------------------------------

TEST_F(BitmapUtilsTest, SetRangeSetsOnlyTheRequestedRangeAcrossWordBoundary) {
  const std::size_t bpw = bitmap_utils::bits_per_word;
  bitmap_utils::set_range(bits_, kBits, bpw - 3, bpw + 3);
  EXPECT_EQ(bitmap_utils::count_range(cbits_, kBits, bpw - 3, bpw + 3), 7u);
  EXPECT_FALSE(bitmap_utils::test(cbits_, kBits, bpw - 4));
  EXPECT_FALSE(bitmap_utils::test(cbits_, kBits, bpw + 4));
  EXPECT_TRUE(bitmap_utils::all_set_in_range(cbits_, kBits, bpw - 3, bpw + 3));
}

TEST_F(BitmapUtilsTest, ClearRangeClearsOnlyTheRequestedRange) {
  bitmap_utils::fill(bits_, kBits);
  bitmap_utils::clear_range(bits_, kBits, 10, 20);
  EXPECT_TRUE(bitmap_utils::all_clear_in_range(cbits_, kBits, 10, 20));
  EXPECT_TRUE(bitmap_utils::test(cbits_, kBits, 9));
  EXPECT_TRUE(bitmap_utils::test(cbits_, kBits, 21));
}

TEST_F(BitmapUtilsTest, SetRangeSingleWordUsesSingleWordMask) {
  bitmap_utils::set_range(bits_, kBits, 2, 5);
  EXPECT_EQ(bitmap_utils::count(cbits_), 4u);
  EXPECT_TRUE(bitmap_utils::all_set_in_range(cbits_, kBits, 2, 5));
  EXPECT_FALSE(bitmap_utils::test(cbits_, kBits, 1));
  EXPECT_FALSE(bitmap_utils::test(cbits_, kBits, 6));
}

TEST_F(BitmapUtilsTest, AllSetAllClearDistinguishMixedRanges) {
  bitmap_utils::set(bits_, kBits, 12);
  EXPECT_FALSE(bitmap_utils::all_set_in_range(cbits_, kBits, 10, 14));
  EXPECT_FALSE(bitmap_utils::all_clear_in_range(cbits_, kBits, 10, 14));
  EXPECT_TRUE(bitmap_utils::all_clear_in_range(cbits_, kBits, 10, 11));
}

TEST_F(BitmapUtilsTest, RangeTryVariantsRejectOutOfOrderOrOutOfRange) {
  EXPECT_FALSE(bitmap_utils::try_set_range(bits_, kBits, 5, 2).has_value());
  EXPECT_FALSE(bitmap_utils::try_set_range(bits_, kBits, 5, kBits).has_value());
  EXPECT_FALSE(bitmap_utils::try_clear_range(bits_, kBits, 5, kBits).has_value());
  EXPECT_FALSE(bitmap_utils::try_count_range(cbits_, kBits, 5, kBits).has_value());
  EXPECT_FALSE(bitmap_utils::try_all_set_in_range(cbits_, kBits, 5, kBits).has_value());
  EXPECT_FALSE(bitmap_utils::try_all_clear_in_range(cbits_, kBits, 5, kBits).has_value());
}

TEST_F(BitmapUtilsTest, LowestClearRunFromSkipsObstructionsAcrossWords) {
  const std::size_t bpw = bitmap_utils::bits_per_word;
  bitmap_utils::set(bits_, kBits, bpw - 1); // obstructs any run spanning that bit
  auto run = bitmap_utils::lowest_clear_run_from(cbits_, kBits, bpw - 4, 6);
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(*run, bpw); // first run of 6 clear bits starts right after the obstruction
}

TEST_F(BitmapUtilsTest, LowestClearRunFailsWhenNoRunFits) {
  bitmap_utils::fill(bits_, kBits);
  bitmap_utils::clear(bits_, kBits, kBits - 1);
  auto run = bitmap_utils::lowest_clear_run(cbits_, kBits, 2);
  EXPECT_FALSE(run.has_value());
}

TEST_F(BitmapUtilsTest, LowestSetRunFromFindsContiguousSetBits) {
  bitmap_utils::set_range(bits_, kBits, 5, 9);
  auto run = bitmap_utils::lowest_set_run_from(cbits_, kBits, 0, 5);
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(*run, 5u);
}

TEST_F(BitmapUtilsTest, FindAndSetRunClaimsAndMarksTheWholeRun) {
  auto run = bitmap_utils::find_and_set_run(bits_, kBits, 4);
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(*run, 0u);
  EXPECT_TRUE(bitmap_utils::all_set_in_range(cbits_, kBits, 0, 3));
  EXPECT_FALSE(bitmap_utils::test(cbits_, kBits, 4));

  auto run2 = bitmap_utils::find_and_set_run(bits_, kBits, 4);
  ASSERT_TRUE(run2.has_value());
  EXPECT_EQ(*run2, 4u);
}

TEST_F(BitmapUtilsTest, ZeroSizeRunRequestsTriviallySucceedAtStart) {
  auto run = bitmap_utils::lowest_clear_run_from(cbits_, kBits, 7, 0);
  ASSERT_TRUE(run.has_value());
  EXPECT_EQ(*run, 7u);
}
