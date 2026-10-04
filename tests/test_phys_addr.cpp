// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#if !defined(_MSC_VER) && defined(__LP64__)
#include <gtest/gtest.h>
#include <structo/pfn_translator.hpp>
#include <structo/phys_addr.hpp>

namespace {

using namespace structo;

// Define a dummy hardware struct
struct hw_register {
  uint32_t control;
  uint32_t status;
};

// Define our mappers
constexpr uint64_t VIRT_BASE = UINT64_C(0xFFF0800000000000);
constexpr uint64_t PHYS_SIZE = UINT64_C(0x0000800000000000);

using kernel_dmap = dmap_mapper<VIRT_BASE, PHYS_SIZE, host_phys_space>;
using secure_dmap = dmap_mapper<VIRT_BASE, PHYS_SIZE, secure_phys_space>;

TEST(PhysAddrTest, NullAddressIsTildeZero) {
  phys_addr<void, host_phys_space> p1;
  EXPECT_TRUE(p1.is_null());
  EXPECT_EQ(p1.value, ~uint64_t(0));

  phys_addr<void, host_phys_space> p2 = nullptr;
  EXPECT_TRUE(p2.is_null());

  phys_addr<void, host_phys_space> p3(0x1000);
  EXPECT_FALSE(p3.is_null());
  EXPECT_EQ(p3.value, 0x1000);
}

TEST(PhysAddrTest, TagAndTypeCasting) {
  phys_addr<void, host_phys_space> void_ptr(0x4000);

  // Safe type cast
  phys_addr<hw_register, host_phys_space> reg_ptr = void_ptr.cast_type<hw_register>();
  EXPECT_EQ(reg_ptr.value, 0x4000);

  // Unsafe space cast (Host to Secure)
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE;
  phys_addr<hw_register, secure_phys_space> sec_ptr = reg_ptr.cast_space<secure_phys_space>();
  EXPECT_EQ(sec_ptr.value, 0x4000);
  RELOCO_END_UNSAFE_BUFFER_USAGE;
}

TEST(DmapMapperTest, ValidTranslation) {
  phys_addr<hw_register, host_phys_space> phys(0x1000);

  auto virt_res = kernel_dmap::to_virt(phys);
  ASSERT_TRUE(virt_res.has_value());
  EXPECT_EQ(reinterpret_cast<uintptr_t>(virt_res.value()), VIRT_BASE + 0x1000);

  auto phys_res = kernel_dmap::to_phys(virt_res.value());
  ASSERT_TRUE(phys_res.has_value());
  EXPECT_EQ(phys_res.value().value, 0x1000);
}

TEST(DmapMapperTest, OutOfBoundsTranslation) {
  // Address is beyond PHYS_SIZE (1GB)
  phys_addr<hw_register, host_phys_space> bad_phys(PHYS_SIZE + 0x1000);

  auto virt_res = kernel_dmap::to_virt(bad_phys);
  EXPECT_FALSE(virt_res.has_value());
  EXPECT_EQ(virt_res.error(), error::out_of_range);

  // Virtual address outside the mapping window
  hw_register *bad_virt = reinterpret_cast<hw_register *>(0xFFFF900000000000); // Beyond 1GB window
  auto phys_res = kernel_dmap::to_phys(bad_virt);
  EXPECT_FALSE(phys_res.has_value());
  EXPECT_EQ(phys_res.error(), error::out_of_range);
}

TEST(DmapPtrTest, ZeroOverheadLayout) {
  // A struct containing just the smart pointer
  struct hw_table {
    dmap_ptr<hw_register, kernel_dmap> next_table;
  };

  // Must be exactly the size of the underlying integer (uint64_t)
  EXPECT_EQ(sizeof(hw_table), sizeof(uint64_t));
  EXPECT_TRUE(std::is_standard_layout_v<hw_table>);
}

TEST(DmapPtrTest, TryGetExplicitUnwrapping) {
  // Simulate allocating a register struct in the "virtual direct map"
  hw_register fake_reg{0xDEADBEEF, 0xCAFEBABE};

  // Cheat for the test: pretend this local variable is within our VIRT_BASE mapping
  using test_dmap = dmap_mapper<0, PHYS_SIZE, host_phys_space, 0>;

  auto ptr_res = dmap_ptr<hw_register, test_dmap>::from_virt(&fake_reg);
  ASSERT_TRUE(ptr_res.has_value());

  dmap_ptr<hw_register, test_dmap> ptr = ptr_res.value();
  EXPECT_FALSE(ptr.is_null());

  // The developer is forced to explicitly check the result of try_get()
  auto virt_res = ptr.try_get();
  ASSERT_TRUE(virt_res.has_value());

  hw_register *vaddr = virt_res.value();
  EXPECT_EQ(vaddr->control, 0xDEADBEEF);
  EXPECT_EQ(vaddr->status, 0xCAFEBABE);
}

TEST(DmapPtrTest, TryGetOnNullReturnsError) {
  dmap_ptr<hw_register, kernel_dmap> ptr = nullptr;
  EXPECT_TRUE(ptr.is_null());

  auto res = ptr.try_get();
  EXPECT_FALSE(res.has_value());
  EXPECT_EQ(res.error(), error::invalid_argument);
}

TEST(DmapPtrTest, FactorySecurityViolations) {
  // 1. Try to create a pointer from an out-of-bounds physical address
  phys_addr<hw_register, host_phys_space> bad_phys(PHYS_SIZE + 0x1000);
  auto p_res = dmap_ptr<hw_register, kernel_dmap>::from_paddr(bad_phys);
  EXPECT_FALSE(p_res.has_value());
  EXPECT_EQ(p_res.error(), error::security_violation);

  // 2. Try to create a pointer from an out-of-bounds virtual address
  hw_register *bad_virt = reinterpret_cast<hw_register *>(0xFFFF900000000000);
  auto v_res = dmap_ptr<hw_register, kernel_dmap>::from_virt(bad_virt);
  EXPECT_FALSE(v_res.has_value());
  EXPECT_EQ(v_res.error(), error::out_of_range);
}

TEST(DmapPtrTest, TryMapMatchesSlotMapPtrGuardApi) {
  // Same trick as TryGetExplicitUnwrapping: pretend `fake_reg` lives at
  // VIRT_BASE+0 so the fixed offset mapper resolves it back to itself.
  hw_register fake_reg{0xDEADBEEF, 0xCAFEBABE};
  using test_dmap = dmap_mapper<0, PHYS_SIZE, host_phys_space, 0>;

  auto ptr_res = dmap_ptr<hw_register, test_dmap>::from_virt(&fake_reg);
  ASSERT_TRUE(ptr_res.has_value());

  auto guard_res = ptr_res.value().try_map();
  ASSERT_TRUE(guard_res.has_value());
  auto guard = std::move(guard_res.value());
  ASSERT_TRUE(guard);

  EXPECT_EQ(guard->control, 0xDEADBEEFu);
  EXPECT_EQ((*guard).status, 0xCAFEBABEu);
  EXPECT_EQ(guard.get(), &fake_reg);

  // Unlike slot_map_ptr::guard, reset()/destruction is a no-op: the direct
  // map is permanent, so there is nothing to tear down.
  guard.reset();
  EXPECT_TRUE(guard.is_null());

  // Null dmap_ptr -> try_map() fails exactly like try_get() does.
  dmap_ptr<hw_register, kernel_dmap> null_ptr = nullptr;
  auto null_res = null_ptr.try_map();
  EXPECT_FALSE(null_res.has_value());
  EXPECT_EQ(null_res.error(), error::invalid_argument);
}

TEST(DmapPtrTest, VoidPointerSupport) {
  // Ensure that void pointers can still be passed around and translated
  dmap_ptr<void, kernel_dmap> void_ptr;
  EXPECT_TRUE(void_ptr.is_null());

  auto res = dmap_ptr<void, kernel_dmap>::from_paddr(phys_addr<void, host_phys_space>(0x2000));
  ASSERT_TRUE(res.has_value());

  auto virt_res = res.value().try_get();
  ASSERT_TRUE(virt_res.has_value());
  EXPECT_EQ(reinterpret_cast<uintptr_t>(virt_res.value()), VIRT_BASE + 0x2000);
}

TEST(PhysPfnTest, ConvertsAddressesAndAppliesPageMath) {
  using pfn_type = phys_pfn<guest_phys_space, page_4k>;
  phys_addr<void, guest_phys_space> address{0x12345};

  const auto pfn = pfn_type::from_addr(address);
  EXPECT_EQ(pfn.value, 0x12u);
  EXPECT_EQ(pfn.to_addr().value, 0x12000u);
  EXPECT_EQ(page_math::offset<page_4k>(address), 0x345u);
  EXPECT_EQ(page_math::align_down<page_4k>(address).value, 0x12000u);
  EXPECT_EQ(page_math::align_up<page_4k>(address).value, 0x13000u);

  const phys_addr<void, guest_phys_space> null_address{nullptr};
  EXPECT_TRUE(pfn_type::from_addr(null_address).is_null());
  EXPECT_TRUE(pfn_type{}.to_addr().is_null());
  EXPECT_EQ(page_math::offset<page_4k>(null_address), 0u);
  EXPECT_TRUE(page_math::align_up<page_4k>(null_address).is_null());
}

} // namespace
#endif