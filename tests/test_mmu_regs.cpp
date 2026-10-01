// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/arch/arm/mmu_regs.hpp>
#include <structo/arch/arm64/mmu_regs.hpp>
#include <structo/arch/riscv/mmu_regs.hpp>
#include <structo/arch/x86/mmu_regs.hpp>

#include <cstdint>

// This file exercises only the pure, portable parse/build bit-arithmetic
// of each register value type (`from_raw()`, the named get/set
// accessors). The `read()`/`write()` static methods compiled on a real
// target are privileged register accesses and cannot be exercised from
// an unprivileged test process, matching the established convention for
// `structo/arch/<arch>/irq_guard.hpp`.

namespace {

class MmuRegsTest : public ::testing::Test {};

} // namespace

// --- ARM64 ----------------------------------------------------------------

TEST_F(MmuRegsTest, Arm64SctlrRoundTripsNamedBits) {
  using namespace structo::arch::arm64;
  sctlr_el1 s{};
  s.set_mmu_enabled(true).set_dcache_enabled(true).set_icache_enabled(true).set_write_xn(true);
  EXPECT_TRUE(s.mmu_enabled());
  EXPECT_TRUE(s.dcache_enabled());
  EXPECT_TRUE(s.icache_enabled());
  EXPECT_TRUE(s.write_xn());
  EXPECT_FALSE(s.alignment_check_enabled());
  EXPECT_FALSE(s.sp_alignment_check_enabled());

  auto reparsed = sctlr_el1::from_raw(s.raw);
  EXPECT_TRUE(reparsed.mmu_enabled());
  EXPECT_TRUE(reparsed.write_xn());
}

TEST_F(MmuRegsTest, Arm64TcrRoundTripsEveryNamedField) {
  using namespace structo::arch::arm64;
  tcr_el1 tcr{};
  tcr.set_t0sz(16)
      .set_t1sz(16)
      .set_tg0(0)
      .set_tg1(2)
      .set_ips(0b101)
      .set_irgn0(0b01)
      .set_orgn0(0b01)
      .set_sh0(0b11)
      .set_irgn1(0b10)
      .set_orgn1(0b10)
      .set_sh1(0b10)
      .set_asid_16bit(true)
      .set_asid_from_ttbr1(true)
      .set_tbi0(true)
      .set_tbi1(false)
      .set_hw_access_flag(true)
      .set_hw_dirty_state(true)
      .set_epd0(false)
      .set_epd1(true);

  EXPECT_EQ(tcr.t0sz(), 16u);
  EXPECT_EQ(tcr.t1sz(), 16u);
  EXPECT_EQ(tcr.tg0(), 0u);
  EXPECT_EQ(tcr.tg1(), 2u);
  EXPECT_EQ(tcr.ips(), 0b101u);
  EXPECT_EQ(tcr.irgn0(), 0b01u);
  EXPECT_EQ(tcr.orgn0(), 0b01u);
  EXPECT_EQ(tcr.sh0(), 0b11u);
  EXPECT_EQ(tcr.irgn1(), 0b10u);
  EXPECT_EQ(tcr.orgn1(), 0b10u);
  EXPECT_EQ(tcr.sh1(), 0b10u);
  EXPECT_TRUE(tcr.asid_16bit());
  EXPECT_TRUE(tcr.asid_from_ttbr1());
  EXPECT_TRUE(tcr.tbi0());
  EXPECT_FALSE(tcr.tbi1());
  EXPECT_TRUE(tcr.hw_access_flag());
  EXPECT_TRUE(tcr.hw_dirty_state());
  EXPECT_FALSE(tcr.epd0());
  EXPECT_TRUE(tcr.epd1());
}

