// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/arm/page_table_traits.hpp>
#include <structo/arch/arm64/page_table_traits.hpp>
#include <structo/arch/riscv/page_table_traits.hpp>
#include <structo/arch/x86/page_table_traits.hpp>

#include <cstdint>

namespace {

class ArchPageTableTraitsTest : public ::testing::Test {};

// A small helper: decompose `va` with `Config` and reassemble it from the
// pieces, asserting the reassembly reproduces `va` exactly (no gap, no
// overlap, across every level including the page offset).
template <typename Config, std::size_t... Is>
constexpr std::uint64_t reassemble_impl(std::uint64_t va, std::index_sequence<Is...>) {
  std::uint64_t acc = Config::page_offset(va);
  ((acc |= static_cast<std::uint64_t>(Config::template index_of<Is>(va))
           << Config::template level<Is>::shift),
   ...);
  return acc;
}

} // namespace

// --- ARM64 (VMSAv8-64), 4KB granule ----------------------------------

TEST_F(ArchPageTableTraitsTest, Arm64Level1ShapeMatchesThreeLevel39BitVa) {
  using cfg = structo::arch::arm64::level1;
  static_assert(cfg::level_count == 3);
  static_assert(cfg::va_bits == 39);
  static_assert(cfg::entry_count<0>() == 512);
  static_assert(cfg::entry_count<1>() == 512);
  static_assert(cfg::entry_count<2>() == 512);
  static_assert(cfg::allows_leaf<0>());
  static_assert(cfg::allows_leaf<1>());

  constexpr std::uint64_t va = 0x0000'007f'1234'5000ull;
  EXPECT_EQ(cfg::index_of<0>(va), (va >> 30) & 0x1FFu);
  EXPECT_EQ(cfg::index_of<1>(va), (va >> 21) & 0x1FFu);
  EXPECT_EQ(cfg::index_of<2>(va), (va >> 12) & 0x1FFu);
}

TEST_F(ArchPageTableTraitsTest, Arm64Level2ShapeMatchesTwoLevel30BitVa) {
  using cfg = structo::arch::arm64::level2;
  static_assert(cfg::level_count == 2);
  static_assert(cfg::va_bits == 30);
  static_assert(cfg::entry_count<0>() == 512);
  static_assert(cfg::entry_count<1>() == 512);

  constexpr std::uint64_t va = 0x0000'0000'3ff0'0123ull;
  EXPECT_EQ(cfg::index_of<0>(va), (va >> 21) & 0x1FFu);
  EXPECT_EQ(cfg::index_of<1>(va), (va >> 12) & 0x1FFu);
  EXPECT_EQ(cfg::page_offset(va), va & 0xFFFu);
}

// --- ARM64 (VMSAv8-64), 16KB granule -----------------------------------

TEST_F(ArchPageTableTraitsTest, Arm64Level1_16kShapeMatchesThreeLevel47BitVa) {
  using cfg = structo::arch::arm64::level1_16k;
  static_assert(cfg::level_count == 3);
  static_assert(cfg::va_bits == 47);
  static_assert(cfg::entry_count<0>() == 2048);
  static_assert(cfg::entry_count<1>() == 2048);
  static_assert(cfg::entry_count<2>() == 2048);
  static_assert(!cfg::allows_leaf<0>()); // no level-1 block mapping for 16KB granule
  static_assert(cfg::allows_leaf<1>());  // 32MB block

  constexpr std::uint64_t va = 0x0000'3ab1'2345'6000ull;
  EXPECT_EQ(cfg::index_of<0>(va), (va >> 36) & 0x7FFu);
  EXPECT_EQ(cfg::index_of<1>(va), (va >> 25) & 0x7FFu);
  EXPECT_EQ(cfg::index_of<2>(va), (va >> 14) & 0x7FFu);
  EXPECT_EQ(cfg::page_offset(va), va & 0x3FFFu);
}

TEST_F(ArchPageTableTraitsTest, Arm64Level2_16kShapeMatchesTwoLevel36BitVa) {
  using cfg = structo::arch::arm64::level2_16k;
  static_assert(cfg::level_count == 2);
  static_assert(cfg::va_bits == 36);
  static_assert(cfg::entry_count<0>() == 2048);
  static_assert(cfg::entry_count<1>() == 2048);
  static_assert(cfg::allows_leaf<0>()); // 32MB block
}

