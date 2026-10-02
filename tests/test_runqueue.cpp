// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/runqueue.hpp>

namespace {

using namespace structo;

struct task {
  struct {
    task *next = nullptr;
    task **prev = nullptr;
  } link;
  unsigned priority = 0;
  int id = 0;
};

// --------------------------------------------------------------------
// fifo_runqueue
// --------------------------------------------------------------------

using fifo_queue = fifo_runqueue<task, &task::link>;

TEST(FifoRunqueueTest, StartsEmpty) {
  fifo_queue q;
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
  EXPECT_EQ(q.peek(), nullptr);
  EXPECT_EQ(q.dequeue(), nullptr);
}

TEST(FifoRunqueueTest, DequeuesInEnqueueOrderIgnoringPriority) {
  fifo_queue q;
  task a{{}, /*priority=*/5, /*id=*/1};
  task b{{}, /*priority=*/0, /*id=*/2};
  task c{{}, /*priority=*/9, /*id=*/3};

  q.enqueue(a);
  q.enqueue(b);
  q.enqueue(c);
  EXPECT_FALSE(q.empty());
  EXPECT_EQ(q.size(), 3u);

  EXPECT_EQ(q.peek(), &a);
  EXPECT_EQ(q.dequeue(), &a);
  EXPECT_EQ(q.dequeue(), &b);
  EXPECT_EQ(q.size(), 1u);
  EXPECT_EQ(q.dequeue(), &c);
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.dequeue(), nullptr);
}

TEST(FifoRunqueueTest, RemoveDetachesArbitraryEntry) {
  fifo_queue q;
  task a{{}, 0, 1};
  task b{{}, 0, 2};
  task c{{}, 0, 3};
  q.enqueue(a);
  q.enqueue(b);
  q.enqueue(c);

  q.remove(b);
  EXPECT_EQ(q.size(), 2u);
  EXPECT_EQ(q.dequeue(), &a);
  EXPECT_EQ(q.dequeue(), &c);
  EXPECT_TRUE(q.empty());
}

TEST(FifoRunqueueTest, IsLinkedTracksEnqueueLifecycle) {
  fifo_queue q;
  task a{{}, 0, 1};
  EXPECT_FALSE(fifo_queue::is_linked(a));

  q.enqueue(a);
  EXPECT_TRUE(fifo_queue::is_linked(a));

  q.remove(a);
  EXPECT_FALSE(fifo_queue::is_linked(a));

  q.enqueue(a);
  EXPECT_TRUE(fifo_queue::is_linked(a));
  EXPECT_EQ(q.dequeue(), &a);
  EXPECT_FALSE(fifo_queue::is_linked(a));
}

// --------------------------------------------------------------------
// priority_list_runqueue
// --------------------------------------------------------------------

using priority_list_queue = priority_list_runqueue<task, &task::link, &task::priority>;

TEST(PriorityListRunqueueTest, StartsEmpty) {
  priority_list_queue q;
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
  EXPECT_EQ(q.peek(), nullptr);
  EXPECT_EQ(q.dequeue(), nullptr);
}

TEST(PriorityListRunqueueTest, DequeuesLowestPriorityValueFirst) {
  priority_list_queue q;
  task low{{}, /*priority=*/1, /*id=*/1};
  task mid{{}, /*priority=*/5, /*id=*/2};
  task high{{}, /*priority=*/0, /*id=*/3};

  // Enqueued out of order; must come out sorted ascending by priority.
  q.enqueue(mid);
  q.enqueue(low);
  q.enqueue(high);
  EXPECT_EQ(q.size(), 3u);

  EXPECT_EQ(q.peek(), &high);
  EXPECT_EQ(q.dequeue(), &high);
  EXPECT_EQ(q.dequeue(), &low);
  EXPECT_EQ(q.dequeue(), &mid);
  EXPECT_TRUE(q.empty());
}

TEST(PriorityListRunqueueTest, TiesResolveFifo) {
  priority_list_queue q;
  task a{{}, 3, 1};
  task b{{}, 3, 2};
  task c{{}, 3, 3};
  q.enqueue(a);
  q.enqueue(b);
  q.enqueue(c);

  EXPECT_EQ(q.dequeue(), &a);
  EXPECT_EQ(q.dequeue(), &b);
  EXPECT_EQ(q.dequeue(), &c);
}

