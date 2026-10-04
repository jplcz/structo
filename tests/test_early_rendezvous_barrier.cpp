// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/sync/early_rendezvous_barrier.h>
#include <structo/sync/early_rendezvous_barrier.hpp>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

using structo::sync::early_rendezvous_wait;

namespace {

// Fake architecture backend: counts how many times spin_wait() was
// called so tests can assert at least one spin happened without relying
// on real wall-clock timing.
struct fake_spin_traits {
  static inline std::atomic<std::size_t> spin_count{0};

  static void spin_wait() noexcept { spin_count.fetch_add(1, std::memory_order_relaxed); }

  static void reset() noexcept { spin_count.store(0, std::memory_order_relaxed); }
};

/** @brief Fixture for `early_rendezvous_wait` tests; resets `fake_spin_traits`'s mutable static state before each test.
 */
class EarlyRendezvousBarrierTest : public ::testing::Test {
protected:
  void SetUp() override { fake_spin_traits::reset(); }
};

} // namespace

TEST_F(EarlyRendezvousBarrierTest, StaticStateIsZeroInitializedWithoutConstruction) {
  static structo_early_rendezvous_state state;
  EXPECT_EQ(state.arrived, 0U);
  EXPECT_EQ(state.generation, 0U);
}

TEST_F(EarlyRendezvousBarrierTest, PlainCApiSingleParticipantIsAlwaysLeaderAndNeverSpins) {
  struct structo_early_rendezvous_state state = STRUCTO_EARLY_RENDEZVOUS_STATE_INIT;
  EXPECT_NE(structo_early_rendezvous_wait(&state, 1, nullptr, nullptr, nullptr), 0);
}

TEST_F(EarlyRendezvousBarrierTest, SingleParticipantIsAlwaysLeaderAndNeverSpins) {
  structo_early_rendezvous_state state{};
  EXPECT_TRUE((early_rendezvous_wait<fake_spin_traits>(state, 1)));
  EXPECT_EQ(fake_spin_traits::spin_count.load(), 0U);
}

TEST_F(EarlyRendezvousBarrierTest, ZeroParticipantsIsTreatedAsOne) {
  structo_early_rendezvous_state state{};
  EXPECT_TRUE((early_rendezvous_wait<fake_spin_traits>(state, 0)));
}

TEST_F(EarlyRendezvousBarrierTest, ExactlyOneLeaderPerWaveAcrossTwoThreads) {
  structo_early_rendezvous_state state{};
  std::atomic<int> leader_count{0};

  std::thread first([&] {
    if (early_rendezvous_wait<fake_spin_traits>(state, 2)) {
      leader_count.fetch_add(1, std::memory_order_relaxed);
    }
  });
  std::thread second([&] {
    if (early_rendezvous_wait<fake_spin_traits>(state, 2)) {
      leader_count.fetch_add(1, std::memory_order_relaxed);
    }
  });

  first.join();
  second.join();

  EXPECT_EQ(leader_count.load(), 1);
}

TEST_F(EarlyRendezvousBarrierTest, NonLeaderInvokesOnSpinCallbackWhileWaiting) {
  structo_early_rendezvous_state state{};
  std::atomic<std::size_t> spin_callback_invocations{0};
  std::atomic<bool> waiter_started{false};

  std::thread waiter([&] {
    waiter_started.store(true, std::memory_order_release);
    const bool leader = early_rendezvous_wait<fake_spin_traits>(
        state, 2, [&] { spin_callback_invocations.fetch_add(1, std::memory_order_relaxed); });
    EXPECT_FALSE(leader);
  });

  while (!waiter_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  // Give the waiter a real chance to spin at least once before releasing it.
  while (fake_spin_traits::spin_count.load(std::memory_order_relaxed) == 0) {
    std::this_thread::yield();
  }

  EXPECT_TRUE((early_rendezvous_wait<fake_spin_traits>(state, 2)));
  waiter.join();

  EXPECT_GT(spin_callback_invocations.load(), 0U);
  EXPECT_GT(fake_spin_traits::spin_count.load(), 0U);
}

TEST_F(EarlyRendezvousBarrierTest, OnSpinCallbackReceivesProvisionalArrivalCountWhenRequested) {
  // The provisional count is documented as a racy lower-bound hint: once
  // the leader arrives, a non-leader's *last* observed value can legally
  // be 0 (read just after the leader resets `arrived` for the next wave)
  // or the leader's own post-increment value, not just 1 -- so this test
  // must assert that the callback *ever* observed the expected mid-wave
  // count of 1, not that it was the final snapshot.
  structo_early_rendezvous_state state{};
  std::atomic<bool> saw_expected_count{false};
  std::atomic<bool> waiter_started{false};

  std::thread waiter([&] {
    waiter_started.store(true, std::memory_order_release);
    early_rendezvous_wait<fake_spin_traits>(state, 2, [&](std::size_t arrived) {
      if (arrived == 1) {
        saw_expected_count.store(true, std::memory_order_relaxed);
      }
    });
  });

  while (!waiter_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  // Only release the leader once the waiter has genuinely observed the
  // expected provisional count at least once, instead of racing against
  // `spin_count` alone (which only proves *a* spin happened, not that it
  // happened with the value this test cares about).
  while (!saw_expected_count.load(std::memory_order_relaxed)) {
    std::this_thread::yield();
  }

  EXPECT_TRUE((early_rendezvous_wait<fake_spin_traits>(state, 2)));
  waiter.join();

  EXPECT_TRUE(saw_expected_count.load());
}

TEST_F(EarlyRendezvousBarrierTest, BarrierIsReusableAcrossMultipleWaves) {
  structo_early_rendezvous_state state{};

  for (int wave = 0; wave < 3; ++wave) {
    std::atomic<int> leader_count{0};

    std::thread first([&] {
      if (early_rendezvous_wait<fake_spin_traits>(state, 2)) {
        leader_count.fetch_add(1, std::memory_order_relaxed);
      }
    });
    std::thread second([&] {
      if (early_rendezvous_wait<fake_spin_traits>(state, 2)) {
        leader_count.fetch_add(1, std::memory_order_relaxed);
      }
    });

    first.join();
    second.join();

    EXPECT_EQ(leader_count.load(), 1);
  }
}

TEST_F(EarlyRendezvousBarrierTest, AllParticipantsAcrossManyThreadsCompleteTheWave) {
  constexpr std::size_t num_threads = 8;
  structo_early_rendezvous_state state{};
  std::atomic<int> leader_count{0};
  std::atomic<int> completed{0};

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (std::size_t i = 0; i < num_threads; ++i) {
    threads.emplace_back([&] {
      if (early_rendezvous_wait<fake_spin_traits>(state, num_threads)) {
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
