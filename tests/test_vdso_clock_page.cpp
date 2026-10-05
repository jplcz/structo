// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/vdso_clock_page.hpp>

using structo::hw::vdso_clock_id;
using structo::hw::vdso_clock_page;
using structo::hw::vdso_clock_slot;
using structo::hw::vdso_clock_source;

namespace {

/** @brief Fixture over a fresh, default-constructed `vdso_clock_page`. */
class VdsoClockPageTest : public ::testing::Test {
protected:
  vdso_clock_page page_;
};

} // namespace

TEST_F(VdsoClockPageTest, DefaultConstructedSlotsAreUnpublished) {
  for (auto id : {vdso_clock_id::realtime, vdso_clock_id::monotonic}) {
    const vdso_clock_slot &slot = page_.slot(id);
    EXPECT_EQ(slot.source, vdso_clock_source::none);
    EXPECT_EQ(slot.generation.load(), 0u);
    EXPECT_EQ(slot.counter_hz, 0u);
    EXPECT_EQ(slot.reference_counter, 0u);
    EXPECT_EQ(slot.reference_secs, 0u);
    EXPECT_EQ(slot.reference_subsec_nanos, 0u);
  }
}

TEST_F(VdsoClockPageTest, RealtimeAndMonotonicSlotsAreIndependentStorage) {
  page_.slot(vdso_clock_id::realtime).source = vdso_clock_source::x86_tsc;
  EXPECT_EQ(page_.slot(vdso_clock_id::realtime).source, vdso_clock_source::x86_tsc);
  EXPECT_EQ(page_.slot(vdso_clock_id::monotonic).source, vdso_clock_source::none);
}

TEST_F(VdsoClockPageTest, ConstAccessorMirrorsMutableAccessor) {
  page_.slot(vdso_clock_id::monotonic).counter_hz = 1'000'000'000ULL;
  const vdso_clock_page &const_page = page_;
  EXPECT_EQ(const_page.slot(vdso_clock_id::monotonic).counter_hz, 1'000'000'000ULL);
}

TEST(VdsoClockPageLayoutTest, SlotAndPageLayoutArePinned) {
  // Mirrors the header's own static_asserts; kept here too so a regression shows up as a normal test failure,
  // not just a compile error someone could miss in an unrelated build.
  EXPECT_EQ(sizeof(vdso_clock_slot), 64u);
  EXPECT_EQ(alignof(vdso_clock_slot), 64u);
  EXPECT_EQ(sizeof(vdso_clock_page), 2u * sizeof(vdso_clock_slot));
  EXPECT_TRUE(std::atomic<std::uint32_t>::is_always_lock_free);
}