// --- ARM64 (VMSAv8-64), 64KB granule -----------------------------------

TEST_F(ArchPageTableTraitsTest, Arm64Level1_64kRootIsFoldedToSixIndexBits) {
  using cfg = structo::arch::arm64::level1_64k;
  static_assert(cfg::level_count == 3);
  static_assert(cfg::va_bits == 48);
  static_assert(cfg::entry_count<0>() == 64); // folded root: 6 index bits, not the full 13
  static_assert(cfg::entry_count<1>() == 8192);
  static_assert(cfg::entry_count<2>() == 8192);
  static_assert(!cfg::allows_leaf<0>()); // no level-1 block mapping for 64KB granule
  static_assert(cfg::allows_leaf<1>());  // 512MB block

  constexpr std::uint64_t va = (40ull << 42) | (1000ull << 29) | (900ull << 16) | 0x456ull;
  EXPECT_EQ(cfg::index_of<0>(va), 40u);
  EXPECT_EQ(cfg::index_of<1>(va), 1000u);
  EXPECT_EQ(cfg::index_of<2>(va), 900u);
  EXPECT_EQ(cfg::page_offset(va), 0x456u);
}

TEST_F(ArchPageTableTraitsTest, Arm64Level2_64kShapeMatchesTwoLevel42BitVa) {
  using cfg = structo::arch::arm64::level2_64k;
  static_assert(cfg::level_count == 2);
  static_assert(cfg::va_bits == 42);
  static_assert(cfg::entry_count<0>() == 8192);
  static_assert(cfg::entry_count<1>() == 8192);
  static_assert(cfg::allows_leaf<0>()); // 512MB block
}

// --- ARMv7 LPAE, 4KB granule ------------------------------------------

TEST_F(ArchPageTableTraitsTest, ArmLpaeLevel1RootHasOnlyTwoIndexBits) {
  using cfg = structo::arch::arm::lpae::level1;
  static_assert(cfg::level_count == 3);
  static_assert(cfg::va_bits == 32);
  static_assert(cfg::entry_count<0>() == 4); // only 2 index bits at the root
  static_assert(cfg::entry_count<1>() == 512);
  static_assert(cfg::entry_count<2>() == 512);

  constexpr std::uint64_t va = 0xffff'f000ull;
  EXPECT_EQ(cfg::index_of<0>(va), (va >> 30) & 0x3u);
  EXPECT_EQ(cfg::index_of<1>(va), (va >> 21) & 0x1FFu);
  EXPECT_EQ(cfg::index_of<2>(va), (va >> 12) & 0x1FFu);
}

TEST_F(ArchPageTableTraitsTest, ArmLpaeLevel2ShapeMatchesArm64Level2) {
  using cfg = structo::arch::arm::lpae::level2;
  static_assert(cfg::level_count == 2);
  static_assert(cfg::va_bits == 30);
  static_assert(cfg::entry_count<0>() == 512);
  static_assert(cfg::entry_count<1>() == 512);
}

// --- RISC-V -------------------------------------------------------------

TEST_F(ArchPageTableTraitsTest, Sv39HasThreeLevelsAndEveryLevelAllowsLeaf) {
  using cfg = structo::arch::riscv::sv39;
  static_assert(cfg::level_count == 3);
  static_assert(cfg::va_bits == 39);
  static_assert(cfg::allows_leaf<0>());
  static_assert(cfg::allows_leaf<1>());
  static_assert(cfg::allows_leaf<2>());
}

TEST_F(ArchPageTableTraitsTest, Sv48HasFourLevelsAndEveryLevelAllowsLeaf) {
  using cfg = structo::arch::riscv::sv48;
  static_assert(cfg::level_count == 4);
  static_assert(cfg::va_bits == 48);
  static_assert(cfg::allows_leaf<0>());
  static_assert(cfg::allows_leaf<1>());
  static_assert(cfg::allows_leaf<2>());
  static_assert(cfg::allows_leaf<3>());
}

