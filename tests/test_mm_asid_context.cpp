// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/mm_asid_context.hpp>

#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

using structo::arch::asid_allocator;
using structo::arch::mm_asid_context;
using structo::arch::physical_cpu_tag;
using structo::arch::process_asid_tag;

namespace {

class MmAsidContextTest : public ::testing::Test {};

using proc_allocator = asid_allocator<process_asid_tag, 8>;
using mm_context = mm_asid_context<process_asid_tag, 64>;

} // namespace

TEST_F(MmAsidContextTest, DefaultConstructedIsInvalidWithEmptyCpuTargets) {
  mm_context mm;
  EXPECT_FALSE(mm.is_valid());
  EXPECT_FALSE(mm.asid().is_valid());
  EXPECT_TRUE(mm.cpu_targets().none());
}

TEST_F(MmAsidContextTest, ActivateAllocatesAsidAndRecordsCpuBit) {
  auto maker = proc_allocator::try_create(4); // 16 ASIDs
  ASSERT_TRUE(maker.has_value());
  auto allocator = std::move(maker.value());

  mm_context mm;
  auto res = mm.activate(allocator, 0);
  ASSERT_TRUE(res.has_value());
  EXPECT_FALSE(res.value()); // first allocation: no rollover
  EXPECT_TRUE(mm.is_valid());
  EXPECT_TRUE(mm.cpu_targets().test(0));
  EXPECT_EQ(mm.cpu_targets().count(), 1u);
}

TEST_F(MmAsidContextTest, ReactivatingOnSameCpuReusesSameAsid) {
  auto maker = proc_allocator::try_create(4);
  ASSERT_TRUE(maker.has_value());
  auto allocator = std::move(maker.value());

  mm_context mm;
  ASSERT_TRUE(mm.activate(allocator, 0).has_value());
  auto first_asid = mm.asid();

  auto res = mm.activate(allocator, 0);
  ASSERT_TRUE(res.has_value());
  EXPECT_FALSE(res.value());
  EXPECT_EQ(mm.asid(), first_asid);
}

TEST_F(MmAsidContextTest, ActivatingOnMultipleCpusAccumulatesCpuTargets) {
  auto maker = proc_allocator::try_create(4);
  ASSERT_TRUE(maker.has_value());
  auto allocator = std::move(maker.value());

  mm_context mm;
  ASSERT_TRUE(mm.activate(allocator, 0).has_value());
  ASSERT_TRUE(mm.activate(allocator, 1).has_value());
  ASSERT_TRUE(mm.activate(allocator, 2).has_value());

  auto targets = mm.cpu_targets();
  EXPECT_TRUE(targets.test(0));
  EXPECT_TRUE(targets.test(1));
  EXPECT_TRUE(targets.test(2));
  EXPECT_EQ(targets.count(), 3u);
}

TEST_F(MmAsidContextTest, ActivateRejectsOutOfRangeSlot) {
  auto maker = proc_allocator::try_create(4);
  ASSERT_TRUE(maker.has_value());
  auto allocator = std::move(maker.value());

  mm_context mm;
  auto res = mm.activate(allocator, proc_allocator::max_active);
  EXPECT_FALSE(res.has_value());
}

TEST_F(MmAsidContextTest, ReleaseInvalidatesAsidButLeavesCpuTargetsUntouched) {
  auto maker = proc_allocator::try_create(4);
  ASSERT_TRUE(maker.has_value());
  auto allocator = std::move(maker.value());

  mm_context mm;
  ASSERT_TRUE(mm.activate(allocator, 0).has_value());
  ASSERT_TRUE(mm.activate(allocator, 1).has_value());

  mm.release(allocator);
  EXPECT_FALSE(mm.is_valid());
  // Deliberately left alone by design (see release()'s doc comment) --
  // matches asid_allocator's own "deactivation is a no-op" semantics.
  EXPECT_EQ(mm.cpu_targets().count(), 2u);
}

TEST_F(MmAsidContextTest, ClearCpuTargetsResetsForRecycling) {
  auto maker = proc_allocator::try_create(4);
  ASSERT_TRUE(maker.has_value());
  auto allocator = std::move(maker.value());

  mm_context mm;
  ASSERT_TRUE(mm.activate(allocator, 0).has_value());
  mm.release(allocator);
  mm.clear_cpu_targets();
  EXPECT_TRUE(mm.cpu_targets().none());
}

TEST_F(MmAsidContextTest, RolloverIsSurfacedThroughActivate) {
  // 4 asid bits -> 16 ASIDs, MaxActive == 8: exhausting every non-active
  // ASID across many distinct mm_contexts forces a generation rollover.
  auto maker = proc_allocator::try_create(4);
  ASSERT_TRUE(maker.has_value());
  auto allocator = std::move(maker.value());

  std::vector<mm_context> contexts(20);
  bool saw_rollover = false;
  for (std::size_t i = 0; i < contexts.size(); ++i) {
    auto res = contexts[i].activate(allocator, i % proc_allocator::max_active);
    ASSERT_TRUE(res.has_value());
    if (res.value()) {
      saw_rollover = true;
    }
  }
  EXPECT_TRUE(saw_rollover);
}

TEST_F(MmAsidContextTest, ConcurrentActivationsUnderExternalLockAndLockFreeCpuTargetsReads) {
  // Mirrors the real usage contract: activate()/release() serialized by
  // an external lock around the shared allocator (here, a std::mutex
  // standing in for `irq_locked<asid_allocator<...>>` + a cross-core
  // spinlock), while cpu_targets() is read concurrently, lock-free, from
  // a separate thread the whole time.
  auto maker = proc_allocator::try_create(4);
  ASSERT_TRUE(maker.has_value());
  auto allocator = std::move(maker.value());

  std::mutex alloc_mutex;
  mm_context shared_mm;
  std::atomic<bool> stop{false};

  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      // A snapshot taken concurrently with in-flight activations must
      // never exceed the final, fully-settled cpu count -- the mask only
      // ever grows monotonically, bit by bit.
      auto snap = shared_mm.cpu_targets();
      EXPECT_LE(snap.count(), 4u);
    }
  });

  constexpr int num_threads = 4;
  std::vector<std::thread> activators;
  activators.reserve(num_threads);
  for (int t = 0; t < num_threads; ++t) {
    activators.emplace_back([&, t] {
      for (int i = 0; i < 200; ++i) {
        std::lock_guard<std::mutex> lk(alloc_mutex);
        auto res = shared_mm.activate(allocator, static_cast<std::size_t>(t));
        EXPECT_TRUE(res.has_value());
      }
    });
  }
  for (auto &th : activators) {
    th.join();
  }
  stop.store(true, std::memory_order_relaxed);
  reader.join();

  EXPECT_EQ(shared_mm.cpu_targets().count(), static_cast<std::size_t>(num_threads));
}
