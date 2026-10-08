// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <reloco/lifetime.hpp>
#include <structo/arch/cpu_mask.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <vector>

using structo::arch::cpu_mask;
using structo::arch::physical_cpu_tag;
using structo::arch::vcpu_tag;

namespace {

/** @brief Fixture for `cpu_mask` tests. */
class CpuMaskTest : public ::testing::Test {};

using core_mask = cpu_mask<physical_cpu_tag, 70>; // deliberately not a multiple of 64
using small_mask = cpu_mask<physical_cpu_tag, 8>;
using vmask = cpu_mask<vcpu_tag, 70>;

} // namespace

TEST_F(CpuMaskTest, DefaultConstructedIsEmpty) {
  core_mask m;
  EXPECT_TRUE(m.none());
  EXPECT_FALSE(m.any());
  EXPECT_FALSE(m.all());
  EXPECT_EQ(m.count(), 0u);
  EXPECT_EQ(m, core_mask::empty());
}

TEST_F(CpuMaskTest, FilledSetsEveryCpuAndNoneBeyond) {
  core_mask m = core_mask::filled();
  EXPECT_TRUE(m.all());
  EXPECT_EQ(m.count(), core_mask::max_cpus);
  for (std::size_t cpu = 0; cpu < core_mask::max_cpus; ++cpu) {
    EXPECT_TRUE(m.test(cpu)) << cpu;
  }
}

TEST_F(CpuMaskTest, FilledTailPaddingDoesNotLeakIntoWordOrComplement) {
  // max_cpus == 70 is not a multiple of 64: the second word only has 6
  // valid bits. Complementing an empty mask must not set any of the
  // padding bits in that word.
  core_mask m = core_mask::filled();
  core_mask complement_of_filled = m.complement();
  EXPECT_TRUE(complement_of_filled.none());

  core_mask empty;
  core_mask complement_of_empty = empty.complement();
  EXPECT_EQ(complement_of_empty.count(), core_mask::max_cpus);
}

TEST_F(CpuMaskTest, SingleSetsExactlyOneBit) {
  core_mask m = core_mask::single(5);
  EXPECT_EQ(m.count(), 1u);
  EXPECT_TRUE(m.test(5));
  EXPECT_FALSE(m.test(4));
  EXPECT_FALSE(m.test(6));
}

TEST_F(CpuMaskTest, TrySingleRejectsOutOfRangeCpu) {
  EXPECT_FALSE(core_mask::try_single(core_mask::max_cpus).has_value());
  EXPECT_TRUE(core_mask::try_single(core_mask::max_cpus - 1).has_value());
}

#if !defined(NDEBUG) || defined(RELOCO_ENABLE_ASSERTS)
TEST_F(CpuMaskTest, SingleTrapsOnOutOfRangeCpu) {
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ (void)core_mask::single(core_mask::max_cpus); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}
#endif

TEST_F(CpuMaskTest, SetClearTestToggleRoundTrip) {
  core_mask m;
  m.set(3);
  EXPECT_TRUE(m.test(3));
  m.toggle(3);
  EXPECT_FALSE(m.test(3));
  m.toggle(3);
  EXPECT_TRUE(m.test(3));
  m.clear(3);
  EXPECT_FALSE(m.test(3));
  EXPECT_TRUE(m.none());
}

