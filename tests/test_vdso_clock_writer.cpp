// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/vdso_clock_writer.hpp>

using reloco::duration;
using structo::hw::cycles;
using structo::hw::vdso_clock_page;
using structo::hw::vdso_clock_slot;
using structo::hw::vdso_clock_source;
using structo::hw::vdso_clock_update_guard;
using structo::hw::vdso_clock_writer;

namespace {

/** @brief Fixture over a fresh slot and a writer bound to it. */
class VdsoClockWriterTest : public ::testing::Test {
protected:
  vdso_clock_slot slot_;
};

} // namespace

TEST_F(VdsoClockWriterTest, PublishSetsAllFieldsAndReturnsToEvenGeneration) {
  vdso_clock_writer writer(slot_);
  writer.publish(vdso_clock_source::x86_tsc, 1'000'000'000ULL, cycles{42}, duration::from_secs(7));

  EXPECT_EQ(slot_.source, vdso_clock_source::x86_tsc);
  EXPECT_EQ(slot_.counter_hz, 1'000'000'000ULL);
  EXPECT_EQ(slot_.reference_counter, 42u);
  EXPECT_EQ(slot_.reference_secs, 7u);
  EXPECT_EQ(slot_.reference_subsec_nanos, 0u);
  EXPECT_EQ(slot_.generation.load() & 1u, 0u); // even: stable, not mid-update
}

TEST_F(VdsoClockWriterTest, EachPublishAdvancesGenerationByTwo) {
  vdso_clock_writer writer(slot_);
  writer.publish(vdso_clock_source::x86_tsc, 1, cycles{0}, duration{});
  const auto gen_after_first = slot_.generation.load();
  writer.publish(vdso_clock_source::x86_tsc, 1, cycles{1}, duration{});
  EXPECT_EQ(slot_.generation.load(), gen_after_first + 2);
}

TEST_F(VdsoClockWriterTest, UpdateGuardGoesOddDuringTheGuardsLifetimeAndEvenAfter) {
  vdso_clock_writer writer(slot_);
  {
    auto guard = writer.begin_update();
    EXPECT_EQ(slot_.generation.load() & 1u, 1u); // odd: mid-update
    guard.set_source(vdso_clock_source::arm_cntvct_el0);
    guard.set_counter_hz(2'000'000'000ULL);
    guard.set_reference(cycles{99}, duration::from_secs(1));
  }
  EXPECT_EQ(slot_.generation.load() & 1u, 0u);
  EXPECT_EQ(slot_.source, vdso_clock_source::arm_cntvct_el0);
  EXPECT_EQ(slot_.reference_counter, 99u);
}

TEST_F(VdsoClockWriterTest, MovedFromGuardDoesNotDoubleBumpGenerationOnDestruction) {
  vdso_clock_writer writer(slot_);
  {
    auto guard = writer.begin_update();
    const auto gen_odd = slot_.generation.load();
    auto moved = std::move(guard);
    // `guard` is now a no-op; only `moved`'s destructor bumps the generation back to even, exactly once.
    (void)gen_odd;
  }
  EXPECT_EQ(slot_.generation.load() & 1u, 0u);
  EXPECT_EQ(slot_.generation.load(), 2u); // one odd bump (begin_update) + one even bump (moved's destructor)
}

TEST_F(VdsoClockWriterTest, SetReferenceSplitsSecondsAndSubsecNanosCorrectly) {
  vdso_clock_writer writer(slot_);
  auto guard = writer.begin_update();
  guard.set_reference(cycles{0}, duration::from_secs(3) + duration::from_nanos(123'456'789));
  EXPECT_EQ(slot_.reference_secs, 3u);
  EXPECT_EQ(slot_.reference_subsec_nanos, 123'456'789u);
}
