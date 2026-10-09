// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/clock_ref.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace structo::hw;
using reloco::error;

namespace {

// 16-bit counter at 1 kHz whose value the test sets directly.
struct fake_clock {
  std::uint64_t value = 0;
};

struct full_clock {
  std::uint64_t value = 0;
};

} // namespace

template <> struct structo::hw::clock_traits<fake_clock> {
  static reloco::result<std::uint64_t> read_counter(fake_clock &b) noexcept { return b.value; }
  static std::uint64_t frequency_hz(fake_clock &) noexcept { return 1000; }
  static unsigned counter_bits(fake_clock &) noexcept { return 16; }
};

template <> struct structo::hw::clock_traits<full_clock> {
  static reloco::result<std::uint64_t> read_counter(full_clock &b) noexcept { return b.value; }
  static std::uint64_t frequency_hz(full_clock &) noexcept { return 1'000'000; }
};

TEST(ClockRefTest, UnboundFails) {
  clock_ref c;
  EXPECT_FALSE(c);
  EXPECT_EQ(c.read().error(), error::unsupported_operation);
  EXPECT_EQ(c.frequency_hz().error(), error::unsupported_operation);
  EXPECT_EQ(c.counter_bits().error(), error::unsupported_operation);
}

TEST(ClockRefTest, ReadMasksToWidthAndDefaultsTo64) {
  fake_clock f{0x12345};
  clock_ref c{f};
  EXPECT_EQ(c.read().value().raw(), 0x2345u);
  EXPECT_EQ(c.counter_bits().value(), 16u);
  EXPECT_EQ(c.max_value().value().raw(), 0xFFFFu);
  EXPECT_EQ(c.frequency_hz().value(), 1000u);

  full_clock g{~std::uint64_t{0}};
  clock_ref d{g};
  EXPECT_EQ(d.counter_bits().value(), 64u);
  EXPECT_EQ(d.read().value().raw(), ~std::uint64_t{0});
}

TEST(ClockRefTest, DeltaHandlesWrap) {
  fake_clock f;
  clock_ref c{f};
  EXPECT_EQ(c.delta(cycles{0xFFF0}, cycles{0x0010}).value().raw(), 0x20u);
  EXPECT_EQ(c.delta(cycles{5}, cycles{9}).value().raw(), 4u);
}

TEST(ClockRefTest, ToDuration) {
  fake_clock f;
  clock_ref c{f};
  EXPECT_EQ(c.to_duration(cycles{1500}).value(), reloco::duration::from_millis(1500));
}

TEST(ClockReaderTest, FirstStepPrimes) {
  fake_clock f{100};
  clock_reader r{clock_ref{f}};
  auto s = r.step().value();
  EXPECT_FALSE(s.glitch);
  EXPECT_EQ(s.elapsed.raw(), 0u);
  EXPECT_TRUE(r.primed());
}

TEST(ClockReaderTest, ForwardAndWrap) {
  fake_clock f{0xFFF0};
  clock_reader r{clock_ref{f}};
  ASSERT_TRUE(r.reset());
  f.value = 0x0010; // wrapped
  auto s = r.step().value();
  EXPECT_FALSE(s.glitch);
  EXPECT_EQ(s.elapsed.raw(), 0x20u);
}

TEST(ClockReaderTest, BackstepIsGlitchAndRebases) {
  fake_clock f{1000};
  clock_reader r{clock_ref{f}};
  ASSERT_TRUE(r.reset());
  f.value = 900;
  auto s = r.step().value();
  EXPECT_TRUE(s.glitch);
  EXPECT_EQ(s.elapsed.raw(), 0u);
  f.value = 950; // relative to the new value
  s = r.step().value();
  EXPECT_FALSE(s.glitch);
  EXPECT_EQ(s.elapsed.raw(), 50u);
}

TEST(ClockReaderTest, OversizedStepIsGlitch) {
  fake_clock f{0};
  clock_reader r{clock_ref{f}, cycles{100}};
  ASSERT_TRUE(r.reset());
  f.value = 100;
  EXPECT_FALSE(r.step().value().glitch);
  f.value = 301;
  EXPECT_TRUE(r.step().value().glitch);
  f.value = 310;
  auto s = r.step().value();
  EXPECT_FALSE(s.glitch);
  EXPECT_EQ(s.elapsed.raw(), 9u);
}

