// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/per_thread_ptr.hpp>

#include <cstddef>
#include <type_traits>

using structo::arch::per_thread_ptr;

namespace {

// Tag without a current-thread fast path: per_thread_ptr must fall back to
// combining Tag::current() with get_ptr()/set_ptr(), modeling an intrusive
// field inside a caller-defined task control block.
struct tcb_backed_tag {
  struct tcb {
    void *slot = nullptr;
  };

  using thread_id_type = tcb *;

  static inline tcb *current_tcb = nullptr;

  static tcb *current() noexcept { return current_tcb; }
  static void *get_ptr(tcb *tid) noexcept { return tid->slot; }
  static void set_ptr(tcb *tid, void *ptr) noexcept { tid->slot = ptr; }
};

// Tag exposing a dedicated current-thread fast path (modeling __thread/TLS
// storage), tracked separately so tests can tell which path was used.
struct fast_path_tag {
  static inline void *current_slot = nullptr;
  static inline int get_current_calls = 0;
  static inline int set_current_calls = 0;

  static void reset() noexcept {
    current_slot = nullptr;
    get_current_calls = 0;
    set_current_calls = 0;
  }

  static void *get_current_ptr() noexcept {
    ++get_current_calls;
    return current_slot;
  }
  static void set_current_ptr(void *ptr) noexcept {
    ++set_current_calls;
    current_slot = ptr;
  }
};

/** @brief Fixture for `per_thread_ptr<tcb_backed_tag, T>` tests; gives each test its own fresh `tcb` and resets the Tag's "current thread" before each test. */
class PerThreadPtrTcbBackedTest : public ::testing::Test {
protected:
  void SetUp() override {
    tcb_backed_tag::current_tcb = &m_tcb;
    m_tcb.slot = nullptr;
  }

  tcb_backed_tag::tcb m_tcb{};
};

/** @brief Fixture for `per_thread_ptr<fast_path_tag, T>` tests; resets the Tag's mutable static state before each test. */
class PerThreadPtrFastPathTest : public ::testing::Test {
protected:
  void SetUp() override { fast_path_tag::reset(); }
};

} // namespace

TEST_F(PerThreadPtrTcbBackedTest, GetReturnsNullptrBeforeAnythingIsSet) {
  using ptr = per_thread_ptr<tcb_backed_tag, int>;
  EXPECT_EQ(ptr::get(), nullptr);
}

TEST_F(PerThreadPtrTcbBackedTest, SetThenGetRoundTripsThroughCurrentPlusGetPtrSetPtr) {
  using ptr = per_thread_ptr<tcb_backed_tag, int>;
  int value = 42;

  ptr::set(&value);

  EXPECT_EQ(m_tcb.slot, &value);
  EXPECT_EQ(ptr::get(), &value);
}

TEST_F(PerThreadPtrTcbBackedTest, SwitchingCurrentThreadResolvesADifferentSlot) {
  using ptr = per_thread_ptr<tcb_backed_tag, int>;
  int value = 7;
  ptr::set(&value);
  ASSERT_EQ(ptr::get(), &value);

  tcb_backed_tag::tcb other_tcb;
  tcb_backed_tag::current_tcb = &other_tcb;

  // A different "current thread" has its own, still-empty slot.
  EXPECT_EQ(ptr::get(), nullptr);
}

TEST_F(PerThreadPtrTcbBackedTest, SettingNullptrClearsTheSlot) {
  using ptr = per_thread_ptr<tcb_backed_tag, int>;
  int value = 9;

  ptr::set(&value);
  ASSERT_EQ(ptr::get(), &value);

  ptr::set(nullptr);
  EXPECT_EQ(ptr::get(), nullptr);
}

TEST_F(PerThreadPtrTcbBackedTest, CustomThreadIdTypeFromTagIsHonored) {
  using ptr = per_thread_ptr<tcb_backed_tag, int>;
  static_assert(std::is_same_v<ptr::thread_id_type, tcb_backed_tag::tcb *>,
                "thread_id_type must come from Tag::thread_id_type when provided");
}

TEST_F(PerThreadPtrFastPathTest, GetSetPreferTagsFastPathOverCurrentPlusGetPtr) {
  using ptr = per_thread_ptr<fast_path_tag, int>;
  int value = 11;

  ptr::set(&value);

  EXPECT_EQ(fast_path_tag::set_current_calls, 1);
  EXPECT_EQ(fast_path_tag::current_slot, &value);

  EXPECT_EQ(ptr::get(), &value);
  EXPECT_EQ(fast_path_tag::get_current_calls, 1);
}

TEST_F(PerThreadPtrFastPathTest, DefaultThreadIdTypeIsSizeTWhenTagDoesNotProvideOne) {
  using ptr = per_thread_ptr<fast_path_tag, int>;
  static_assert(std::is_same_v<ptr::thread_id_type, std::size_t>,
                "thread_id_type must default to std::size_t when Tag has no thread_id_type");
}
