// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/page_decay.hpp>

namespace {

using namespace structo;
using decay = page_decay<4>;

TEST(PageDecay, IdlePageDemotedAfterBitsScans) {
  auto age = decay::fresh();
  for (int i = 0; i < 3; ++i) {
    auto r = decay::step(age, false);
    EXPECT_EQ(r.action, decay_action::keep_active) << i;
    age = r.age;
  }
  EXPECT_EQ(decay::step(age, false).action, decay_action::demote);
}

TEST(PageDecay, AccessResetsSurvival) {
  auto age = decay::fresh();
  for (int i = 0; i < 3; ++i) {
    age = decay::step(age, false).age;
  }
  EXPECT_EQ(decay::idle_scans_left(age), 1u);
  auto r = decay::step(age, true); // accessed just in time
  EXPECT_EQ(r.action, decay_action::keep_active);
  EXPECT_EQ(decay::idle_scans_left(r.age), 4u);
}

TEST(PageDecay, TouchAndEdge) {
  EXPECT_EQ(decay::touch(0), decay::top_bit);
  EXPECT_EQ(decay::idle_scans_left(0), 0u);
  EXPECT_EQ(decay::step(0, false).action, decay_action::demote);
  EXPECT_EQ(page_decay<1>::step(page_decay<1>::fresh(), false).action, decay_action::demote);
  EXPECT_EQ(page_decay<12>::max_age, 0xFFFu);
}

TEST(PageDecay, AgeFieldPacksIntoFlags) {
  using field = page_age_field<8, 4>;
  std::uint32_t flags = 0xFFFF'FFFFu;
  flags = field::set(flags, 0x5);
  EXPECT_EQ(field::get(flags), 0x5u);
  EXPECT_EQ(flags, 0xFFFF'F5FFu); // neighbours untouched
}

TEST(PagePressure, Urgency) {
  EXPECT_EQ(decay_urgency(1000, 100, 200), 0u);
  EXPECT_EQ(decay_urgency(100, 100, 200), 256u);
  EXPECT_EQ(decay_urgency(150, 100, 200), 128u);
}

TEST(PagePressure, ScanCount) {
  decay_pressure_config cfg{};
  // Plenty of free memory and balanced lists: nothing to do.
  EXPECT_EQ(decay_scan_count(cfg, 1000, 1000, 5000, 100, 200), 0u);
  // Plenty of free memory but INACTIVE too small: slow background aging (16/256).
  EXPECT_EQ(decay_scan_count(cfg, 4096, 100, 5000, 100, 200), 256u);
  // Critical: scan everything.
  EXPECT_EQ(decay_scan_count(cfg, 4096, 100, 50, 100, 200), 4096u);
  // Tiny ACTIVE list is clamped to its size; empty means 0.
  EXPECT_EQ(decay_scan_count(cfg, 10, 0, 50, 100, 200), 10u);
  EXPECT_EQ(decay_scan_count(cfg, 0, 0, 50, 100, 200), 0u);
  // Huge counts do not overflow.
  EXPECT_EQ(decay_scan_count(cfg, ~0ull >> 8, 0, 0, 100, 200), ~0ull >> 8);
  EXPECT_TRUE(decay_inactive_is_low(cfg, 100, 10));
  EXPECT_FALSE(decay_inactive_is_low(cfg, 100, 100));
}

} // namespace