TEST_F(ArchPageTableTraitsTest, Sv57HasFiveLevelsAndEveryLevelAllowsLeaf) {
  using cfg = structo::arch::riscv::sv57;
  static_assert(cfg::level_count == 5);
  static_assert(cfg::va_bits == 57);
  static_assert(cfg::allows_leaf<0>());
  static_assert(cfg::allows_leaf<1>());
  static_assert(cfg::allows_leaf<2>());
  static_assert(cfg::allows_leaf<3>());
  static_assert(cfg::allows_leaf<4>());
}

TEST_F(ArchPageTableTraitsTest, Sv48IndexDecompositionRoundTrips) {
  using cfg = structo::arch::riscv::sv48;
  constexpr std::uint64_t va = 0x0000'1234'5678'9abcull & ((std::uint64_t(1) << 48) - 1);
  auto rebuilt = reassemble_impl<cfg>(va, std::make_index_sequence<4>{});
  EXPECT_EQ(rebuilt, va);
}

TEST_F(ArchPageTableTraitsTest, Arm64Level1_64kIndexDecompositionRoundTrips) {
  using cfg = structo::arch::arm64::level1_64k;
  constexpr std::uint64_t va = 0x0000'8765'4321'0abcull & ((std::uint64_t(1) << 48) - 1);
  auto rebuilt = reassemble_impl<cfg>(va, std::make_index_sequence<3>{});
  EXPECT_EQ(rebuilt, va);
}

// --- x86 / x86-64 ---------------------------------------------------

TEST_F(ArchPageTableTraitsTest, I386RootAllowsFourMegabytePseLeaf) {
  using cfg = structo::arch::x86::i386;
  static_assert(cfg::level_count == 2);
  static_assert(cfg::va_bits == 32);
  static_assert(cfg::entry_count<0>() == 1024);
  static_assert(cfg::entry_count<1>() == 1024);
  static_assert(cfg::allows_leaf<0>());

  constexpr std::uint64_t va = 0xdead'b000ull;
  EXPECT_EQ(cfg::index_of<0>(va), (va >> 22) & 0x3FFu);
  EXPECT_EQ(cfg::index_of<1>(va), (va >> 12) & 0x3FFu);
}

TEST_F(ArchPageTableTraitsTest, PaeRootDisallowsLeafButPdeAllowsTwoMegabyte) {
  using cfg = structo::arch::x86::pae;
  static_assert(cfg::level_count == 3);
  static_assert(cfg::va_bits == 32);
  static_assert(cfg::entry_count<0>() == 4); // PDPTE: only 4 entries
  static_assert(!cfg::allows_leaf<0>());
  static_assert(cfg::allows_leaf<1>());
  static_assert(cfg::allows_leaf<2>());
}

TEST_F(ArchPageTableTraitsTest, LongMode4LevelPml4DisallowsLeafButLowerLevelsAllow) {
  using cfg = structo::arch::x86::long_mode_4level;
  static_assert(cfg::level_count == 4);
  static_assert(cfg::va_bits == 48);
  static_assert(!cfg::allows_leaf<0>()); // PML4
  static_assert(cfg::allows_leaf<1>());  // PDPTE: 1GB pages
  static_assert(cfg::allows_leaf<2>());  // PDE: 2MB pages
  static_assert(cfg::allows_leaf<3>());

  constexpr std::uint64_t va = 0x0000'7fff'ffff'f000ull;
  auto rebuilt = reassemble_impl<cfg>(va, std::make_index_sequence<4>{});
  EXPECT_EQ(rebuilt, va);
}

TEST_F(ArchPageTableTraitsTest, LongMode5LevelPml5AndPml4BothDisallowLeaf) {
  using cfg = structo::arch::x86::long_mode_5level;
  static_assert(cfg::level_count == 5);
  static_assert(cfg::va_bits == 57);
  static_assert(!cfg::allows_leaf<0>()); // PML5
  static_assert(!cfg::allows_leaf<1>()); // PML4
  static_assert(cfg::allows_leaf<2>());
  static_assert(cfg::allows_leaf<3>());
  static_assert(cfg::allows_leaf<4>());
}
