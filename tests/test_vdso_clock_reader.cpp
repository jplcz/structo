// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/vdso_clock_reader.hpp>
#include <structo/hw/vdso_clock_writer.hpp>

using structo::hw::cycles;
using structo::hw::vdso_clock_reader;
using structo::hw::vdso_clock_slot;
using structo::hw::vdso_clock_source;
using structo::hw::vdso_clock_writer;
using reloco::duration;
using reloco::error;

namespace {

// Scripted counter: `read_counter` always returns `current_value`, settable by the test.
struct fake_reader_traits {
  static inline std::uint64_t current_value = 0;
  static std::uint64_t read_counter(vdso_clock_source) noexcept { return current_value; }
};

using reader = vdso_clock_reader<fake_reader_traits>;

/** @brief Fixture over a fresh slot; resets the scripted counter before each test. */
class VdsoClockReaderTest : public ::testing::Test {
protected:
  void SetUp() override { fake_reader_traits::current_value = 0; }
  vdso_clock_slot slot_;
};

} // namespace

TEST_F(VdsoClockReaderTest, UnpublishedSlotFailsWithNotInitialized) {
  auto now = reader::try_now(slot_);
  ASSERT_FALSE(now.has_value());
  EXPECT_EQ(now.error(), error::not_initialized);
}

TEST_F(VdsoClockReaderTest, ReadsBackExactReferenceWhenCounterUnchanged) {
  vdso_clock_writer writer(slot_);
  writer.publish(vdso_clock_source::x86_tsc, 1'000'000'000ULL, cycles{1'000}, duration::from_secs(10));
  fake_reader_traits::current_value = 1'000;

  auto now = reader::try_now(slot_);
  ASSERT_TRUE(now.has_value());
  EXPECT_EQ(now.value().as_secs(), 10u);
}

TEST_F(VdsoClockReaderTest, ComputesElapsedSinceReferenceFromLiveCounter) {
  vdso_clock_writer writer(slot_);
  writer.publish(vdso_clock_source::x86_tsc, 1'000'000'000ULL, cycles{1'000}, duration::from_secs(10));
  fake_reader_traits::current_value = 1'000 + 500'000'000ULL; // +0.5s

  auto now = reader::try_now(slot_);
  ASSERT_TRUE(now.has_value());
  EXPECT_EQ(now.value().as_millis(), 10'500u);
}

TEST_F(VdsoClockReaderTest, CounterBehindReferenceFailsRatherThanUnderflowing) {
  vdso_clock_writer writer(slot_);
  writer.publish(vdso_clock_source::x86_tsc, 1'000'000'000ULL, cycles{1'000}, duration::from_secs(10));
  fake_reader_traits::current_value = 500; // behind the published reference

  auto now = reader::try_now(slot_);
  ASSERT_FALSE(now.has_value());
  EXPECT_EQ(now.error(), error::integer_overflow);
}

TEST_F(VdsoClockReaderTest, ExhaustedRetriesFailWithTryAgainWhenWriterNeverCompletes) {
  vdso_clock_writer writer(slot_);
  // Begin an update and deliberately never let the guard destruct (never returns to an even generation),
  // simulating a writer that never completes -- the reader must give up rather than loop forever.
  auto guard = writer.begin_update();
  guard.set_source(vdso_clock_source::x86_tsc);

  auto now = reader::try_now(slot_);
  ASSERT_FALSE(now.has_value());
  EXPECT_EQ(now.error(), error::try_again);
}