TEST_F(CpuMaskTest, FallibleAccessorsReportOutOfRangeCpu) {
  core_mask m;
  EXPECT_FALSE(m.try_set(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.try_clear(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.try_toggle(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.try_test(core_mask::max_cpus).has_value());

  ASSERT_TRUE(m.try_set(1).has_value());
  auto test_res = m.try_test(1);
  ASSERT_TRUE(test_res.has_value());
  EXPECT_TRUE(test_res.value());
}

TEST_F(CpuMaskTest, WordAccessorsRoundTrip) {
  core_mask m;
  m.set(0);
  m.set(63);
  m.set(64);
  EXPECT_EQ(m.word(0), (std::uint64_t{1} << 0) | (std::uint64_t{1} << 63));
  EXPECT_EQ(m.word(1), std::uint64_t{1});
  EXPECT_FALSE(m.try_word(core_mask::word_count).has_value());
}

TEST_F(CpuMaskTest, WordsSpanDecaysOverEveryBackingWord) {
  core_mask m;
  m.set(0);
  m.set(63);
  m.set(64);

  auto words = m.words();
  ASSERT_EQ(words.size(), core_mask::word_count);
  EXPECT_EQ(words[0], m.word(0));
  EXPECT_EQ(words[1], m.word(1));
}

TEST_F(CpuMaskTest, UnionIntersectionDifferenceSymmetricDifference) {
  core_mask a;
  a.set(1);
  a.set(2);
  a.set(3);
  core_mask b;
  b.set(2);
  b.set(3);
  b.set(4);

  core_mask u = a.union_with(b);
  EXPECT_EQ(u.count(), 4u);
  for (std::size_t cpu : {1u, 2u, 3u, 4u}) {
    EXPECT_TRUE(u.test(cpu));
  }

  core_mask i = a.intersection(b);
  EXPECT_EQ(i.count(), 2u);
  EXPECT_TRUE(i.test(2));
  EXPECT_TRUE(i.test(3));

  core_mask d = a.difference(b);
  EXPECT_EQ(d.count(), 1u);
  EXPECT_TRUE(d.test(1));

  core_mask sd = a.symmetric_difference(b);
  EXPECT_EQ(sd.count(), 2u);
  EXPECT_TRUE(sd.test(1));
  EXPECT_TRUE(sd.test(4));

  EXPECT_EQ(u, a | b);
  EXPECT_EQ(i, a & b);
  EXPECT_EQ(d, a - b);
  EXPECT_EQ(sd, a ^ b);
}

TEST_F(CpuMaskTest, InPlaceOperators) {
  core_mask a;
  a.set(1);
  core_mask b;
  b.set(2);

  core_mask c = a;
  c |= b;
  EXPECT_EQ(c, a | b);

  core_mask d = a | b;
  d &= b;
  EXPECT_EQ(d, b);

  core_mask e = a | b;
  e -= b;
  EXPECT_EQ(e, a);

  core_mask f = a;
  f ^= b;
  EXPECT_EQ(f, a | b);
}

TEST_F(CpuMaskTest, ContainsAndIntersects) {
  core_mask a;
  a.set(1);
  a.set(2);
  a.set(3);

  core_mask subset = core_mask::single(1) | core_mask::single(2);
  EXPECT_TRUE(a.contains(subset));
  EXPECT_TRUE(subset.intersects(a));

  core_mask disjoint = core_mask::single(10);
  EXPECT_FALSE(a.contains(disjoint));
  EXPECT_FALSE(a.intersects(disjoint));
}

TEST_F(CpuMaskTest, LowestAndHighestSet) {
  core_mask m;
  EXPECT_FALSE(m.lowest_set().has_value());
  EXPECT_FALSE(m.highest_set().has_value());

  m.set(5);
  m.set(69);
  m.set(30);

  ASSERT_TRUE(m.lowest_set().has_value());
  EXPECT_EQ(m.lowest_set().value(), 5u);
  ASSERT_TRUE(m.highest_set().has_value());
  EXPECT_EQ(m.highest_set().value(), 69u);

  ASSERT_TRUE(m.lowest_set_from(6).has_value());
  EXPECT_EQ(m.lowest_set_from(6).value(), 30u);
  EXPECT_FALSE(m.lowest_set_from(70).has_value());
}

TEST_F(CpuMaskTest, IterationYieldsSetBitsAscending) {
  core_mask m;
  m.set(5);
  m.set(0);
  m.set(69);
  m.set(64);

  std::vector<std::size_t> seen;
  for (std::size_t cpu : m) {
    seen.push_back(cpu);
  }
  EXPECT_EQ(seen, (std::vector<std::size_t>{0, 5, 64, 69}));
}

TEST_F(CpuMaskTest, IterationOverEmptyMaskYieldsNothing) {
  core_mask m;
  std::size_t iterations = 0;
  for ([[maybe_unused]] std::size_t cpu : m) {
    ++iterations;
  }
  EXPECT_EQ(iterations, 0u);
}

TEST_F(CpuMaskTest, IterationOverFilledMaskYieldsEveryIndex) {
  core_mask m = core_mask::filled();
  std::size_t expected = 0;
  for (std::size_t cpu : m) {
    EXPECT_EQ(cpu, expected);
    ++expected;
  }
  EXPECT_EQ(expected, core_mask::max_cpus);
}

TEST_F(CpuMaskTest, EqualityAndInequality) {
  core_mask a;
  a.set(1);
  core_mask b;
  b.set(1);
  core_mask c;
  c.set(2);

  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
}

TEST_F(CpuMaskTest, TaggedTypesAreDistinctAcrossTags) {
  static_assert(!std::is_same_v<core_mask, vmask>);
  static_assert(std::is_same_v<core_mask::tag_type, physical_cpu_tag>);
  static_assert(std::is_same_v<vmask::tag_type, vcpu_tag>);
}

TEST_F(CpuMaskTest, ExactMultipleOfWordSizeHasNoTailPadding) {
  using exact_mask = cpu_mask<physical_cpu_tag, 64>;
  exact_mask m = exact_mask::filled();
  EXPECT_EQ(m.word(0), ~std::uint64_t{0});
  EXPECT_EQ(m.count(), 64u);
}

TEST_F(CpuMaskTest, SmallMaskBelowOneWordWorks) {
  small_mask m;
  m.set(0);
  m.set(7);
  EXPECT_EQ(small_mask::word_count, 1u);
  EXPECT_EQ(m.count(), 2u);
  EXPECT_TRUE(small_mask::filled().all());
}

TEST_F(CpuMaskTest, ConstexprUsageCompiles) {
#if __cplusplus >= 202002L
  constexpr core_mask c = core_mask::single(5);
  static_assert(c.test(5));
  static_assert(!c.test(6));
  static_assert(core_mask::filled().all());
#else
  GTEST_SKIP() << "RELOCO_CONSTEXPR20 accessors require C++20 or newer";
#endif
}

TEST_F(CpuMaskTest, AtomicTestSetClearToggleRoundTrip) {
  core_mask m;
  EXPECT_FALSE(m.atomic_test(5));
  m.atomic_set(5);
  EXPECT_TRUE(m.atomic_test(5));
  m.atomic_clear(5);
  EXPECT_FALSE(m.atomic_test(5));
  m.atomic_toggle(5);
  EXPECT_TRUE(m.atomic_test(5));
  m.atomic_toggle(5);
  EXPECT_FALSE(m.atomic_test(5));
}

TEST_F(CpuMaskTest, AtomicTestAndSetReturnsPreviousState) {
  core_mask m;
  EXPECT_FALSE(m.atomic_test_and_set(5));
  EXPECT_TRUE(m.atomic_test(5));
  EXPECT_TRUE(m.atomic_test_and_set(5)); // already set
}

TEST_F(CpuMaskTest, AtomicTestAndClearReturnsPreviousState) {
  core_mask m;
  m.atomic_set(5);
  EXPECT_TRUE(m.atomic_test_and_clear(5));
  EXPECT_FALSE(m.atomic_test(5));
  EXPECT_FALSE(m.atomic_test_and_clear(5)); // already clear
}

TEST_F(CpuMaskTest, AtomicTestAndToggleReturnsPreviousState) {
  core_mask m;
  EXPECT_FALSE(m.atomic_test_and_toggle(5));
  EXPECT_TRUE(m.atomic_test(5));
  EXPECT_TRUE(m.atomic_test_and_toggle(5));
  EXPECT_FALSE(m.atomic_test(5));
}

TEST_F(CpuMaskTest, AtomicFallibleAccessorsReportOutOfRangeCpu) {
  core_mask m;
  EXPECT_FALSE(m.atomic_try_test(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.atomic_try_set(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.atomic_try_clear(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.atomic_try_toggle(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.atomic_try_test_and_set(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.atomic_try_test_and_clear(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.atomic_try_test_and_toggle(core_mask::max_cpus).has_value());
  EXPECT_FALSE(m.atomic_try_word(core_mask::word_count).has_value());

  ASSERT_TRUE(m.atomic_try_set(3).has_value());
  auto test_res = m.atomic_try_test(3);
  ASSERT_TRUE(test_res.has_value());
  EXPECT_TRUE(test_res.value());
}

#if !defined(NDEBUG) || defined(RELOCO_ENABLE_ASSERTS)
TEST_F(CpuMaskTest, AtomicSetTrapsOnOutOfRangeCpu) {
  core_mask m;
  // gtest's death-test macro expands to libc fprintf calls outside our control.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  EXPECT_DEATH({ m.atomic_set(core_mask::max_cpus); }, "");
  RELOCO_END_UNSAFE_BUFFER_USAGE
}
#endif

TEST_F(CpuMaskTest, AtomicWordLoadsBackingWord) {
  core_mask m;
  m.set(0);
  m.set(63);
  EXPECT_EQ(m.atomic_word(0), (std::uint64_t{1} << 0) | (std::uint64_t{1} << 63));
}

TEST_F(CpuMaskTest, AtomicOperationsAreConcurrencySafeAcrossThreads) {
  // Each thread repeatedly toggles its own bit via test_and_set/
  // test_and_clear; bits are packed so several threads share a word,
  // exercising real read-modify-write contention on the same word.
  core_mask shared;
  constexpr int num_threads = 8;
  constexpr int iterations = 2000;
  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&shared, t]() {
      auto cpu = static_cast<std::size_t>(t);
      for (int i = 0; i < iterations; ++i) {
        shared.atomic_test_and_set(cpu);
        shared.atomic_test_and_clear(cpu);
        shared.atomic_test_and_set(cpu);
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }
  for (int t = 0; t < num_threads; ++t) {
    EXPECT_TRUE(shared.atomic_test(static_cast<std::size_t>(t)));
  }
  EXPECT_EQ(shared.count(), static_cast<std::size_t>(num_threads));
}

TEST_F(CpuMaskTest, AtomicLowestSetFindsFirstSetBit) {
  core_mask m;
  EXPECT_FALSE(m.atomic_lowest_set().has_value());
  m.set(5);
  m.set(69);
  ASSERT_TRUE(m.atomic_lowest_set().has_value());
  EXPECT_EQ(m.atomic_lowest_set().value(), 5u);
  ASSERT_TRUE(m.atomic_lowest_set_from(6).has_value());
  EXPECT_EQ(m.atomic_lowest_set_from(6).value(), 69u);
  EXPECT_FALSE(m.atomic_lowest_set_from(70).has_value());
  EXPECT_FALSE(m.atomic_lowest_set_from(core_mask::max_cpus).has_value());
}

TEST_F(CpuMaskTest, AtomicFindAndSetBehavesLikeABitmapAllocator) {
  core_mask m;
  auto first = m.atomic_find_and_set();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first.value(), 0u);
  auto second = m.atomic_find_and_set();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second.value(), 1u);

  m.atomic_clear(0);
  auto third = m.atomic_find_and_set();
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(third.value(), 0u); // reclaims the freed slot before allocating new ones
}

TEST_F(CpuMaskTest, AtomicFindAndSetExhaustsEveryCpuThenFails) {
  core_mask m;
  for (std::size_t i = 0; i < core_mask::max_cpus; ++i) {
    auto got = m.atomic_find_and_set();
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got.value(), i);
  }
  EXPECT_FALSE(m.atomic_find_and_set().has_value());
}

TEST_F(CpuMaskTest, AtomicFindAndSetConcurrentClaimsAreUniqueAndExhaustive) {
  // Many threads racing `atomic_find_and_set()` against a shared mask
  // must each claim a distinct CPU index, and together must claim every
  // index exactly once -- this is the compare-and-swap retry loop's
  // actual correctness property, not just absence of a crash.
  core_mask shared;
  constexpr int num_threads = 8;
  std::vector<std::thread> threads;
  std::vector<std::vector<std::size_t>> claimed(static_cast<std::size_t>(num_threads));
  for (int t = 0; t < num_threads; ++t) {
    threads.emplace_back([&shared, &claimed, t]() {
      for (;;) {
        auto cpu = shared.atomic_find_and_set();
        if (!cpu.has_value()) {
          break;
        }
        claimed[static_cast<std::size_t>(t)].push_back(cpu.value());
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }

  std::vector<bool> seen(core_mask::max_cpus, false);
  std::size_t total = 0;
  for (auto &per_thread : claimed) {
    for (auto cpu : per_thread) {
      EXPECT_FALSE(seen[cpu]) << "cpu " << cpu << " claimed more than once";
      seen[cpu] = true;
      ++total;
    }
  }
  EXPECT_EQ(total, core_mask::max_cpus);
  EXPECT_EQ(shared.count(), core_mask::max_cpus);
}
