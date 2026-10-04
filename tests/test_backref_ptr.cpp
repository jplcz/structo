// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/backref_ptr.hpp>

namespace {

struct owner {
  int id = 0;
};

template <typename Cell> class BackrefPtrTest : public ::testing::Test {};

using CellTypes = ::testing::Types<structo::embedded_mutex_cell<owner>, structo::embedded_rw_cell<owner>,
                                   structo::embedded_seqlock_cell<owner>, structo::striped_mutex_cell<owner, 4>,
                                   structo::striped_rw_cell<owner, 4>, structo::striped_seqlock_cell<owner, 4>>;

TYPED_TEST_SUITE(BackrefPtrTest, CellTypes, ::testing::internal::DefaultNameGenerator);

TYPED_TEST(BackrefPtrTest, DefaultConstructedPointsAtNothing) {
  structo::backref_ptr<owner, TypeParam> ref;
  auto g = ref.lock();
  EXPECT_EQ(g.get(), nullptr);
}

TYPED_TEST(BackrefPtrTest, ResetReassignsThePointer) {
  structo::backref_ptr<owner, TypeParam> ref;
  owner o{42};
  {
    auto g = ref.lock();
    g.reset(&o);
  }
  auto g2 = ref.lock();
  ASSERT_EQ(g2.get(), &o);
  EXPECT_EQ(g2->id, 42);
  EXPECT_EQ((*g2).id, 42);
}

TYPED_TEST(BackrefPtrTest, TryLockSucceedsWhenUncontended) {
  structo::backref_ptr<owner, TypeParam> ref;
  auto result = ref.try_lock();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->get(), nullptr);
}

TYPED_TEST(BackrefPtrTest, ResetToNullptrDetaches) {
  structo::backref_ptr<owner, TypeParam> ref;
  owner o{7};
  {
    auto g = ref.lock();
    g.reset(&o);
  }
  {
    auto g = ref.lock();
    g.reset(nullptr);
  }
  auto g = ref.lock();
  EXPECT_EQ(g.get(), nullptr);
}

// ----------------------------------------------------------------------
// embedded_rw_cell / striped_rw_cell: shared-reader access
// ----------------------------------------------------------------------

TEST(BackrefPtrRwTest, SharedLockObservesTheCurrentPointer) {
  structo::backref_ptr<owner, structo::embedded_rw_cell<owner>> ref;
  owner o{5};
  ref.lock().reset(&o);

  auto reader = ref.shared_lock();
  EXPECT_EQ(reader.get(), &o);
  EXPECT_EQ(reader->id, 5);
}

TEST(BackrefPtrRwTest, TrySharedLockSucceedsWhenUncontended) {
  structo::backref_ptr<owner, structo::embedded_rw_cell<owner>> ref;
  auto reader = ref.try_shared_lock();
  ASSERT_TRUE(reader.has_value());
  EXPECT_EQ(reader->get(), nullptr);
}

TEST(BackrefPtrStripedRwTest, SharedLockObservesTheCurrentPointer) {
  structo::backref_ptr<owner, structo::striped_rw_cell<owner, 4>> ref;
  owner o{9};
  ref.lock().reset(&o);

  auto reader = ref.shared_lock();
  EXPECT_EQ(reader.get(), &o);
}

// ----------------------------------------------------------------------
// embedded_seqlock_cell / striped_seqlock_cell: lock-free optimistic reads
// ----------------------------------------------------------------------

TEST(BackrefPtrSeqlockTest, ReadUnlockedObservesTheCurrentPointer) {
  structo::backref_ptr<owner, structo::embedded_seqlock_cell<owner>> ref;
  owner o{3};
  ref.lock().reset(&o);

  EXPECT_EQ(ref.read_unlocked(), &o);
}

TEST(BackrefPtrSeqlockTest, ReadUnlockedObservesNullptrInitially) {
  structo::backref_ptr<owner, structo::embedded_seqlock_cell<owner>> ref;
  EXPECT_EQ(ref.read_unlocked(), nullptr);
}

TEST(BackrefPtrStripedSeqlockTest, ReadUnlockedObservesTheCurrentPointer) {
  structo::backref_ptr<owner, structo::striped_seqlock_cell<owner, 4>> ref;
  owner o{11};
  ref.lock().reset(&o);

  EXPECT_EQ(ref.read_unlocked(), &o);
}

} // namespace