TEST_F(MmuRegsTest, Arm64TtbrRoundTripsBaseAddrAndAsid) {
  using namespace structo::arch::arm64;
  ttbr0_el1 t0{};
  t0.set_base_addr(0x1234'5000ull).set_asid(0xbeefu).set_common_not_private(true);
  EXPECT_EQ(t0.base_addr(), 0x1234'5000ull);
  EXPECT_EQ(t0.asid(), 0xbeefu);
  EXPECT_TRUE(t0.common_not_private());

  ttbr1_el1 t1{};
  t1.set_base_addr(0x7f00'0000'1000ull).set_asid(7);
  EXPECT_EQ(t1.base_addr(), 0x7f00'0000'1000ull);
  EXPECT_EQ(t1.asid(), 7u);
}

TEST_F(MmuRegsTest, Arm64MairIndependentlyAddressesEachAttrByte) {
  using namespace structo::arch::arm64;
  mair_el1 mair{};
  mair.set_attr(0, 0xff).set_attr(1, 0x04).set_attr(7, 0xaa);
  EXPECT_EQ(mair.attr(0), 0xffu);
  EXPECT_EQ(mair.attr(1), 0x04u);
  EXPECT_EQ(mair.attr(7), 0xaau);
  EXPECT_EQ(mair.attr(2), 0u); // untouched
}

TEST_F(MmuRegsTest, Arm64ScrEl3RoundTripsTrustZoneWorldSwitchAndRoutingBits) {
  using namespace structo::arch::arm64;
  scr_el3 s{};
  s.set_ns(true)
      .set_irq_to_el3(true)
      .set_fiq_to_el3(false)
      .set_external_abort_to_el3(true)
      .set_secure_monitor_call_disabled(false)
      .set_hyp_call_enabled(true)
      .set_secure_instruction_fetch(false)
      .set_rw(true)
      .set_secure_el1_timer_access(true)
      .set_trap_wfi_to_el3(false)
      .set_trap_wfe_to_el3(true);
  EXPECT_TRUE(s.ns());
  EXPECT_TRUE(s.irq_to_el3());
  EXPECT_FALSE(s.fiq_to_el3());
  EXPECT_TRUE(s.external_abort_to_el3());
  EXPECT_FALSE(s.secure_monitor_call_disabled());
  EXPECT_TRUE(s.hyp_call_enabled());
  EXPECT_FALSE(s.secure_instruction_fetch());
  EXPECT_TRUE(s.rw());
  EXPECT_TRUE(s.secure_el1_timer_access());
  EXPECT_FALSE(s.trap_wfi_to_el3());
  EXPECT_TRUE(s.trap_wfe_to_el3());
}

TEST_F(MmuRegsTest, Arm64HcrEl2RoundTripsStage2AndTrapControlBits) {
  using namespace structo::arch::arm64;
  hcr_el2 h{};
  h.set_vm(true)
      .set_set_way_invalidation_override(true)
      .set_protected_table_walk(false)
      .set_fiq_to_el2(true)
      .set_irq_to_el2(true)
      .set_serror_to_el2(false)
      .set_default_cacheability(true)
      .set_barrier_shareability_upgrade(0b10)
      .set_trap_wfi(true)
      .set_trap_wfe(false)
      .set_trap_smc(true)
      .set_trap_tlb_maintenance(false)
      .set_tvm(true)
      .set_trap_general_exceptions(false)
      .set_trap_dc_zva(true)
      .set_hyp_call_disabled(false)
      .set_trap_vm_reads(true)
      .set_rw(true)
      .set_cacheability_disabled(true)
      .set_instruction_cacheability_disabled(true)
      .set_e2h(true);
  EXPECT_TRUE(h.vm());
  EXPECT_TRUE(h.set_way_invalidation_override());
  EXPECT_FALSE(h.protected_table_walk());
  EXPECT_TRUE(h.fiq_to_el2());
  EXPECT_TRUE(h.irq_to_el2());
  EXPECT_FALSE(h.serror_to_el2());
  EXPECT_TRUE(h.default_cacheability());
  EXPECT_EQ(h.barrier_shareability_upgrade(), 0b10u);
  EXPECT_TRUE(h.trap_wfi());
  EXPECT_FALSE(h.trap_wfe());
  EXPECT_TRUE(h.trap_smc());
  EXPECT_FALSE(h.trap_tlb_maintenance());
  EXPECT_TRUE(h.tvm());
  EXPECT_FALSE(h.trap_general_exceptions());
  EXPECT_TRUE(h.trap_dc_zva());
  EXPECT_FALSE(h.hyp_call_disabled());
  EXPECT_TRUE(h.trap_vm_reads());
  EXPECT_TRUE(h.rw());
  EXPECT_TRUE(h.cacheability_disabled());
  EXPECT_TRUE(h.instruction_cacheability_disabled());
  EXPECT_TRUE(h.e2h());
}

TEST_F(MmuRegsTest, Arm64VtcrEl2RoundTripsEveryNamedField) {
  using namespace structo::arch::arm64;
  vtcr_el2 t{};
  t.set_t0sz(24)
      .set_sl0(0b01)
      .set_irgn0(0b11)
      .set_orgn0(0b10)
      .set_sh0(0b01)
      .set_tg0(0b00)
      .set_ps(0b010)
      .set_vmid_16bit(true)
      .set_hw_access_flag(true)
      .set_hw_dirty_state(false);
  EXPECT_EQ(t.t0sz(), 24u);
  EXPECT_EQ(t.sl0(), 0b01u);
  EXPECT_EQ(t.irgn0(), 0b11u);
  EXPECT_EQ(t.orgn0(), 0b10u);
  EXPECT_EQ(t.sh0(), 0b01u);
  EXPECT_EQ(t.tg0(), 0b00u);
  EXPECT_EQ(t.ps(), 0b010u);
  EXPECT_TRUE(t.vmid_16bit());
  EXPECT_TRUE(t.hw_access_flag());
  EXPECT_FALSE(t.hw_dirty_state());
}

TEST_F(MmuRegsTest, Arm64VttbrEl2RoundTripsBaseAddrAndVmid) {
  using namespace structo::arch::arm64;
  vttbr_el2 v{};
  v.set_base_addr(0x1'2345'6000ull).set_vmid(0xbeefu).set_common_not_private(true);
  EXPECT_EQ(v.base_addr(), 0x1'2345'6000ull);
  EXPECT_EQ(v.vmid(), 0xbeefu);
  EXPECT_TRUE(v.common_not_private());
}

// --- ARM32 ------------------------------------------------------------

TEST_F(MmuRegsTest, Arm32SctlrRoundTripsNamedBits) {
  using namespace structo::arch::arm;
  sctlr s{};
  s.set_mmu_enabled(true).set_dcache_enabled(true).set_icache_enabled(true).set_alignment_check_enabled(true);
  EXPECT_TRUE(s.mmu_enabled());
  EXPECT_TRUE(s.dcache_enabled());
  EXPECT_TRUE(s.icache_enabled());
  EXPECT_TRUE(s.alignment_check_enabled());
}

TEST_F(MmuRegsTest, Arm32TtbcrShortExposesNAndPdBitsAndEae) {
  using namespace structo::arch::arm;
  ttbcr_short t{};
  t.set_n(0b101).set_pd0(true).set_pd1(false).set_eae(false);
  EXPECT_EQ(t.n(), 0b101u);
  EXPECT_TRUE(t.pd0());
  EXPECT_FALSE(t.pd1());
  EXPECT_FALSE(t.eae());
}

TEST_F(MmuRegsTest, Arm32TtbcrLpaeRoundTripsEveryNamedField) {
  using namespace structo::arch::arm;
  ttbcr_lpae t{};
  t.set_t0sz(0b011)
      .set_t1sz(0b010)
      .set_irgn0(0b01)
      .set_orgn0(0b01)
      .set_sh0(0b11)
      .set_irgn1(0b10)
      .set_orgn1(0b10)
      .set_sh1(0b10)
      .set_asid_from_ttbr1(true)
      .set_epd0(false)
      .set_epd1(true)
      .set_eae(true);

  EXPECT_EQ(t.t0sz(), 0b011u);
  EXPECT_EQ(t.t1sz(), 0b010u);
  EXPECT_EQ(t.irgn0(), 0b01u);
  EXPECT_EQ(t.orgn0(), 0b01u);
  EXPECT_EQ(t.sh0(), 0b11u);
  EXPECT_EQ(t.irgn1(), 0b10u);
  EXPECT_EQ(t.orgn1(), 0b10u);
  EXPECT_EQ(t.sh1(), 0b10u);
  EXPECT_TRUE(t.asid_from_ttbr1());
  EXPECT_FALSE(t.epd0());
  EXPECT_TRUE(t.epd1());
  EXPECT_TRUE(t.eae());
}

TEST_F(MmuRegsTest, Arm32TtbrShortRoundTripsBaseAddr) {
  using namespace structo::arch::arm;
  ttbr0_short t0{};
  t0.set_base_addr(0x8000'0000u);
  EXPECT_EQ(t0.base_addr(), 0x8000'0000u);

  ttbr1_short t1{};
  t1.set_base_addr(0xc000'4000u);
  EXPECT_EQ(t1.base_addr(), 0xc000'4000u);
}

TEST_F(MmuRegsTest, Arm32TtbrLpaeRoundTripsBaseAddrAndAsid) {
  using namespace structo::arch::arm;
  ttbr0_lpae t0{};
  t0.set_base_addr(0x1'2345'6000ull).set_asid(0x42);
  EXPECT_EQ(t0.base_addr(), 0x1'2345'6000ull);
  EXPECT_EQ(t0.asid(), 0x42u);
}

TEST_F(MmuRegsTest, Arm32ContextidrRoundTripsAsidAndProcid) {
  using namespace structo::arch::arm;
  contextidr c{};
  c.set_asid(0x7a).set_procid(0x00abcdef);
  EXPECT_EQ(c.asid(), 0x7au);
  EXPECT_EQ(c.procid(), 0x00abcdefu);
}

TEST_F(MmuRegsTest, Arm32ScrRoundTripsTrustZoneWorldSwitchAndRoutingBits) {
  using namespace structo::arch::arm;
  scr s{};
  s.set_ns(true)
      .set_irq_to_monitor(true)
      .set_fiq_to_monitor(false)
      .set_external_abort_to_monitor(true)
      .set_fiq_mask_writable_from_ns(false)
      .set_abort_mask_writable_from_ns(true)
      .set_secure_monitor_call_disabled(false)
      .set_hyp_call_enabled(true)
      .set_secure_instruction_fetch(true);

  EXPECT_TRUE(s.ns());
  EXPECT_TRUE(s.irq_to_monitor());
  EXPECT_FALSE(s.fiq_to_monitor());
  EXPECT_TRUE(s.external_abort_to_monitor());
  EXPECT_FALSE(s.fiq_mask_writable_from_ns());
  EXPECT_TRUE(s.abort_mask_writable_from_ns());
  EXPECT_FALSE(s.secure_monitor_call_disabled());
  EXPECT_TRUE(s.hyp_call_enabled());
  EXPECT_TRUE(s.secure_instruction_fetch());

  auto reparsed = scr::from_raw(s.raw);
  EXPECT_TRUE(reparsed.ns());
  EXPECT_TRUE(reparsed.hyp_call_enabled());
}

TEST_F(MmuRegsTest, Arm32NsacrGrantsAndRevokesPerCoprocessorNonSecureAccess) {
  using namespace structo::arch::arm;
  nsacr n{};
  n.set_cp10_accessible(true).set_cp11_accessible(true).set_cp_accessible(2, true);
  EXPECT_TRUE(n.cp10_accessible());
  EXPECT_TRUE(n.cp11_accessible());
  EXPECT_TRUE(n.cp_accessible(2));
  EXPECT_FALSE(n.cp_accessible(3));

  n.set_cp10_accessible(false);
  EXPECT_FALSE(n.cp10_accessible());
  EXPECT_TRUE(n.cp11_accessible()); // untouched by revoking cp10 alone

  n.set_nsd32_disabled(true).set_nsase_disabled(true);
  EXPECT_TRUE(n.nsd32_disabled());
  EXPECT_TRUE(n.nsase_disabled());
}

TEST_F(MmuRegsTest, Arm32HcrRoundTripsStage2AndTrapControlBits) {
  using namespace structo::arch::arm;
  hcr h{};
  h.set_vm(true)
      .set_set_way_invalidation_override(true)
      .set_protected_table_walk(false)
      .set_fiq_to_hyp(true)
      .set_irq_to_hyp(true)
      .set_external_abort_to_hyp(false)
      .set_default_cacheability(true)
      .set_barrier_shareability_upgrade(0b11)
      .set_trap_wfi(true)
      .set_trap_wfe(false)
      .set_trap_smc(true)
      .set_trap_tlb_maintenance(false)
      .set_tvm(true)
      .set_trap_general_exceptions(false)
      .set_trap_dc_zva(true)
      .set_hyp_call_disabled(false)
      .set_trap_vm_reads(true);
  EXPECT_TRUE(h.vm());
  EXPECT_TRUE(h.set_way_invalidation_override());
  EXPECT_FALSE(h.protected_table_walk());
  EXPECT_TRUE(h.fiq_to_hyp());
  EXPECT_TRUE(h.irq_to_hyp());
  EXPECT_FALSE(h.external_abort_to_hyp());
  EXPECT_TRUE(h.default_cacheability());
  EXPECT_EQ(h.barrier_shareability_upgrade(), 0b11u);
  EXPECT_TRUE(h.trap_wfi());
  EXPECT_FALSE(h.trap_wfe());
  EXPECT_TRUE(h.trap_smc());
  EXPECT_FALSE(h.trap_tlb_maintenance());
  EXPECT_TRUE(h.tvm());
  EXPECT_FALSE(h.trap_general_exceptions());
  EXPECT_TRUE(h.trap_dc_zva());
  EXPECT_FALSE(h.hyp_call_disabled());
  EXPECT_TRUE(h.trap_vm_reads());
}

TEST_F(MmuRegsTest, Arm32VtcrRoundTripsEveryNamedField) {
  using namespace structo::arch::arm;
  vtcr t{};
  t.set_t0sz(8).set_sl0(0b01).set_irgn0(0b11).set_orgn0(0b10).set_sh0(0b01);
  EXPECT_EQ(t.t0sz(), 8u);
  EXPECT_EQ(t.sl0(), 0b01u);
  EXPECT_EQ(t.irgn0(), 0b11u);
  EXPECT_EQ(t.orgn0(), 0b10u);
  EXPECT_EQ(t.sh0(), 0b01u);
}

TEST_F(MmuRegsTest, Arm32VttbrRoundTripsBaseAddrAndVmid) {
  using namespace structo::arch::arm;
  vttbr v{};
  v.set_base_addr(0x1234'5000ull).set_vmid(0x42u);
  EXPECT_EQ(v.base_addr(), 0x1234'5000ull);
  EXPECT_EQ(v.vmid(), 0x42u);
}

// --- RISC-V -----------------------------------------------------------

TEST_F(MmuRegsTest, RiscvSatpRoundTripsModeAsidAndPpn) {
  using namespace structo::arch::riscv;
  satp s{};
  s.set_mode(satp_mode::sv39).set_asid(0x1234).set_ppn(0x0000'0012'3456ull);
  EXPECT_EQ(s.mode(), satp_mode::sv39);
  EXPECT_EQ(s.asid(), 0x1234u);
  EXPECT_EQ(s.ppn(), 0x0000'0012'3456ull);

  s.set_mode(satp_mode::sv57);
  EXPECT_EQ(s.mode(), satp_mode::sv57);
  // other fields unaffected by changing MODE alone
  EXPECT_EQ(s.asid(), 0x1234u);
  EXPECT_EQ(s.ppn(), 0x0000'0012'3456ull);
}

TEST_F(MmuRegsTest, RiscvSatpBareModeIsZero) {
  using namespace structo::arch::riscv;
  EXPECT_EQ(static_cast<unsigned>(satp_mode::bare), 0u);
  satp s{};
  EXPECT_EQ(s.mode(), satp_mode::bare);
}

// --- x86 ----------------------------------------------------------------

TEST_F(MmuRegsTest, X86Cr0RoundTripsNamedBits) {
  using namespace structo::arch::x86;
  cr0 c{};
  c.set_protected_mode_enabled(true).set_write_protect(true).set_cache_disabled(false).set_paging_enabled(true);
  EXPECT_TRUE(c.protected_mode_enabled());
  EXPECT_TRUE(c.write_protect());
  EXPECT_FALSE(c.cache_disabled());
  EXPECT_TRUE(c.paging_enabled());
}

TEST_F(MmuRegsTest, X86Cr3RoundTripsBaseAddrAndPcid) {
  using namespace structo::arch::x86;
  cr3 c{};
  c.set_base_addr(0x12'3456'7000ull).set_pcid(0x0ab);
  EXPECT_EQ(c.base_addr(), 0x12'3456'7000ull);
  EXPECT_EQ(c.pcid(), 0x0abu);

  cr3 c2{};
  c2.set_page_write_through(true).set_page_cache_disabled(true);
  EXPECT_TRUE(c2.page_write_through());
  EXPECT_TRUE(c2.page_cache_disabled());
}

TEST_F(MmuRegsTest, X86Cr4RoundTripsNamedBits) {
  using namespace structo::arch::x86;
  cr4 c{};
  c.set_pae(true).set_page_global_enabled(true).set_pcide(true).set_smep(true).set_smap(true).set_la57(true);
  EXPECT_TRUE(c.pae());
  EXPECT_TRUE(c.page_global_enabled());
  EXPECT_TRUE(c.pcide());
  EXPECT_TRUE(c.smep());
  EXPECT_TRUE(c.smap());
  EXPECT_TRUE(c.la57());
}

TEST_F(MmuRegsTest, X86EferRoundTripsWritableBitsAndExposesReadOnlyLma) {
  using namespace structo::arch::x86;
  efer e{};
  e.set_syscall_enabled(true).set_long_mode_enabled(true).set_no_execute_enabled(true);
  EXPECT_TRUE(e.syscall_enabled());
  EXPECT_TRUE(e.long_mode_enabled());
  EXPECT_TRUE(e.no_execute_enabled());
  EXPECT_FALSE(e.long_mode_active()); // LMA is a read-only status bit; never set by the builder

  auto with_lma = efer::from_raw(e.raw | (std::uint64_t{1} << 10));
  EXPECT_TRUE(with_lma.long_mode_active());
}
