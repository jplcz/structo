// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/cpu_online_dispatcher.hpp>

#include <cstddef>
#include <string>

using structo::arch::cpu_online_dispatcher;
using structo::arch::physical_cpu_tag;

namespace {

/** @brief Fixture for `cpu_online_dispatcher` tests. */
class CpuOnlineDispatcherTest : public ::testing::Test {};

using dispatcher = cpu_online_dispatcher<physical_cpu_tag, 70>; // deliberately not a multiple of 64
using mask_type = dispatcher::mask_type;

/** @brief No-op hotplug callback for tests that don't care about it. */
constexpr auto no_op = [](std::size_t, const mask_type &) noexcept {};

} // namespace

TEST_F(CpuOnlineDispatcherTest, DefaultConstructedHasNoCpusOnline) {
  dispatcher d;
  EXPECT_TRUE(d.snapshot().none());
}

TEST_F(CpuOnlineDispatcherTest, MarkOnlineSetsExactlyOneBit) {
  dispatcher d;
  d.mark_online(3, no_op);

  mask_type mask = d.snapshot();
  EXPECT_EQ(mask.count(), 1u);
  EXPECT_TRUE(mask.test(3));
}

TEST_F(CpuOnlineDispatcherTest, MarkOnlineTwiceAccumulates) {
  dispatcher d;
  d.mark_online(0, no_op);
  d.mark_online(5, no_op);
  d.mark_online(69, no_op); // highest valid bit for max_cpus == 70

  mask_type mask = d.snapshot();
  EXPECT_EQ(mask.count(), 3u);
  EXPECT_TRUE(mask.test(0));
  EXPECT_TRUE(mask.test(5));
  EXPECT_TRUE(mask.test(69));
}

TEST_F(CpuOnlineDispatcherTest, MarkOfflineClearsOnlyThatBit) {
  dispatcher d;
  d.mark_online(0, no_op);
  d.mark_online(5, no_op);
  d.mark_offline(5, no_op);

  mask_type mask = d.snapshot();
  EXPECT_EQ(mask.count(), 1u);
  EXPECT_TRUE(mask.test(0));
  EXPECT_FALSE(mask.test(5));
}

TEST_F(CpuOnlineDispatcherTest, MarkOnlineCallbackSeesMaskBeforeBitIsSet) {
  dispatcher d;
  d.mark_online(0, no_op);

  bool saw_target_offline_in_callback = false;
  d.mark_online(5, [&](std::size_t cpu, const mask_type &mask) {
    EXPECT_EQ(cpu, 5u);
    saw_target_offline_in_callback = !mask.test(5);
  });

  EXPECT_TRUE(saw_target_offline_in_callback);
  EXPECT_TRUE(d.snapshot().test(5)); // bit is set once mark_online() returns
}

TEST_F(CpuOnlineDispatcherTest, MarkOfflineCallbackSeesMaskAfterBitIsCleared) {
  dispatcher d;
  d.mark_online(0, no_op);
  d.mark_online(5, no_op);

  bool saw_target_already_cleared_in_callback = false;
  d.mark_offline(5, [&](std::size_t cpu, const mask_type &mask) {
    EXPECT_EQ(cpu, 5u);
    saw_target_already_cleared_in_callback = !mask.test(5);
  });

  EXPECT_TRUE(saw_target_already_cleared_in_callback);
}

TEST_F(CpuOnlineDispatcherTest, SnapshotReturnsIndependentCopy) {
  dispatcher d;
  d.mark_online(1, no_op);

  mask_type first = d.snapshot();
  d.mark_online(2, no_op);
  mask_type second = d.snapshot();

  EXPECT_EQ(first.count(), 1u);
  EXPECT_EQ(second.count(), 2u);
}

TEST_F(CpuOnlineDispatcherTest, WithOnlineMaskSingleCallbackSeesConsistentSnapshot) {
  dispatcher d;
  d.mark_online(2, no_op);
  d.mark_online(4, no_op);

  std::size_t count = d.with_online_mask([](const mask_type &mask) { return mask.count(); });
  EXPECT_EQ(count, 2u);
}

TEST_F(CpuOnlineDispatcherTest, WithOnlineMaskSingleCallbackReturnValuePropagates) {
  dispatcher d;
  d.mark_online(0, no_op);

  std::string label =
      d.with_online_mask([](const mask_type &mask) { return std::string("online=") + std::to_string(mask.count()); });
  EXPECT_EQ(label, "online=1");
}

TEST_F(CpuOnlineDispatcherTest, WithOnlineMaskTwoCallbackFormPassesPreprocessedValueToCompute) {
  dispatcher d;
  d.mark_online(1, no_op);
  d.mark_online(2, no_op);
  d.mark_online(3, no_op);

  auto result =
      d.with_online_mask([](const mask_type &mask) { return mask.count() * 10; },
                         [](const mask_type &mask, std::size_t preprocessed) { return preprocessed + mask.count(); });

  EXPECT_EQ(result, 33u); // preprocess: 3 * 10 == 30, compute: 30 + 3 == 33
}

TEST_F(CpuOnlineDispatcherTest, WithOnlineMaskTwoCallbackFormPreprocessSeesSameMaskAsCompute) {
  dispatcher d;
  d.mark_online(7, no_op);

  bool masks_matched = false;
  int unused = d.with_online_mask([](const mask_type &mask) { return mask; },
                                  [&](const mask_type &mask, const mask_type &preprocessed) {
                                    masks_matched = (mask == preprocessed);
                                    return 0;
                                  });
  (void)unused;

  EXPECT_TRUE(masks_matched);
}

TEST_F(CpuOnlineDispatcherTest, BeginReadManualTransactionVerifiesAndExtracts) {
  dispatcher d;
  d.mark_online(6, no_op);

  auto tx = d.begin_read();
  ASSERT_TRUE(tx.verify());
  mask_type mask = tx.extract();
  EXPECT_TRUE(mask.test(6));
  EXPECT_EQ(mask.count(), 1u);
}