TEST(PriorityListRunqueueTest, RemoveDetachesArbitraryEntry) {
  priority_list_queue q;
  task low{{}, 1, 1};
  task mid{{}, 5, 2};
  task high{{}, 0, 3};
  q.enqueue(mid);
  q.enqueue(low);
  q.enqueue(high);

  q.remove(low);
  EXPECT_EQ(q.size(), 2u);
  EXPECT_EQ(q.dequeue(), &high);
  EXPECT_EQ(q.dequeue(), &mid);
  EXPECT_TRUE(q.empty());
}

TEST(PriorityListRunqueueTest, IsLinkedTracksEnqueueLifecycle) {
  priority_list_queue q;
  task a{{}, 2, 1};
  EXPECT_FALSE(priority_list_queue::is_linked(a));

  q.enqueue(a);
  EXPECT_TRUE(priority_list_queue::is_linked(a));

  q.remove(a);
  EXPECT_FALSE(priority_list_queue::is_linked(a));
}

// --------------------------------------------------------------------
// priority_bucket_runqueue
// --------------------------------------------------------------------

// `NumPriorities = 140` spans more than one 64-bit bitmap word,
// exercising the multi-word `find_first_set` scan (matching Linux's
// classic 140-level O(1) scheduler priority range).
using bucket_queue = priority_bucket_runqueue<task, &task::link, &task::priority, 140>;

TEST(PriorityBucketRunqueueTest, StartsEmpty) {
  bucket_queue q;
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.size(), 0u);
  EXPECT_EQ(q.peek(), nullptr);
  EXPECT_EQ(q.dequeue(), nullptr);
}

TEST(PriorityBucketRunqueueTest, DequeuesLowestPriorityBucketFirst) {
  bucket_queue q;
  task low{{}, /*priority=*/10, /*id=*/1};
  task mid{{}, /*priority=*/70, /*id=*/2};
  task high{{}, /*priority=*/0, /*id=*/3};

  q.enqueue(mid);
  q.enqueue(low);
  q.enqueue(high);
  EXPECT_EQ(q.size(), 3u);

  EXPECT_EQ(q.peek(), &high);
  EXPECT_EQ(q.dequeue(), &high);
  EXPECT_EQ(q.dequeue(), &low);
  EXPECT_EQ(q.dequeue(), &mid);
  EXPECT_TRUE(q.empty());
}

TEST(PriorityBucketRunqueueTest, SamePriorityBucketIsFifo) {
  bucket_queue q;
  task a{{}, 42, 1};
  task b{{}, 42, 2};
  task c{{}, 42, 3};
  q.enqueue(a);
  q.enqueue(b);
  q.enqueue(c);

  EXPECT_EQ(q.dequeue(), &a);
  EXPECT_EQ(q.dequeue(), &b);
  EXPECT_EQ(q.dequeue(), &c);
}

TEST(PriorityBucketRunqueueTest, HighestWordBucketIsFound) {
  // Priority 139 lives in the second 64-bit bitmap word (bit 11 of
  // word 2); verifies the multi-word scan doesn't stop at word 0.
  bucket_queue q;
  task last{{}, /*priority=*/139, /*id=*/1};
  q.enqueue(last);
  EXPECT_EQ(q.peek(), &last);
  EXPECT_EQ(q.dequeue(), &last);
  EXPECT_TRUE(q.empty());
}

TEST(PriorityBucketRunqueueTest, RemoveClearsBucketBit) {
  bucket_queue q;
  task only{{}, 5, 1};
  q.enqueue(only);
  q.remove(only);
  EXPECT_TRUE(q.empty());
  EXPECT_EQ(q.dequeue(), nullptr); // would return stale `only` if the bit wasn't cleared
}

TEST(PriorityBucketRunqueueTest, RemoveLeavesBucketBitSetWhenSiblingsRemain) {
  bucket_queue q;
  task a{{}, 7, 1};
  task b{{}, 7, 2};
  q.enqueue(a);
  q.enqueue(b);

  q.remove(a);
  EXPECT_EQ(q.size(), 1u);
  EXPECT_EQ(q.peek(), &b);
  EXPECT_EQ(q.dequeue(), &b);
  EXPECT_TRUE(q.empty());
}

TEST(PriorityBucketRunqueueTest, IsLinkedTracksEnqueueLifecycle) {
  bucket_queue q;
  task a{{}, 3, 1};
  EXPECT_FALSE(bucket_queue::is_linked(a));

  q.enqueue(a);
  EXPECT_TRUE(bucket_queue::is_linked(a));

  q.remove(a);
  EXPECT_FALSE(bucket_queue::is_linked(a));

  q.enqueue(a);
  EXPECT_EQ(q.dequeue(), &a);
  EXPECT_FALSE(bucket_queue::is_linked(a));
}

} // namespace