TEST(ClockReaderTest, StepDuration) {
  fake_clock f{0};
  clock_reader r{clock_ref{f}};
  ASSERT_TRUE(r.reset());
  f.value = 250;
  bool glitch = true;
  EXPECT_EQ(r.step_duration(&glitch).value(), reloco::duration::from_millis(250));
  EXPECT_FALSE(glitch);
}

TEST(ClockReaderTest, UnboundReaderFails) {
  clock_reader r;
  EXPECT_EQ(r.step().error(), error::unsupported_operation);
}

TEST(ClockReaderTest, InstantAccumulatesAcrossWrapsAndSkipsGlitches) {
  fake_clock f{0xFFF0};
  clock_reader r{clock_ref{f}, reloco::duration::from_millis(100)};
  ASSERT_TRUE(r.reset());
  f.value = 0x0010; // wrapped, 32 ms
  auto t1 = r.now().value();
  EXPECT_EQ(t1 - reloco::instant{}, reloco::duration::from_millis(32));
  f.value = 0x0000; // backstep: glitch, instant unchanged
  EXPECT_EQ(r.now().value(), t1);
  f.value = 0x0040; // 64 ms after the rebase
  auto t2 = r.now().value();
  EXPECT_EQ(t2 - t1, reloco::duration::from_millis(64));
  EXPECT_EQ(r.total().raw(), 96u);
  f.value = 0x0040 + 500; // above the 100 ms limit: glitch
  EXPECT_EQ(r.now().value(), t2);
}

namespace {
// 16-bit, 1 kHz counter backed by an atomic, advanced by the test.
struct shared_clock {
  std::atomic<std::uint64_t> value{0};
};
} // namespace

template <> struct structo::hw::clock_traits<shared_clock> {
  static reloco::result<std::uint64_t> read_counter(shared_clock &b) noexcept { return b.value.load(); }
  static std::uint64_t frequency_hz(shared_clock &) noexcept { return 1000; }
  static unsigned counter_bits(shared_clock &) noexcept { return 16; }
};

TEST(AtomicClockReaderTest, NotInitializedBeforeReset) {
  fake_clock f;
  atomic_clock_reader r{clock_ref{f}};
  EXPECT_FALSE(r.primed());
  EXPECT_EQ(r.step().error(), error::not_initialized);
}

TEST(AtomicClockReaderTest, WrapGlitchAndInstant) {
  fake_clock f{0xFFF0};
  atomic_clock_reader r{clock_ref{f}, reloco::duration::from_millis(100)};
  ASSERT_TRUE(r.reset());
  f.value = 0x0010;
  auto t1 = r.now().value();
  EXPECT_EQ(t1 - reloco::instant{}, reloco::duration::from_millis(32));
  f.value = 0x0000; // backstep
  EXPECT_EQ(r.now().value(), t1);
  EXPECT_EQ(r.glitch_count(), 1u);
  f.value = 0x0040;
  EXPECT_EQ(r.now().value() - t1, reloco::duration::from_millis(64));
}

TEST(AtomicClockReaderTest, ConcurrentStepsCountEveryCycleOnce) {
  shared_clock c;
  atomic_clock_reader r{clock_ref{c}};
  ASSERT_TRUE(r.reset());
  constexpr int ticks = 20000;
  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> sum{0};
  std::vector<std::thread> th;
  for (int i = 0; i < 4; ++i)
    th.emplace_back([&] {
      while (!done.load())
        sum += r.step().value().elapsed.raw();
    });
  for (int i = 0; i < ticks; ++i) {
    c.value.fetch_add(1);
    if (i % 64 == 0)
      std::this_thread::yield();
  }
  done = true;
  for (auto &t : th)
    t.join();
  sum += r.step().value().elapsed.raw();
  EXPECT_EQ(r.glitch_count(), 0u);
  EXPECT_EQ(sum.load(), static_cast<std::uint64_t>(ticks));
  EXPECT_EQ(r.total().raw(), static_cast<std::uint64_t>(ticks));
}

TEST(AtomicClockReaderTest, InvalidateUnprimes) {
  fake_clock f{0};
  atomic_clock_reader r{clock_ref{f}};
  ASSERT_TRUE(r.reset()); // raw 0 is a valid primed sample
  EXPECT_TRUE(r.primed());
  r.invalidate();
  EXPECT_FALSE(r.primed());
  EXPECT_EQ(r.step().error(), error::not_initialized);
}
