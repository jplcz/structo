// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/rng_combinator.hpp>

#include <array>
#include <deque>

namespace {

using namespace structo;
using namespace structo::hw;

// --------------------------------------------------------------------
// A fake hardware RNG backend driven by a scripted queue of draws,
// plus a settable `available` flag.
// --------------------------------------------------------------------
struct fake_rng {
  std::deque<result<std::uint64_t>> draws;
  bool available = true;
};

} // namespace

template <> struct structo::hw::hw_rng_traits<fake_rng> {
  static reloco::result<std::uint64_t> try_generate64(fake_rng &b) noexcept {
    if (b.draws.empty())
      return reloco::unexpected(reloco::error::try_again);
    auto v = b.draws.front();
    b.draws.pop_front();
    return v;
  }
  static bool is_available(fake_rng &b) noexcept { return b.available; }
};

namespace {

TEST(HwRngCombinatorTest, EmptySourceListFailsWithUnsupportedOperation) {
  hw_rng_combinator combo(span<const hw_rng_ref>{});
  EXPECT_FALSE(combo.is_available());

  auto word = combo.try_generate64();
  ASSERT_FALSE(word.has_value());
  EXPECT_EQ(word.error(), error::unsupported_operation);
}

TEST(HwRngCombinatorTest, IsAvailableTrueIfAnySourceAvailable) {
  fake_rng a, b;
  a.available = false;
  b.available = true;
  std::array<hw_rng_ref, 2> sources{hw_rng_ref(a), hw_rng_ref(b)};
  hw_rng_combinator combo(sources);
  EXPECT_TRUE(combo.is_available());
}

TEST(HwRngCombinatorTest, IsAvailableFalseIfNoSourceAvailable) {
  fake_rng a, b;
  a.available = false;
  b.available = false;
  std::array<hw_rng_ref, 2> sources{hw_rng_ref(a), hw_rng_ref(b)};
  hw_rng_combinator combo(sources);
  EXPECT_FALSE(combo.is_available());
}

TEST(HwRngCombinatorTest, SingleSourceDrawIsAvalancheMixedNotPassedThrough) {
  fake_rng a;
  a.draws.push_back(std::uint64_t{0x1234567890ABCDEFull});
  std::array<hw_rng_ref, 1> sources{hw_rng_ref(a)};
  hw_rng_combinator combo(sources);

  auto word = combo.try_generate64();
  ASSERT_TRUE(word.has_value());
  // The combinator always finalizes through avalanche_mix64, even with
  // a single source: the raw draw must not pass through unmodified.
  EXPECT_NE(word.value(), 0x1234567890ABCDEFull);
}

TEST(HwRngCombinatorTest, CombiningIsDeterministicGivenTheSameSourceDraws) {
  fake_rng a1, b1;
  a1.draws.push_back(std::uint64_t{111});
  b1.draws.push_back(std::uint64_t{222});
  std::array<hw_rng_ref, 2> sources1{hw_rng_ref(a1), hw_rng_ref(b1)};
  hw_rng_combinator combo1(sources1);
  auto word1 = combo1.try_generate64();

  fake_rng a2, b2;
  a2.draws.push_back(std::uint64_t{111});
  b2.draws.push_back(std::uint64_t{222});
  std::array<hw_rng_ref, 2> sources2{hw_rng_ref(a2), hw_rng_ref(b2)};
  hw_rng_combinator combo2(sources2);
  auto word2 = combo2.try_generate64();

  ASSERT_TRUE(word1.has_value());
  ASSERT_TRUE(word2.has_value());
  EXPECT_EQ(word1.value(), word2.value());
}

TEST(HwRngCombinatorTest, SucceedsWhenOnlySomeSourcesSucceed) {
  fake_rng ok, dead;
  ok.draws.push_back(std::uint64_t{999});
  // `dead`'s queue is empty: every draw reports try_again, exhausting
  // its retry budget below.
  std::array<hw_rng_ref, 2> sources{hw_rng_ref(ok), hw_rng_ref(dead)};
  hw_rng_combinator combo(sources);

  auto word = combo.try_generate64(/*max_retries_per_source=*/1);
  EXPECT_TRUE(word.has_value());
}

TEST(HwRngCombinatorTest, FailsOnlyWhenEverySourceFails) {
  fake_rng a, b; // both queues empty -> both report try_again
  std::array<hw_rng_ref, 2> sources{hw_rng_ref(a), hw_rng_ref(b)};
  hw_rng_combinator combo(sources);

  auto word = combo.try_generate64(/*max_retries_per_source=*/1);
  ASSERT_FALSE(word.has_value());
  EXPECT_EQ(word.error(), error::try_again);
}

TEST(HwRngCombinatorTest, ReportsLastObservedErrorWhenEverySourceFails) {
  fake_rng a, b;
  a.draws.push_back(reloco::unexpected(error::io_error));
  // `b`'s queue is empty -> reports try_again.
  std::array<hw_rng_ref, 2> sources{hw_rng_ref(a), hw_rng_ref(b)};
  hw_rng_combinator combo(sources);

  auto word = combo.try_generate64(/*max_retries_per_source=*/1);
  ASSERT_FALSE(word.has_value());
  EXPECT_EQ(word.error(), error::try_again);
}

TEST(HwRngCombinatorTest, UnboundRefSourceContributesNothingButDoesNotAbort) {
  hw_rng_ref unbound;
  fake_rng ok;
  ok.draws.push_back(std::uint64_t{555});
  std::array<hw_rng_ref, 2> sources{unbound, hw_rng_ref(ok)};
  hw_rng_combinator combo(sources);

  auto word = combo.try_generate64();
  EXPECT_TRUE(word.has_value());
}

TEST(HwRngCombinatorTest, CombinatorItselfBindsThroughAnotherHwRngRef) {
  fake_rng a;
  a.draws.push_back(std::uint64_t{123});
  std::array<hw_rng_ref, 1> sources{hw_rng_ref(a)};
  hw_rng_combinator combo(sources);

  hw_rng_ref outer(combo);
  EXPECT_TRUE(static_cast<bool>(outer));
  EXPECT_TRUE(outer.is_available());

  auto word = outer.try_generate64();
  EXPECT_TRUE(word.has_value());
}

} // namespace
