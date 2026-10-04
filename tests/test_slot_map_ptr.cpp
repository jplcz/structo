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

} // namespace
#endif
