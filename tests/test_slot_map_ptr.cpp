// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#if !defined(_MSC_VER) && defined(__LP64__)
#include <gtest/gtest.h>
#include <structo/slot_map_ptr.hpp>

namespace {

using namespace structo;

struct hw_register {
  uint32_t control;
  uint32_t status;
};

// ============================================================================
// Fake ArchHooks: a tiny software-only stand-in for real page-table
// programming. Each slot just remembers which offset of `fake_ram` it is
// currently "mapped" to and returns a pointer straight into it -- enough to
// exercise slot_map_mapper's pool bookkeeping and slot_map_ptr's RAII
// contract without any real MMU involved.
// ============================================================================
constexpr std::size_t kSlotSize = 64;
alignas(64) std::byte fake_ram[4096];

template <std::size_t NumSlots> struct fake_hooks {
  static constexpr std::size_t slot_size = kSlotSize;

  static inline std::size_t phys_offset[NumSlots] = {};
  static inline bool mapped[NumSlots] = {};
  static inline int program_calls = 0;
  static inline int unprogram_calls = 0;

  static void *slot_base(std::size_t slot) noexcept { return fake_ram + phys_offset[slot]; }

  static result<void> program(std::size_t slot, std::uint64_t phys_aligned) noexcept {
    if (phys_aligned >= sizeof(fake_ram))
      return unexpected(error::out_of_range);
    phys_offset[slot] = static_cast<std::size_t>(phys_aligned);
    mapped[slot] = true;
    ++program_calls;
    return {};
  }

  static void unprogram(std::size_t slot) noexcept {
    mapped[slot] = false;
    ++unprogram_calls;
  }
};

using two_slot_hooks = fake_hooks<2>;
using two_slot_mapper = slot_map_mapper<2, two_slot_hooks, host_phys_space>;
using reg_ptr = slot_map_ptr<hw_register, two_slot_mapper>;

struct SlotMapPtrTest : ::testing::Test {
  void SetUp() override {
    for (std::size_t i = 0; i < 2; ++i) {
      two_slot_hooks::mapped[i] = false;
      two_slot_hooks::phys_offset[i] = 0;
    }
    two_slot_hooks::program_calls = 0;
    two_slot_hooks::unprogram_calls = 0;
  }
};

TEST_F(SlotMapPtrTest, ZeroOverheadLayout) {
  struct hw_table {
    reg_ptr next_table;
  };

  EXPECT_EQ(sizeof(hw_table), sizeof(uint64_t));
  EXPECT_TRUE(std::is_standard_layout_v<hw_table>);
}

TEST_F(SlotMapPtrTest, NullPointerFromPaddr) {
  auto res = reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(nullptr));
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(res.value().is_null());
}

TEST_F(SlotMapPtrTest, MapWriteAndReadThroughGuard) {
  auto ptr_res = reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x100));
  ASSERT_TRUE(ptr_res.has_value());
  reg_ptr ptr = ptr_res.value();
  EXPECT_FALSE(ptr.is_null());

  auto guard_res = ptr.try_map();
  ASSERT_TRUE(guard_res.has_value());
  auto guard = std::move(guard_res.value());
  ASSERT_TRUE(guard);

  guard->control = 0xDEADBEEF;
  guard->status = 0xCAFEBABE;

  EXPECT_EQ(guard->control, 0xDEADBEEFu);
  EXPECT_EQ((*guard).status, 0xCAFEBABEu);
  EXPECT_EQ(guard.get(), reinterpret_cast<hw_register *>(fake_ram + 0x100));
  EXPECT_EQ(two_slot_hooks::program_calls, 1);

  // Unmapped automatically at scope exit (checked in the next test).
}

TEST_F(SlotMapPtrTest, GuardUnmapsOnDestructionAndReset) {
  auto ptr_res = reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x200));
  ASSERT_TRUE(ptr_res.has_value());

  {
    auto guard_res = ptr_res.value().try_map();
    ASSERT_TRUE(guard_res.has_value());
    EXPECT_EQ(two_slot_hooks::unprogram_calls, 0);
  }
  EXPECT_EQ(two_slot_hooks::unprogram_calls, 1);

  auto guard_res2 = ptr_res.value().try_map();
  ASSERT_TRUE(guard_res2.has_value());
  auto guard = std::move(guard_res2.value());
  guard.reset();
  EXPECT_TRUE(guard.is_null());
  EXPECT_EQ(two_slot_hooks::unprogram_calls, 2);

  // reset() is idempotent.
  guard.reset();
  EXPECT_EQ(two_slot_hooks::unprogram_calls, 2);
}

