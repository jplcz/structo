// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/arm/pte_stage1.hpp>
#include <structo/arch/arm/pte_stage2.hpp>
#include <structo/arch/arm64/pte_stage1.hpp>
#include <structo/arch/arm64/pte_stage2.hpp>
#include <structo/arch/riscv/pte.hpp>
#include <structo/arch/x86/pte.hpp>
#include <structo/arch/x86/pte_ept.hpp>
#include <structo/phys_page.hpp>

#include <cstdint>

namespace {

class ArchPteFieldsTest : public ::testing::Test {};

} // namespace

// --- ARM64 stage 1 -------------------------------------------------------

TEST_F(ArchPteFieldsTest, Arm64Stage1NsLeafRoundTripsFrameAndFields) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::arm64::stage1_ns_tag<>>;
  auto leaf = traits::make_leaf_entry(traits::phys_type{0x1234'5000ull}, 0b01, 0b11, 2, /*final_level=*/true,
                                      /*af=*/true, /*ng=*/true, /*contiguous=*/false, /*pxn=*/false, /*uxn=*/true);
  EXPECT_TRUE(traits::is_present(leaf));
  EXPECT_EQ(traits::leaf_frame_addr(leaf).value, 0x1234'5000ull);
  EXPECT_EQ(traits::ap(leaf), 0b01u);
  EXPECT_EQ(traits::sh(leaf), 0b11u);
  EXPECT_EQ(traits::attr_indx(leaf), 2u);
  EXPECT_EQ(traits::af(leaf), 1u);
  EXPECT_TRUE(traits::ng(leaf));
  EXPECT_TRUE(traits::uxn(leaf));
  EXPECT_FALSE(traits::pxn(leaf));
}

TEST_F(ArchPteFieldsTest, Arm64Stage1NsBlockIsLeafButPageIsNot) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::arm64::stage1_ns_tag<>>;
  auto block = traits::make_leaf_entry(traits::phys_type{0x2000'0000ull}, 0, 0, 0, /*final_level=*/false);
  auto page = traits::make_leaf_entry(traits::phys_type{0x2100'0000ull}, 0, 0, 0, /*final_level=*/true);
  EXPECT_TRUE(traits::is_leaf(block)); // bit 1 == 0 at an intermediate level
  EXPECT_FALSE(traits::is_leaf(page)); // bit 1 == 1: a "page" descriptor, not asked about via is_leaf()
  EXPECT_EQ(traits::leaf_frame_addr(page).value, 0x2100'0000ull);
}

TEST_F(ArchPteFieldsTest, Arm64Stage1NsTableEntryExposesTableAttributes) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::arm64::stage1_ns_tag<>>;
  auto tbl = traits::make_table_entry(traits::phys_type{0x3000'0000ull}, /*xn_table=*/true, /*pxn_table=*/true,
                                      /*ap_table=*/0b10);
  EXPECT_FALSE(traits::is_leaf(tbl));
  EXPECT_EQ(traits::child_table_addr(tbl).value, 0x3000'0000ull);
  EXPECT_TRUE(traits::xn_table(tbl));
  EXPECT_TRUE(traits::pxn_table(tbl));
  EXPECT_EQ(traits::ap_table(tbl), 0b10u);
}

TEST_F(ArchPteFieldsTest, Arm64Stage1SecureNsBitSelectsOutputSpacePerEntry) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::arm64::stage1_secure_tag<>>;
  auto secure_out = traits::make_leaf_entry(traits::phys_type{0x4000'0000ull}, 0, 0, 0, /*ns=*/false);
  auto nonsecure_out = traits::make_leaf_entry(traits::phys_type{0x5000'0000ull}, 0, 0, 0, /*ns=*/true);
  EXPECT_FALSE(traits::ns(secure_out));
  EXPECT_TRUE(traits::ns(nonsecure_out));
}

TEST_F(ArchPteFieldsTest, Arm64Stage1SecureEl2OutputIsFixedSecure) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::arm64::stage1_secure_el2_tag<>>;
  static_assert(std::is_same_v<traits::phys_type, structo::phys_addr<void, structo::secure_phys_space>>);
  auto leaf = traits::make_leaf_entry(traits::phys_type{0x6000'0000ull}, 0, 0, 0);
  EXPECT_EQ(traits::leaf_frame_addr(leaf).value, 0x6000'0000ull);
}

TEST_F(ArchPteFieldsTest, Arm64Stage1Supports16kAnd64kGranules) {
  using traits16k = structo::arch::page_table_entry_traits<structo::arch::arm64::stage1_ns_tag<structo::page_16k>>;
  auto leaf16k = traits16k::make_leaf_entry(traits16k::phys_type{0x1'0000'4000ull}, 0, 0, 0);
  EXPECT_EQ(traits16k::leaf_frame_addr(leaf16k).value, 0x1'0000'4000ull);

  using traits64k = structo::arch::page_table_entry_traits<structo::arch::arm64::stage1_ns_tag<structo::page_64k>>;
  auto leaf64k = traits64k::make_leaf_entry(traits64k::phys_type{0x2'0001'0000ull}, 0, 0, 0);
  EXPECT_EQ(traits64k::leaf_frame_addr(leaf64k).value, 0x2'0001'0000ull);
}

// --- ARM64 stage 2 (hypervisor staging) -----------------------------------

TEST_F(ArchPteFieldsTest, Arm64Stage2OrdinaryLeafExposesS2apAndMemAttr) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::arm64::stage2_tag<>>;
  static_assert(std::is_same_v<traits::phys_type, structo::phys_addr<void, structo::host_phys_space>>);
  auto leaf = traits::make_leaf_entry(traits::phys_type{0x7000'0000ull}, 0b11, 0b10, 0xF);
  EXPECT_TRUE(traits::readable(leaf));
  EXPECT_TRUE(traits::writable(leaf));
  EXPECT_EQ(traits::mem_attr(leaf), 0xFu);
  EXPECT_EQ(traits::leaf_frame_addr(leaf).value, 0x7000'0000ull);
}

TEST_F(ArchPteFieldsTest, Arm64Stage2SecureOutputIsFixedSecure) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::arm64::stage2_secure_tag<>>;
  static_assert(std::is_same_v<traits::phys_type, structo::phys_addr<void, structo::secure_phys_space>>);
  auto leaf = traits::make_leaf_entry(traits::phys_type{0x8000'0000ull}, 0b01, 0, 0);
  EXPECT_TRUE(traits::readable(leaf));
  EXPECT_FALSE(traits::writable(leaf));
}

// --- ARMv7 LPAE ------------------------------------------------------------

TEST_F(ArchPteFieldsTest, ArmLpaeStage1AndStage2ShareArm64BitLayout) {
  using s1traits = structo::arch::page_table_entry_traits<structo::arch::arm::lpae::stage1_ns_tag>;
  auto leaf = s1traits::make_leaf_entry(s1traits::phys_type{0x9000'0000ull}, 0b01, 0b10, 1);
  EXPECT_EQ(s1traits::leaf_frame_addr(leaf).value, 0x9000'0000ull);

  using s2traits = structo::arch::page_table_entry_traits<structo::arch::arm::lpae::stage2_tag>;
  auto s2leaf = s2traits::make_leaf_entry(s2traits::phys_type{0xA000'0000ull}, 0b11, 0, 0x4);
  EXPECT_TRUE(s2traits::writable(s2leaf));
  EXPECT_EQ(s2traits::leaf_frame_addr(s2leaf).value, 0xA000'0000ull);
}

// --- RISC-V ----------------------------------------------------------------

TEST_F(ArchPteFieldsTest, RiscvPteLeafEncodesRwxAndRoundTripsFrame) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::riscv::pte_tag>;
  auto leaf = traits::make_leaf_entry(traits::phys_type{0x8020'0000ull}, true, true, false);
  EXPECT_TRUE(traits::is_present(leaf));
  EXPECT_TRUE(traits::is_leaf(leaf));
  EXPECT_TRUE(traits::readable(leaf));
  EXPECT_TRUE(traits::writable(leaf));
  EXPECT_FALSE(traits::executable(leaf));
  EXPECT_EQ(traits::leaf_frame_addr(leaf).value, 0x8020'0000ull);
}

TEST_F(ArchPteFieldsTest, RiscvPteTableEntryIsNotLeaf) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::riscv::pte_tag>;
  auto tbl = traits::make_table_entry(traits::phys_type{0x8030'0000ull});
  EXPECT_TRUE(traits::is_present(tbl));
  EXPECT_FALSE(traits::is_leaf(tbl));
  EXPECT_EQ(traits::child_table_addr(tbl).value, 0x8030'0000ull);
}

TEST_F(ArchPteFieldsTest, RiscvGStageSharesSameLayoutAsSStage) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::riscv::pte_g_stage_tag>;
  static_assert(std::is_same_v<traits::phys_type, structo::phys_addr<void, structo::host_phys_space>>);
  auto leaf = traits::make_leaf_entry(traits::phys_type{0x9000'0000ull}, true, false, false);
  EXPECT_TRUE(traits::readable(leaf));
  EXPECT_FALSE(traits::writable(leaf));
}

// --- x86 ---------------------------------------------------------------

TEST_F(ArchPteFieldsTest, X86PteLeafRoundTripsFrameAndPermissionBits) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::x86::pte_tag>;
  auto leaf = traits::make_leaf_entry(traits::phys_type{0xA000'0000ull}, true, true);
  EXPECT_TRUE(traits::is_present(leaf));
  EXPECT_TRUE(traits::writable(leaf));
  EXPECT_TRUE(traits::user_accessible(leaf));
  EXPECT_EQ(traits::leaf_frame_addr(leaf).value, 0xA000'0000ull);
}

TEST_F(ArchPteFieldsTest, X86PteHugePageSetsPsBitDistinctFromFinalLevelPage) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::x86::pte_tag>;
  auto huge = traits::make_leaf_entry(traits::phys_type{0xC000'0000ull}, true, false, /*final_level=*/false);
  auto page = traits::make_leaf_entry(traits::phys_type{0xC100'0000ull}, true, false, /*final_level=*/true);
  EXPECT_TRUE(traits::is_leaf(huge));  // PS == 1
  EXPECT_FALSE(traits::is_leaf(page)); // bit 7 here means PAT, not PS
}

TEST_F(ArchPteFieldsTest, X86NptSharesSameLayoutAsOrdinaryPaging) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::x86::npt_tag>;
  static_assert(std::is_same_v<traits::phys_type, structo::phys_addr<void, structo::host_phys_space>>);
  auto leaf = traits::make_leaf_entry(traits::phys_type{0xB000'0000ull}, true, true);
  EXPECT_EQ(traits::leaf_frame_addr(leaf).value, 0xB000'0000ull);
}

TEST_F(ArchPteFieldsTest, X86EptPresentIsDerivedFromReadWriteExecuteNotADedicatedBit) {
  using traits = structo::arch::page_table_entry_traits<structo::arch::x86::ept_tag>;
  auto not_present = structo::arch::page_table_entry<structo::arch::x86::ept_tag>{std::uint64_t{0}};
  EXPECT_FALSE(traits::is_present(not_present));

  auto leaf = traits::make_leaf_entry(traits::phys_type{0xD000'0000ull}, true, true, true, /*mem_type=*/6);
  EXPECT_TRUE(traits::is_present(leaf));
  EXPECT_TRUE(traits::readable(leaf));
  EXPECT_TRUE(traits::writable(leaf));
  EXPECT_TRUE(traits::executable(leaf));
  EXPECT_EQ(traits::mem_type(leaf), 6u);
  EXPECT_EQ(traits::leaf_frame_addr(leaf).value, 0xD000'0000ull);
}
