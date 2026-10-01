// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/core_rendezvous_barrier.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

using structo::sync::core_rendezvous_barrier;

namespace {

// Fake architecture backend: counts how many times spin_wait() was
// called so tests can assert at least one spin happened without relying
// on real wall-clock timing.
struct fake_spin_traits {
  static inline std::atomic<std::size_t> spin_count{0};

  static void spin_wait() noexcept { spin_count.fetch_add(1, std::memory_order_relaxed); }

  static void reset() noexcept { spin_count.store(0, std::memory_order_relaxed); }
};

/** @brief Fixture for `core_rendezvous_barrier` tests; resets `fake_spin_traits`'s mutable static state before each test. */
class CoreRendezvousBarrierTest : public ::testing::Test {
protected:
  void SetUp() override { fake_spin_traits::reset(); }
};

} // namespace

TEST_F(CoreRendezvousBarrierTest, SingleParticipantIsAlwaysLeaderAndNeverSpins) {
  core_rendezvous_barrier<fake_spin_traits> barrier(1);
  EXPECT_EQ(barrier.threshold(), 1U);
  EXPECT_TRUE(barrier.wait());
  EXPECT_EQ(fake_spin_traits::spin_count.load(), 0U);
}

TEST_F(CoreRendezvousBarrierTest, ZeroParticipantsIsTreatedAsOne) {
  core_rendezvous_barrier<fake_spin_traits> barrier(0);
  EXPECT_EQ(barrier.threshold(), 1U);
  EXPECT_TRUE(barrier.wait());
}

TEST_F(CoreRendezvousBarrierTest, ExactlyOneLeaderPerWaveAcrossTwoThreads) {
  core_rendezvous_barrier<fake_spin_traits> barrier(2);
  std::atomic<int> leader_count{0};

  std::thread first([&] {
    if (barrier.wait()) {
      leader_count.fetch_add(1, std::memory_order_relaxed);
    }
  });
  std::thread second([&] {
    if (barrier.wait()) {
      leader_count.fetch_add(1, std::memory_order_relaxed);
    }
  });

  first.join();
  second.join();

  EXPECT_EQ(leader_count.load(), 1);
}

TEST_F(CoreRendezvousBarrierTest, NonLeaderInvokesOnSpinCallbackWhileWaiting) {
  core_rendezvous_barrier<fake_spin_traits> barrier(2);
  std::atomic<std::size_t> spin_callback_invocations{0};
  std::atomic<bool> waiter_started{false};

  std::thread waiter([&] {
    waiter_started.store(true, std::memory_order_release);
    const bool leader = barrier.wait([&] { spin_callback_invocations.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_FALSE(leader);
  });

  while (!waiter_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  // Give the waiter a real chance to spin at least once before releasing it.
  while (fake_spin_traits::spin_count.load(std::memory_order_relaxed) == 0) {
    std::this_thread::yield();
  }

  EXPECT_TRUE(barrier.wait());
  waiter.join();

  EXPECT_GT(spin_callback_invocations.load(), 0U);
  EXPECT_GT(fake_spin_traits::spin_count.load(), 0U);
}

TEST_F(CoreRendezvousBarrierTest, OnSpinCallbackReceivesProvisionalArrivalCountWhenRequested) {
  core_rendezvous_barrier<fake_spin_traits> barrier(2);
  std::atomic<std::size_t> last_seen_count{0};
  std::atomic<bool> waiter_started{false};

  std::thread waiter([&] {
    waiter_started.store(true, std::memory_order_release);
    barrier.wait([&](std::size_t arrived) { last_seen_count.store(arrived, std::memory_order_relaxed); });
  });

  while (!waiter_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  while (fake_spin_traits::spin_count.load(std::memory_order_relaxed) == 0) {
    std::this_thread::yield();
  }

  EXPECT_TRUE(barrier.wait());
  waiter.join();

  EXPECT_EQ(last_seen_count.load(), 1U);
}

TEST_F(CoreRendezvousBarrierTest, BarrierIsReusableAcrossMultipleWaves) {
  core_rendezvous_barrier<fake_spin_traits> barrier(2);

  for (int wave = 0; wave < 3; ++wave) {
    std::atomic<int> leader_count{0};

    std::thread first([&] {
      if (barrier.wait()) {
        leader_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
    std::thread second([&] {
      if (barrier.wait()) {
        leader_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    first.join();
    second.join();

    EXPECT_EQ(leader_count.load(), 1);
  }
}

TEST_F(CoreRendezvousBarrierTest, AllParticipantsAcrossManyThreadsCompleteTheWave) {
  constexpr std::size_t num_threads = 8;
  core_rendezvous_barrier<fake_spin_traits> barrier(num_threads);
  std::atomic<int> leader_count{0};
  std::atomic<int> completed{0};

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) {
    threads.emplace_back([&] {
      if (barrier.wait()) {
        leader_count.fetch_add(1, std::memory_order_relaxed);
      }
      completed.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (auto &t : threads) {
    t.join();
  }

  EXPECT_EQ(leader_count.load(), 1);
  EXPECT_EQ(completed.load(), static_cast<int>(num_threads));
}
