// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/mii_phy.hpp>

#include <reloco/array.hpp>

using namespace structo::hw;

namespace {

// A simulated Clause 22 gigabit PHY at one address, with a link partner behind it.
struct fake_mdio {
  unsigned phy_addr = 5;
  reloco::array<std::uint16_t, 32> regs{};
  unsigned reset_polls = 2; // BMCR reset stays set for this many reads
  unsigned reset_left = 0;
  bool never_resets = false;
  bool cable = false; // link partner present
  std::uint16_t lp_anar = mii::an_selector_8023 | mii::an_10hd | mii::an_10fd | mii::an_100hd | mii::an_100fd;
  std::uint16_t lp_gbsr = 0;
  bool bmsr_latched_down = false;
  unsigned bmsr_reads = 0;
  unsigned bus_ops = 0;

  fake_mdio() {
    regs[mii::reg_id1] = 0x0022;
    regs[mii::reg_id2] = 0x1623; // model 0x162_, revision 3
    regs[mii::reg_bmsr] = mii::bmsr_extended_caps | mii::bmsr_an_capable | mii::bmsr_extended_status | mii::bmsr_10hd |
                          mii::bmsr_10fd | mii::bmsr_100hd | mii::bmsr_100fd;
    regs[mii::reg_estatus] = mii::estatus_1000fd | mii::estatus_1000hd;
    regs[mii::reg_bmcr] = mii::bmcr_an_enable;
  }

  bool an_done() const {
    return cable && (regs[mii::reg_bmcr] & mii::bmcr_an_enable) && !(regs[mii::reg_bmcr] & mii::bmcr_restart_an);
  }
};

} // namespace

template <> struct structo::hw::mdio_traits<fake_mdio> {
  static reloco::result<std::uint16_t> read(fake_mdio &m, unsigned phy, unsigned reg) noexcept {
    ++m.bus_ops;
    if (phy != m.phy_addr || reg > 31)
      return std::uint16_t{0xFFFF};
    if (reg == mii::reg_bmcr && m.reset_left > 0) {
      if (!m.never_resets && --m.reset_left == 0)
        m.regs[reg] = static_cast<std::uint16_t>(m.regs[reg] & ~mii::bmcr_reset);
      return m.regs[reg];
    }
    if (reg == mii::reg_bmsr) {
      ++m.bmsr_reads;
      std::uint16_t v = m.regs[reg];
      const bool latched = m.bmsr_latched_down;
      m.bmsr_latched_down = false;
      if (m.cable && !latched)
        v = static_cast<std::uint16_t>(v | mii::bmsr_link);
      if (m.an_done())
        v = static_cast<std::uint16_t>(v | mii::bmsr_an_complete);
      return v;
    }
    if (reg == mii::reg_anlpar)
      return m.lp_anar;
    if (reg == mii::reg_gbsr)
      return m.lp_gbsr;
    return m.regs[reg];
  }
  static reloco::result<void> write(fake_mdio &m, unsigned phy, unsigned reg, std::uint16_t v) noexcept {
    ++m.bus_ops;
    if (phy != m.phy_addr || reg > 31)
      return {};
    if (reg == mii::reg_bmcr && (v & mii::bmcr_reset)) {
      m.reset_left = m.reset_polls;
    }
    m.regs[reg] = v;
    return {};
  }
};

TEST(MiiPhy, UnboundBusFails) {
  mdio_bus_ref bus;
  EXPECT_FALSE(bus);
  EXPECT_EQ(bus.read(0, 0).error(), reloco::error::unsupported_operation);
  EXPECT_EQ(bus.write(0, 0, 0).error(), reloco::error::unsupported_operation);
}

TEST(MiiPhy, ProbeScansAndReadsId) {
  fake_mdio m;
  mii_phy phy{mdio_bus_ref{m}};
  ASSERT_TRUE(phy.probe().has_value());
  EXPECT_EQ(phy.address(), 5u);
  EXPECT_EQ(phy.id(), 0x00221623u);
  EXPECT_TRUE(phy.gigabit_capable());

  mii_phy fixed{mdio_bus_ref{m}, 9};
  EXPECT_EQ(fixed.probe().error(), reloco::error::not_found);
  mii_phy bad{mdio_bus_ref{m}, 40};
  EXPECT_EQ(bad.probe().error(), reloco::error::invalid_argument);

  fake_mdio empty;
  empty.phy_addr = 99; // nobody answers
  mii_phy none{mdio_bus_ref{empty}};
  EXPECT_EQ(none.probe().error(), reloco::error::not_found);
}

TEST(MiiPhy, ResetWaitsForSelfClearingBitAndRunsQuirk) {
  fake_mdio m;
  mii_phy phy{mdio_bus_ref{m}};
  ASSERT_TRUE(phy.probe().has_value());

  static int post_reset_calls = 0;
  post_reset_calls = 0;
  mii_quirks q;
  q.post_reset = [](void *, const mdio_bus_ref &, unsigned) noexcept -> reloco::result<void> {
    ++post_reset_calls;
    return {};
  };
  phy.set_quirks(q);
  ASSERT_TRUE(phy.reset().has_value());
  EXPECT_EQ(post_reset_calls, 1);

  m.never_resets = true;
  EXPECT_EQ(phy.reset(10).error(), reloco::error::timed_out);
  EXPECT_EQ(post_reset_calls, 1);
}