TEST_F(SlotMapPtrTest, PoolExhaustionReturnsBusy) {
  auto ptr_res = reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x100));
  ASSERT_TRUE(ptr_res.has_value());

  auto g1 = ptr_res.value().try_map();
  auto g2 = ptr_res.value().try_map();
  ASSERT_TRUE(g1.has_value());
  ASSERT_TRUE(g2.has_value());

  // Both of the 2 slots are now busy.
  auto g3 = ptr_res.value().try_map();
  EXPECT_FALSE(g3.has_value());
  EXPECT_EQ(g3.error(), error::busy);

  // Releasing one frees it back up for a new acquire.
  g1.value().reset();
  auto g4 = ptr_res.value().try_map();
  EXPECT_TRUE(g4.has_value());
}

TEST_F(SlotMapPtrTest, OversizedMappingRejected) {
  auto ptr_res = reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x100));
  ASSERT_TRUE(ptr_res.has_value());

  auto res = ptr_res.value().try_map(kSlotSize + 1);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::out_of_range);
}

TEST_F(SlotMapPtrTest, MoveTransfersOwnership) {
  auto ptr_res = reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x100));
  ASSERT_TRUE(ptr_res.has_value());

  auto guard_res = ptr_res.value().try_map();
  ASSERT_TRUE(guard_res.has_value());
  auto moved = std::move(guard_res.value());
  ASSERT_TRUE(moved);

  auto moved2(std::move(moved));
  EXPECT_TRUE(moved2);
  EXPECT_TRUE(moved.is_null()); // NOLINT(bugprone-use-after-move) -- intentional: verifying moved-from state.
  EXPECT_EQ(two_slot_hooks::unprogram_calls, 0);
}

TEST_F(SlotMapPtrTest, VoidPointerRequiresExplicitSize) {
  using void_ptr = slot_map_ptr<void, two_slot_mapper>;
  auto ptr_res = void_ptr::from_paddr(phys_addr<void, host_phys_space>(0x100));
  ASSERT_TRUE(ptr_res.has_value());

  auto guard_res = ptr_res.value().try_map(sizeof(hw_register));
  ASSERT_TRUE(guard_res.has_value());
  EXPECT_NE(guard_res.value().get(), nullptr);
}

// ============================================================================
// shared_slot_map_mapper: lock-protected sharing/refcounting over ArchHooks
// ============================================================================

using shared_two_slot_hooks = fake_hooks<2>;
using shared_two_slot_mapper =
    shared_slot_map_mapper<2, shared_two_slot_hooks, reloco::spin_lock, slot_map_no_wait_policy, host_phys_space>;
using shared_reg_ptr = slot_map_ptr<hw_register, shared_two_slot_mapper>;

struct SharedSlotMapPtrTest : ::testing::Test {
  void SetUp() override {
    for (std::size_t i = 0; i < 2; ++i) {
      shared_two_slot_hooks::mapped[i] = false;
      shared_two_slot_hooks::phys_offset[i] = 0;
    }
    shared_two_slot_hooks::program_calls = 0;
    shared_two_slot_hooks::unprogram_calls = 0;
    ASSERT_TRUE(shared_two_slot_mapper::try_init());
  }
};

TEST_F(SharedSlotMapPtrTest, TryInitIsIdempotent) {
  EXPECT_TRUE(shared_two_slot_mapper::try_init());
  EXPECT_TRUE(shared_two_slot_mapper::try_init());
}

TEST_F(SharedSlotMapPtrTest, ConcurrentAcquiresOfTheSamePageShareOneSlot) {
  auto ptr_res = shared_reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x100));
  ASSERT_TRUE(ptr_res.has_value());
  shared_reg_ptr ptr = ptr_res.value();

  auto guard_a_res = ptr.try_map();
  ASSERT_TRUE(guard_a_res.has_value());
  auto guard_a = std::move(guard_a_res.value());

  auto guard_b_res = ptr.try_map();
  ASSERT_TRUE(guard_b_res.has_value());
  auto guard_b = std::move(guard_b_res.value());

  // Same physical page -> same underlying slot/vaddr, programmed only once.
  EXPECT_EQ(guard_a.get(), guard_b.get());
  EXPECT_EQ(shared_two_slot_hooks::program_calls, 1);

  // Releasing the first sharer must not tear the slot down while the second
  // still holds a reference to it.
  guard_a.reset();
  EXPECT_EQ(shared_two_slot_hooks::unprogram_calls, 0);
  ASSERT_TRUE(guard_b);
  guard_b->control = 0x42; // guard_b is still usable.
  EXPECT_EQ(guard_b->control, 0x42u);

  guard_b.reset();
  EXPECT_EQ(shared_two_slot_hooks::unprogram_calls, 1);
}

