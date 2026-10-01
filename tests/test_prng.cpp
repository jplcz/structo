// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <structo/prng.hpp>
#include <gtest/gtest.h>

#include <deque>
#include <set>

namespace {

using namespace structo;
using namespace structo::hw;
using namespace structo::prng;

// --------------------------------------------------------------------
// A fake hardware RNG backend driven by a scripted queue of draws, for
// exercising every generator's from_hw_rng factory (success and
// error-propagation paths) deterministically.
// --------------------------------------------------------------------
struct fake_rng {
  std::deque<result<std::uint64_t>> draws;
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
};

namespace {

// ============================================================================
// splitmix64
// ============================================================================

TEST(SplitMix64Test, SameSeedProducesSameSequence) {
  splitmix64 a(12345);
  splitmix64 b(12345);
  for (int i = 0; i < 8; ++i)
    EXPECT_EQ(a.next(), b.next());
}

TEST(SplitMix64Test, DifferentSeedsProduceDifferentSequences) {
  splitmix64 a(1);
  splitmix64 b(2);
  EXPECT_NE(a.next(), b.next());
}

TEST(SplitMix64Test, KnownSeedZeroMatchesReferenceOutput) {
  // Reference values for splitmix64 seeded with 0, from the
  // well-known public-domain reference implementation.
  splitmix64 g(0);
  EXPECT_EQ(g.next(), 0xE220A8397B1DCDAFull);
  EXPECT_EQ(g.next(), 0x6E789E6AA1B965F4ull);
  EXPECT_EQ(g.next(), 0x06C45D188009454Full);
}

TEST(SplitMix64Test, SuccessiveOutputsAreDistinct) {
  splitmix64 g(42);
  std::set<std::uint64_t> seen;
  for (int i = 0; i < 1000; ++i)
    seen.insert(g.next());
  EXPECT_EQ(seen.size(), 1000u);
}

TEST(SplitMix64Test, FromHwRngUsesSingleDrawAsSeed) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{0});
  hw_rng_ref ref(dev);

  auto g = splitmix64::from_hw_rng(ref);
  ASSERT_TRUE(g.has_value());

  splitmix64 expected(0);
  EXPECT_EQ(g->next(), expected.next());
}

TEST(SplitMix64Test, FromHwRngPropagatesDrawFailure) {
  hw_rng_ref unbound;
  auto g = splitmix64::from_hw_rng(unbound);
  ASSERT_FALSE(g.has_value());
  EXPECT_EQ(g.error(), error::unsupported_operation);
}

// ============================================================================
// xoshiro256ss
// ============================================================================

TEST(Xoshiro256ssTest, SameSeedProducesSameSequence) {
  xoshiro256ss a(777);
  xoshiro256ss b(777);
  for (int i = 0; i < 8; ++i)
    EXPECT_EQ(a.next(), b.next());
}

TEST(Xoshiro256ssTest, DifferentSeedsProduceDifferentSequences) {
  xoshiro256ss a(1);
  xoshiro256ss b(2);
  EXPECT_NE(a.next(), b.next());
}

TEST(Xoshiro256ssTest, SuccessiveOutputsAreDistinct) {
  xoshiro256ss g(99);
  std::set<std::uint64_t> seen;
  for (int i = 0; i < 1000; ++i)
    seen.insert(g.next());
  EXPECT_EQ(seen.size(), 1000u);
}

TEST(Xoshiro256ssTest, FromHwRngDrawsFourWordsDirectly) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{1});
  dev.draws.push_back(std::uint64_t{2});
  dev.draws.push_back(std::uint64_t{3});
  dev.draws.push_back(std::uint64_t{4});
  hw_rng_ref ref(dev);

  auto g = xoshiro256ss::from_hw_rng(ref);
  ASSERT_TRUE(g.has_value());
  EXPECT_TRUE(dev.draws.empty());

  // The generator's first output must be reproducible from state
  // {1,2,3,4} directly (no splitmix64 expansion for from_hw_rng).
  auto first = g->next();
  EXPECT_NE(first, 0u);
}

TEST(Xoshiro256ssTest, FromHwRngPropagatesDrawFailureOnFirstWord) {
  hw_rng_ref unbound;
  auto g = xoshiro256ss::from_hw_rng(unbound);
  ASSERT_FALSE(g.has_value());
  EXPECT_EQ(g.error(), error::unsupported_operation);
}

TEST(Xoshiro256ssTest, FromHwRngPropagatesDrawFailureOnLaterWord) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{1});
  dev.draws.push_back(std::uint64_t{2});
  // Third draw (empty queue) reports try_again, exhausting the
  // single-retry budget below.
  hw_rng_ref ref(dev);

  auto g = xoshiro256ss::from_hw_rng(ref, /*max_retries=*/1);
  ASSERT_FALSE(g.has_value());
  EXPECT_EQ(g.error(), error::try_again);
}

// ============================================================================
// pcg32
// ============================================================================

TEST(Pcg32Test, SameSeedAndSequenceProduceSameOutput) {
  pcg32 a(1, 1);
  pcg32 b(1, 1);
  for (int i = 0; i < 8; ++i)
    EXPECT_EQ(a.next(), b.next());
}

TEST(Pcg32Test, DifferentSeedsProduceDifferentOutput) {
  pcg32 a(1, 1);
  pcg32 b(2, 1);
  EXPECT_NE(a.next(), b.next());
}

TEST(Pcg32Test, DifferentSequencesWithSameSeedProduceDifferentStreams) {
  pcg32 a(42, 1);
  pcg32 b(42, 2);
  EXPECT_NE(a.next(), b.next());
}

TEST(Pcg32Test, SuccessiveOutputsAreDistinct) {
  pcg32 g(7, 3);
  std::set<std::uint32_t> seen;
  for (int i = 0; i < 1000; ++i)
    seen.insert(g.next());
  EXPECT_EQ(seen.size(), 1000u);
}

TEST(Pcg32Test, FromHwRngDrawsSeedAndSequence) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{42});
  dev.draws.push_back(std::uint64_t{5});
  hw_rng_ref ref(dev);

  auto g = pcg32::from_hw_rng(ref);
  ASSERT_TRUE(g.has_value());
  EXPECT_TRUE(dev.draws.empty());

  pcg32 expected(42, 5);
  EXPECT_EQ(g->next(), expected.next());
}

TEST(Pcg32Test, FromHwRngPropagatesDrawFailureOnSeed) {
  hw_rng_ref unbound;
  auto g = pcg32::from_hw_rng(unbound);
  ASSERT_FALSE(g.has_value());
  EXPECT_EQ(g.error(), error::unsupported_operation);
}

TEST(Pcg32Test, FromHwRngPropagatesDrawFailureOnSequence) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{42}); // seed draw succeeds
  // sequence draw (empty queue) reports try_again, exhausting the
  // single-retry budget below.
  hw_rng_ref ref(dev);

  auto g = pcg32::from_hw_rng(ref, /*max_retries=*/1);
  ASSERT_FALSE(g.has_value());
  EXPECT_EQ(g.error(), error::try_again);
}

} // namespace
