// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/time_manager.hpp>
#include <structo/hw/vdso_clock_reader.hpp>

using reloco::duration;
using reloco::error;
using reloco::result;
using structo::hw::cycles;
using structo::hw::time_manager;
using structo::hw::time_source_capabilities;
using structo::hw::time_source_ref;
using structo::hw::vdso_clock_id;
using structo::hw::vdso_clock_page;
using structo::hw::vdso_clock_reader;
using structo::hw::vdso_clock_source;

namespace {

// --------------------------------------------------------------------
// A fully scriptable free-running counter backend: a settable raw
// value/frequency/wraparound bound and monotonic/per-CPU flags, for
// exercising every time_manager edge case deterministically.
// --------------------------------------------------------------------
struct fake_backend {
  std::uint64_t value = 0;
  std::uint64_t clock_hz = 1'000'000'000ULL;
  std::uint64_t max_value = UINT64_MAX;
  bool monotonic = true;
  bool per_cpu = false;
};

} // namespace

template <> struct structo::hw::time_source_traits<fake_backend> {
  static result<cycles> try_now(fake_backend &b) noexcept { return cycles{b.value}; }
  static time_source_capabilities capabilities(fake_backend &b) noexcept {
    time_source_capabilities caps;
    caps.clock_hz = b.clock_hz;
    caps.max_value = cycles{b.max_value};
    caps.is_monotonic = b.monotonic;
    caps.is_per_cpu = b.per_cpu;
    return caps;
  }
};

namespace {

/** @brief Fixture providing a fresh `fake_backend` and its bound `time_source_ref` for each test. */
class TimeManagerTest : public ::testing::Test {
protected:
  fake_backend backend_;
  time_source_ref ref_{backend_};
};

TEST_F(TimeManagerTest, TryCreateFailsOnUnboundCounter) {
  time_source_ref unbound;
  auto mgr = time_manager::try_create(unbound, vdso_clock_source::x86_tsc);
  ASSERT_FALSE(mgr.has_value());
  EXPECT_EQ(mgr.error(), error::unsupported_operation);
}

TEST_F(TimeManagerTest, TryCreateFailsOnZeroClockHz) {
  backend_.clock_hz = 0;
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_FALSE(mgr.has_value());
  EXPECT_EQ(mgr.error(), error::invalid_argument);
}

TEST_F(TimeManagerTest, TryCreateRejectsVdsoPageForUnstableCounter) {
  backend_.monotonic = false;
  vdso_clock_page page;
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc, &page);
  ASSERT_FALSE(mgr.has_value());
  EXPECT_EQ(mgr.error(), error::invalid_argument);
}

TEST_F(TimeManagerTest, TryCreateRejectsVdsoPageForPerCpuCounter) {
  backend_.per_cpu = true;
  vdso_clock_page page;
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc, &page);
  ASSERT_FALSE(mgr.has_value());
  EXPECT_EQ(mgr.error(), error::invalid_argument);
}

TEST_F(TimeManagerTest, TryCreateSucceedsWithoutVdsoPageEvenForUnstablePerCpuCounter) {
  backend_.monotonic = false;
  backend_.per_cpu = true;
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  EXPECT_TRUE(mgr.has_value());
}

TEST_F(TimeManagerTest, ReadsBeforeStartFailWithNotInitialized) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  auto now = mgr->try_monotonic_now();
  ASSERT_FALSE(now.has_value());
  EXPECT_EQ(now.error(), error::not_initialized);
}

TEST_F(TimeManagerTest, StartThenResyncAdvancesMonotonicAndRealtime) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  backend_.value = 1'000'000'000ULL; // +1s
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 1u);

  ASSERT_TRUE(mgr->set_realtime(duration::from_secs(1'700'000'000ULL)));
  EXPECT_EQ(mgr->try_realtime_now().value().as_secs(), 1'700'000'000ULL);

  backend_.value = 2'000'000'000ULL; // +1 more second
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 2u);
  EXPECT_EQ(mgr->try_realtime_now().value().as_secs(), 1'700'000'001ULL);
}

TEST_F(TimeManagerTest, VdsoPageIsReadableThroughVdsoClockReader) {
  vdso_clock_page page;
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc, &page);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  backend_.value = 5'000'000'000ULL;
  ASSERT_TRUE(mgr->resync());

  struct reader_traits {
    static std::uint64_t read_counter(vdso_clock_source) noexcept { return 5'000'000'000ULL; }
  };
  auto v = vdso_clock_reader<reader_traits>::try_now(page.slot(vdso_clock_id::monotonic));
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v.value().as_secs(), 5u);
}

TEST_F(TimeManagerTest, LegitimateNarrowWraparoundIsHandledCorrectly) {
  backend_.max_value = 999; // period 1000, half_range 500
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::riscv_time);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  backend_.value = 900;
  ASSERT_TRUE(mgr->resync());
  backend_.value = 50; // wrapped: elapsed = (1000-900)+50 = 150, well under half_range(500)
  ASSERT_TRUE(mgr->resync());

  EXPECT_EQ(mgr->try_monotonic_now().value().as_nanos(), 1050u);
  EXPECT_EQ(mgr->backstep_count(), 0u);
}

TEST_F(TimeManagerTest, UnstableCounterBackstepClampsThenRecovers) {
  backend_.monotonic = false;
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  backend_.value = 1'000'000'000ULL; // +1s
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 1u);

  backend_.value = 500'000'000ULL; // backstep: must clamp, not regress
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->backstep_count(), 1u);
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 1u);

  backend_.value = 2'000'000'000ULL; // recovers above the high-water mark (1e9): resumes advancing
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 2u);
}