TEST(MiiPhy, AutonegotiationResolvesBestCommonMode) {
  fake_mdio m;
  mii_phy phy{mdio_bus_ref{m}};
  ASSERT_TRUE(phy.probe().has_value());
  ASSERT_TRUE(phy.start_autoneg().has_value());
  EXPECT_EQ(m.regs[mii::reg_anar] & mii::an_all_speeds, mii::an_all_speeds);
  EXPECT_EQ(m.regs[mii::reg_gbcr], mii::gbcr_1000fd | mii::gbcr_1000hd);

  auto l = phy.poll(); // no cable
  ASSERT_TRUE(l.has_value());
  EXPECT_FALSE(l->up);
  EXPECT_FALSE(l->changed);

  m.cable = true;
  m.regs[mii::reg_bmcr] = static_cast<std::uint16_t>(m.regs[mii::reg_bmcr] & ~mii::bmcr_restart_an);
  l = phy.poll(); // partner: 10/100 only
  ASSERT_TRUE(l.has_value());
  EXPECT_TRUE(l->up);
  EXPECT_TRUE(l->changed);
  EXPECT_EQ(l->speed, mii_speed::mbps100);
  EXPECT_TRUE(l->full_duplex);

  l = phy.poll();
  EXPECT_FALSE(l->changed);

  m.lp_gbsr = mii::gbsr_lp_1000fd;
  l = phy.poll();
  EXPECT_EQ(l->speed, mii_speed::mbps1000);
  EXPECT_TRUE(l->changed);

  // Restricting the advertisement to 10 Mb/s half duplex only.
  ASSERT_TRUE(phy.start_autoneg(mii::an_10hd, 0).has_value());
  m.regs[mii::reg_bmcr] = static_cast<std::uint16_t>(m.regs[mii::reg_bmcr] & ~mii::bmcr_restart_an);
  l = phy.poll();
  EXPECT_EQ(l->speed, mii_speed::mbps10);
  EXPECT_FALSE(l->full_duplex);
}

TEST(MiiPhy, LatchedLinkDownIsDebounced) {
  fake_mdio m;
  mii_phy phy{mdio_bus_ref{m}};
  ASSERT_TRUE(phy.probe().has_value());
  ASSERT_TRUE(phy.force(mii_speed::mbps100, true).has_value());
  m.cable = true;
  const unsigned before = m.bmsr_reads;
  m.bmsr_latched_down = true; // a brief drop since the last read: the first BMSR read reports down
  auto l = phy.poll();
  ASSERT_TRUE(l.has_value());
  EXPECT_TRUE(l->up);
  EXPECT_EQ(m.bmsr_reads - before, 2u);
}

TEST(MiiPhy, ForcedModeReportsBmcr) {
  fake_mdio m;
  mii_phy phy{mdio_bus_ref{m}};
  ASSERT_TRUE(phy.probe().has_value());
  EXPECT_EQ(phy.force(mii_speed::none, true).error(), reloco::error::invalid_argument);
  ASSERT_TRUE(phy.force(mii_speed::mbps1000, false).has_value());
  m.cable = true;
  auto l = phy.poll();
  ASSERT_TRUE(l.has_value());
  EXPECT_TRUE(l->up);
  EXPECT_EQ(l->speed, mii_speed::mbps1000);
  EXPECT_FALSE(l->full_duplex);
}

TEST(MiiPhy, StatusQuirkCanOverride) {
  fake_mdio m;
  mii_phy phy{mdio_bus_ref{m}};
  ASSERT_TRUE(phy.probe().has_value());
  mii_quirks q;
  q.read_status = [](void *, const mdio_bus_ref &, unsigned, const mii_link &g) noexcept -> reloco::result<mii_link> {
    mii_link l = g;
    l.up = true;
    l.speed = mii_speed::mbps10;
    return l;
  };
  phy.set_quirks(q);
  auto l = phy.poll();
  ASSERT_TRUE(l.has_value());
  EXPECT_TRUE(l->up);
  EXPECT_TRUE(l->changed);
}

TEST(MiiPhy, NoCommonModeMeansNoLink) {
  fake_mdio m;
  mii_phy phy{mdio_bus_ref{m}};
  ASSERT_TRUE(phy.probe().has_value());
  ASSERT_TRUE(phy.start_autoneg(mii::an_10hd, 0).has_value());
  m.regs[mii::reg_bmcr] = static_cast<std::uint16_t>(m.regs[mii::reg_bmcr] & ~mii::bmcr_restart_an);
  m.cable = true;
  m.lp_anar = mii::an_selector_8023 | mii::an_100fd;
  auto l = phy.poll();
  ASSERT_TRUE(l.has_value());
  EXPECT_FALSE(l->up);
}

TEST(MiiPhy, MmdIndirectAccessSequence) {
  fake_mdio m;
  mii_phy phy{mdio_bus_ref{m}};
  ASSERT_TRUE(phy.probe().has_value());
  EXPECT_EQ(phy.write_mmd(32, 0, 0).error(), reloco::error::invalid_argument);
  ASSERT_TRUE(phy.write_mmd(7, 0x3C, 0x1234).has_value());
  EXPECT_EQ(m.regs[mii::reg_mmdctl], 0x4007); // data function for device 7
}
