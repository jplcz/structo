// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/per_cpu_ptr.hpp>

#include <cstddef>

#include <reloco/lifetime.hpp>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

using structo::arch::per_cpu_ptr;

namespace {

// Tag without a current-CPU fast path: per_cpu_ptr must fall back to
// combining Tag::current() with get_ptr()/set_ptr().
struct array_backed_tag {
  static inline constexpr std::size_t max_cpus = 4;
  static inline void *slots[max_cpus]{nullptr};
  static inline std::size_t current_cpu = 0;

  static void reset() noexcept {
    for (auto &slot : slots) {
      slot = nullptr;
    }
    current_cpu = 0;
  }

  static void *get_ptr(std::size_t cpu) noexcept { return slots[cpu]; }
  static void set_ptr(std::size_t cpu, void *ptr) noexcept { slots[cpu] = ptr; }

  static std::size_t current() noexcept { return current_cpu; }
};

// Tag exposing a dedicated current-CPU fast path (tracked separately from
// the regular per-cpu slots so tests can tell which path was actually
// used), plus a custom cpu_id_type to exercise that resolution path too.
struct fast_path_tag {
  using cpu_id_type = int;

  static inline constexpr std::size_t max_cpus = 4;
  static inline void *slots[max_cpus]{nullptr};
  static inline void *current_slot = nullptr;
  static inline int get_current_calls = 0;
  static inline int set_current_calls = 0;

  static void reset() noexcept {
    for (auto &slot : slots) {
      slot = nullptr;
    }
    current_slot = nullptr;
    get_current_calls = 0;
    set_current_calls = 0;
  }

  static void *get_ptr(int cpu) noexcept { return slots[static_cast<std::size_t>(cpu)]; }
  static void set_ptr(int cpu, void *ptr) noexcept { slots[static_cast<std::size_t>(cpu)] = ptr; }

  static void *get_current_ptr() noexcept {
    ++get_current_calls;
    return current_slot;
  }
  static void set_current_ptr(void *ptr) noexcept {
    ++set_current_calls;
    current_slot = ptr;
  }
};

/** @brief Fixture for `per_cpu_ptr<array_backed_tag, T>` tests; resets the Tag's mutable static state before each test.
 */
class PerCpuPtrArrayBackedTest : public ::testing::Test {
protected:
  void SetUp() override { array_backed_tag::reset(); }
};

/** @brief Fixture for `per_cpu_ptr<fast_path_tag, T>` tests; resets the Tag's mutable static state before each test. */
class PerCpuPtrFastPathTest : public ::testing::Test {
protected:
  void SetUp() override { fast_path_tag::reset(); }
};

} // namespace

TEST_F(PerCpuPtrArrayBackedTest, ExplicitCpuGetSetRoundTripsThroughTagStorage) {
  using ptr = per_cpu_ptr<array_backed_tag, int>;
  int value = 42;

  EXPECT_EQ(ptr::get(2), nullptr);
  ptr::set(2, &value);
  EXPECT_EQ(ptr::get(2), &value);
  EXPECT_EQ(array_backed_tag::slots[2], &value);
}

TEST_F(PerCpuPtrArrayBackedTest, ExplicitCpuSlotsAreIndependent) {
  using ptr = per_cpu_ptr<array_backed_tag, int>;
  int a = 1;
  int b = 2;

  ptr::set(0, &a);
  ptr::set(1, &b);

  EXPECT_EQ(ptr::get(0), &a);
  EXPECT_EQ(ptr::get(1), &b);
}

TEST_F(PerCpuPtrArrayBackedTest, CurrentCpuAccessorsFallBackToTagCurrentWhenNoFastPathExists) {
  using ptr = per_cpu_ptr<array_backed_tag, int>;
  int value = 7;

  array_backed_tag::current_cpu = 3;
  ptr::set(&value);

  EXPECT_EQ(array_backed_tag::slots[3], &value);
  EXPECT_EQ(ptr::get(), &value);

  array_backed_tag::current_cpu = 0;
  EXPECT_EQ(ptr::get(), nullptr); // a different "current" CPU has its own, still-empty slot
}

TEST_F(PerCpuPtrArrayBackedTest, SettingNullptrClearsASlot) {
  using ptr = per_cpu_ptr<array_backed_tag, int>;
  int value = 9;

  ptr::set(1, &value);
  ASSERT_EQ(ptr::get(1), &value);

  ptr::set(1, nullptr);
  EXPECT_EQ(ptr::get(1), nullptr);
}

TEST_F(PerCpuPtrFastPathTest, CurrentCpuAccessorsPreferTagsFastPathOverCurrentPlusGetPtr) {
  using ptr = per_cpu_ptr<fast_path_tag, int>;
  int value = 11;

  ptr::set(&value);

  EXPECT_EQ(fast_path_tag::set_current_calls, 1);
  EXPECT_EQ(fast_path_tag::current_slot, &value);
  // The regular per-cpu slots must be untouched: the fast path bypasses them entirely.
  for (const auto &slot : fast_path_tag::slots) {
    EXPECT_EQ(slot, nullptr);
  }

  EXPECT_EQ(ptr::get(), &value);
  EXPECT_EQ(fast_path_tag::get_current_calls, 1);
}

TEST_F(PerCpuPtrFastPathTest, ExplicitCpuAccessorsStillUseGetPtrSetPtrRegardlessOfFastPath) {
  using ptr = per_cpu_ptr<fast_path_tag, int>;
  int value = 13;

  ptr::set(2, &value);

  EXPECT_EQ(fast_path_tag::set_current_calls, 0);
  EXPECT_EQ(fast_path_tag::slots[2], &value);
  EXPECT_EQ(ptr::get(2), &value);
  EXPECT_EQ(fast_path_tag::get_current_calls, 0);
}

TEST_F(PerCpuPtrFastPathTest, CustomCpuIdTypeFromTagIsHonored) {
  using ptr = per_cpu_ptr<fast_path_tag, int>;
  static_assert(std::is_same_v<ptr::cpu_id_type, int>, "cpu_id_type must come from Tag::cpu_id_type when provided");
}

RELOCO_END_UNSAFE_BUFFER_USAGE