TEST_F(TimeManagerTest, ResetToZeroOnFullWidthCounterClampsInsteadOfBogusForwardJump) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  backend_.value = 5'000'000'000'000ULL; // 5000s of "uptime"
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 5000u);

  backend_.value = 0; // simulated power-off/power-on reset
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->backstep_count(), 1u); // flagged as untrusted, not a genuine wrap
  // Must stay pinned, never jump to ~2^64 ns (hundreds of years).
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 5000u);
}

TEST_F(TimeManagerTest, LiveReadBackstepClampsAndIncrementsLiveDiagnosticWithoutErroring) {
  backend_.monotonic = false;
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  backend_.value = 1'000'000'000ULL;
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->live_backstep_count(), 0u);

  backend_.value = 100; // live read dips below the published reference
  auto now = mgr->try_monotonic_now();
  ASSERT_TRUE(now.has_value());         // must not error
  EXPECT_EQ(now.value().as_secs(), 1u); // pinned
  EXPECT_EQ(mgr->live_backstep_count(), 1u);
  EXPECT_EQ(mgr->backstep_count(), 0u); // independent of the writer-path diagnostic

  backend_.value = 1'500'000'000ULL; // recovers above the reference: resumes advancing, no counter bump
  auto recovered = mgr->try_monotonic_now();
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered.value().as_millis(), 1500u);
  EXPECT_EQ(mgr->live_backstep_count(), 1u);
}

TEST_F(TimeManagerTest, MaxResyncIntervalIsHalfTheFullWrapPeriod) {
  backend_.max_value = 2'000'000'000ULL; // period == 2e9 cycles @ 1e9 Hz == 2s; half == 1s
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  auto interval = mgr->max_resync_interval();
  ASSERT_TRUE(interval.has_value());
  EXPECT_EQ(interval.value().as_secs(), 1u);
}

TEST_F(TimeManagerTest, SwitchSourcePreservesMonotonicContinuityAcrossAFrequencyChange) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  backend_.value = 1'000'000'000ULL; // +1s
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 1u);

  fake_backend new_backend;
  new_backend.clock_hz = 2'000'000'000ULL; // 2 GHz
  new_backend.value = 500;
  time_source_ref new_ref(new_backend);

  ASSERT_TRUE(mgr->switch_source(new_ref, vdso_clock_source::riscv_time));
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 1u); // no jump across the switch

  new_backend.value = 500 + 2'000'000'000ULL; // +1s @ 2GHz
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 2u);
}

TEST_F(TimeManagerTest, SwitchSourceRejectsUnboundCounter) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  time_source_ref unbound;
  auto sw = mgr->switch_source(unbound, vdso_clock_source::arm_cntvct_el0);
  ASSERT_FALSE(sw.has_value());
  EXPECT_EQ(sw.error(), error::unsupported_operation);
}

TEST_F(TimeManagerTest, SwitchSourceRejectsVdsoIncompatibleCounterAndLeavesManagerUsable) {
  vdso_clock_page page;
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc, &page);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());

  fake_backend bad_backend;
  bad_backend.per_cpu = true;
  time_source_ref bad_ref(bad_backend);
  auto sw = mgr->switch_source(bad_ref, vdso_clock_source::arm_cntpct_el0);
  ASSERT_FALSE(sw.has_value());
  EXPECT_EQ(sw.error(), error::invalid_argument);

  // The manager must remain bound to the original counter after a rejected switch.
  backend_.value = 1'000'000'000ULL;
  ASSERT_TRUE(mgr->resync());
  EXPECT_EQ(mgr->try_monotonic_now().value().as_secs(), 1u);
}

TEST_F(TimeManagerTest, SwitchSourceRequiresStart) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  // start() deliberately not called.

  fake_backend other;
  time_source_ref other_ref(other);
  auto sw = mgr->switch_source(other_ref, vdso_clock_source::riscv_time);
  ASSERT_FALSE(sw.has_value());
  EXPECT_EQ(sw.error(), error::not_initialized);
}

TEST_F(TimeManagerTest, MoveConstructionTransfersStateAndKeepsWorking) {
  auto mgr = time_manager::try_create(ref_, vdso_clock_source::x86_tsc);
  ASSERT_TRUE(mgr.has_value());
  ASSERT_TRUE(mgr->start());
  backend_.value = 1'000'000'000ULL;
  ASSERT_TRUE(mgr->resync());

  time_manager moved(std::move(mgr.value()));
  EXPECT_EQ(moved.try_monotonic_now().value().as_secs(), 1u);

  backend_.value = 2'000'000'000ULL;
  ASSERT_TRUE(moved.resync());
  EXPECT_EQ(moved.try_monotonic_now().value().as_secs(), 2u);
}

} // namespace