TEST_F(SharedSlotMapPtrTest, DistinctPagesConsumeDistinctSlotsAndExhaustTheTable) {
  auto ptr1_res = shared_reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x100));
  auto ptr2_res = shared_reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x200));
  auto ptr3_res = shared_reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x300));
  ASSERT_TRUE(ptr1_res.has_value());
  ASSERT_TRUE(ptr2_res.has_value());
  ASSERT_TRUE(ptr3_res.has_value());

  auto g1 = ptr1_res.value().try_map();
  auto g2 = ptr2_res.value().try_map();
  ASSERT_TRUE(g1.has_value());
  ASSERT_TRUE(g2.has_value());
  EXPECT_EQ(shared_two_slot_hooks::program_calls, 2);

  // Both of the 2 slots are now occupied by distinct physical pages.
  auto g3 = ptr3_res.value().try_map();
  EXPECT_FALSE(g3.has_value());
  EXPECT_EQ(g3.error(), error::busy);

  // Releasing one frees its row/slot back up for a new, distinct page.
  g1.value().reset();
  EXPECT_EQ(shared_two_slot_hooks::unprogram_calls, 1);
  auto g4 = ptr3_res.value().try_map();
  EXPECT_TRUE(g4.has_value());
}

// ============================================================================
// A custom, synchronous WaitPolicy: proves acquire() actually calls
// WaitPolicy::wait() when exhausted and re-scans afterward, and that
// release() calls WaitPolicy::notify_all() once a row is freed. Stores
// only an opaque `void*` + plain function pointer (set by the test after
// the mapper's own types are known) to avoid a circular dependency
// between the policy type and the `slot_map_ptr::guard` type that is
// itself parameterized by a mapper using this very policy.
// ============================================================================
struct trigger_on_wait_policy {
  static inline int wait_calls = 0;
  static inline int notify_calls = 0;
  static inline void *pending_release = nullptr;
  static inline void (*release_fn)(void *) = nullptr;

  template <typename LockT> static result<void> wait(LockT &lock) noexcept {
    ++wait_calls;
    if (pending_release != nullptr && release_fn != nullptr) {
      // Must unlock before re-entering the mapper (release_fn() calls
      // release(), which itself locks the same, non-reentrant lock), then
      // relock before returning, per the WaitPolicy contract.
      lock.unlock();
      release_fn(pending_release);
      pending_release = nullptr;
      lock.lock();
    }
    return {}; // Always succeeds; acquire() re-scans for a free row afterward.
  }

  static void notify_all() noexcept { ++notify_calls; }
};

using retry_two_slot_hooks = fake_hooks<2>;
using retry_mapper =
    shared_slot_map_mapper<2, retry_two_slot_hooks, reloco::spin_lock, trigger_on_wait_policy, host_phys_space>;
using retry_reg_ptr = slot_map_ptr<hw_register, retry_mapper>;

struct RetryingSharedSlotMapPtrTest : ::testing::Test {
  void SetUp() override {
    for (std::size_t i = 0; i < 2; ++i) {
      retry_two_slot_hooks::mapped[i] = false;
      retry_two_slot_hooks::phys_offset[i] = 0;
    }
    retry_two_slot_hooks::program_calls = 0;
    retry_two_slot_hooks::unprogram_calls = 0;
    trigger_on_wait_policy::wait_calls = 0;
    trigger_on_wait_policy::notify_calls = 0;
    trigger_on_wait_policy::pending_release = nullptr;
    trigger_on_wait_policy::release_fn = nullptr;
    ASSERT_TRUE(retry_mapper::try_init());
  }
};

TEST_F(RetryingSharedSlotMapPtrTest, AcquireCallsWaitPolicyWhenExhaustedThenRetriesAndSucceeds) {
  auto ptr1_res = retry_reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x100));
  auto ptr2_res = retry_reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x200));
  auto ptr3_res = retry_reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x300));
  ASSERT_TRUE(ptr1_res.has_value());
  ASSERT_TRUE(ptr2_res.has_value());
  ASSERT_TRUE(ptr3_res.has_value());

  auto g1 = ptr1_res.value().try_map();
  auto g2 = ptr2_res.value().try_map();
  ASSERT_TRUE(g1.has_value());
  ASSERT_TRUE(g2.has_value());

  // Arrange for the (only) wait() call to release g1's slot, "waking up"
  // the still-pending acquire() for a 3rd, distinct physical page.
  trigger_on_wait_policy::pending_release = &g1.value();
  trigger_on_wait_policy::release_fn = [](void *p) { static_cast<retry_reg_ptr::guard *>(p)->reset(); };

  auto g3 = ptr3_res.value().try_map();
  EXPECT_TRUE(g3.has_value());
  EXPECT_EQ(trigger_on_wait_policy::wait_calls, 1);
  EXPECT_EQ(trigger_on_wait_policy::notify_calls, 1); // from g1's reset() inside wait().
}

TEST_F(SharedSlotMapPtrTest, OversizedMappingRejected) {
  auto ptr_res = shared_reg_ptr::from_paddr(phys_addr<hw_register, host_phys_space>(0x100));
  ASSERT_TRUE(ptr_res.has_value());

  auto res = ptr_res.value().try_map(kSlotSize + 1);
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::out_of_range);
}

} // namespace
#endif
