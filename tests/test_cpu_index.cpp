// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/cpu_index.hpp>

#include <cstddef>

using structo::arch::cpu_index;
using structo::arch::uniprocessor_tag;

namespace {

// Minimal multi-core Tag exercising every optional SFINAE-detected method
// (current(), from_context(), hardware_id(), is_bsp(), yield(),
// wait_for_event(), send_event()) so cpu_index<multicore_tag> enables its
// full passthrough surface, not just the subset uniprocessor_tag provides.
struct fault_context {
  std::size_t cpu = 0;
};

struct multicore_tag {
  static inline constexpr std::size_t max_cpus = 4;
  static inline std::size_t current_cpu = 0;
  static inline int yield_count = 0;
  static inline int wait_count = 0;
  static inline int send_count = 0;

  static std::size_t current() noexcept { return current_cpu; }
  static std::size_t from_context(const fault_context &ctx) noexcept { return ctx.cpu; }
  static constexpr uint32_t hardware_id() noexcept { return 0xC0FFEEu; }
  static bool is_bsp() noexcept { return current_cpu == 0; }
  static void yield() noexcept { ++yield_count; }
  static void wait_for_event() noexcept { ++wait_count; }
  static void send_event() noexcept { ++send_count; }
};

} // namespace

TEST(CpuIndexTest, UniprocessorTagReportsSingleAlwaysValidCore) {
  using idx = cpu_index<uniprocessor_tag>;

  EXPECT_EQ(idx::max_cpus, 1u);
  EXPECT_EQ(idx::current(), 0u);
  EXPECT_TRUE(idx::is_valid(0));
  EXPECT_FALSE(idx::is_valid(1));
  EXPECT_TRUE(idx::is_bsp());
  EXPECT_EQ(idx::hardware_id(), 0u);

  // These passthroughs must simply not crash: uniprocessor_tag's bodies are
  // empty no-ops.
  idx::yield();
  idx::wait_for_event();
  idx::send_event();
}

TEST(CpuIndexTest, UniprocessorTagFromContextAlwaysResolvesToCoreZero) {
  using idx = cpu_index<uniprocessor_tag>;
  struct dummy_context {};
  EXPECT_EQ(idx::from_context(dummy_context{}), 0u);
}

TEST(CpuIndexTest, MulticoreTagDelegatesCurrentAndClampsOutOfRangeResults) {
  using idx = cpu_index<multicore_tag>;

  multicore_tag::current_cpu = 2;
  EXPECT_EQ(idx::current(), 2u);
  EXPECT_TRUE(idx::is_bsp() == false);

  // current() clamps any Tag-reported index that is out of bounds back to 0
  // rather than propagating an unchecked, possibly-out-of-bounds value.
  multicore_tag::current_cpu = 99;
  EXPECT_EQ(idx::current(), 0u);
  multicore_tag::current_cpu = 0;
}

TEST(CpuIndexTest, MulticoreTagResolvesFromExplicitFaultContext) {
  using idx = cpu_index<multicore_tag>;

  EXPECT_EQ(idx::from_context(fault_context{3}), 3u);
  // Out-of-range context-reported index is clamped to 0, same as current().
  EXPECT_EQ(idx::from_context(fault_context{42}), 0u);
}

TEST(CpuIndexTest, MulticoreTagExposesHardwareIdAndKernelOperationPassthroughs) {
  using idx = cpu_index<multicore_tag>;

  EXPECT_EQ(idx::hardware_id(), 0xC0FFEEu);

  const int yields_before = multicore_tag::yield_count;
  const int waits_before = multicore_tag::wait_count;
  const int sends_before = multicore_tag::send_count;

  idx::yield();
  idx::wait_for_event();
  idx::send_event();

  EXPECT_EQ(multicore_tag::yield_count, yields_before + 1);
  EXPECT_EQ(multicore_tag::wait_count, waits_before + 1);
  EXPECT_EQ(multicore_tag::send_count, sends_before + 1);
}

TEST(CpuIndexTest, IsValidRespectsTagMaxCpusBoundary) {
  using idx = cpu_index<multicore_tag>;

  EXPECT_TRUE(idx::is_valid(0));
  EXPECT_TRUE(idx::is_valid(3));
  EXPECT_FALSE(idx::is_valid(4));
  EXPECT_FALSE(idx::is_valid(static_cast<std::size_t>(-1)));
}
