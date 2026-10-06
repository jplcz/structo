// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/vm_time_manager.hpp>

#include <reloco/span.hpp>

using structo::hw::cycles;
using structo::hw::time_source_capabilities;
using structo::hw::time_source_ref;
using structo::hw::vm_time_manager;
using reloco::error;
using reloco::result;
using reloco::unexpected;

namespace {

// A fully scriptable host counter backend, mirroring test_time_source_ref.cpp's fake_counter.
struct fake_host_counter {
  result<cycles> next_value = cycles{0};
  time_source_capabilities caps{};
};

} // namespace

template <> struct structo::hw::time_source_traits<fake_host_counter> {
  static result<cycles> try_now(fake_host_counter &b) noexcept { return b.next_value; }
  static time_source_capabilities capabilities(fake_host_counter &b) noexcept { return b.caps; }
};

namespace {

// A fake per-vCPU offset register, directly storing the `guest = host + offset` convention this header
// documents -- i.e. this fake is architecture-neutral (no ARM-style sign flip), mirroring the x86/RISC-V
// examples' native convention. test_arm_sign_flip below separately exercises an ARM-shaped backend.
struct fake_vcpu_register {
  cycles offset{};
  result<cycles> next_read_offset = cycles{};
  bool read_offset_tracks_apply = true; // if true, try_read_offset ignores next_read_offset and echoes `offset`
};

struct fake_vm_timer_traits {
  using vcpu_handle = fake_vcpu_register *;

  static void apply_offset(vcpu_handle vcpu, cycles offset) noexcept {
    vcpu->offset = offset;
    if (vcpu->read_offset_tracks_apply)
      vcpu->next_read_offset = offset;
  }

  static result<cycles> try_read_offset(vcpu_handle vcpu) noexcept { return vcpu->next_read_offset; }
};

class VmTimeManagerTest : public ::testing::Test {
protected:
  fake_host_counter host_;
  fake_vcpu_register vcpu_;
  time_source_ref host_ref_{host_};
  vm_time_manager<fake_vm_timer_traits> mgr_{host_ref_};
};

} // namespace

TEST_F(VmTimeManagerTest, RebindProgramsOffsetSoGuestReadsTarget) {
  host_.next_value = cycles{1'000};
  ASSERT_TRUE(mgr_.rebind(&vcpu_, cycles{1'500}).has_value());
  // guest = host + offset => offset = 1'500 - 1'000 = 500.
  EXPECT_EQ(vcpu_.offset, cycles{500});
}

TEST_F(VmTimeManagerTest, RebindOffsetWrapsRatherThanUnderflowingWhenTargetIsBelowHost) {
  host_.next_value = cycles{1'000};
  ASSERT_TRUE(mgr_.rebind(&vcpu_, cycles{100}).has_value());
  // 100 - 1'000, modulo 2^64 -- the same wraparound arithmetic the real register performs.
  EXPECT_EQ(vcpu_.offset, cycles{100}.wrapping_sub(cycles{1'000}));
}

TEST_F(VmTimeManagerTest, ResetBindsGuestCounterToZero) {
  host_.next_value = cycles{42'000};
  ASSERT_TRUE(mgr_.reset(&vcpu_).has_value());
  EXPECT_EQ(vcpu_.offset, cycles{0}.wrapping_sub(cycles{42'000}));

  auto guest_now = mgr_.try_guest_now(&vcpu_);
  ASSERT_TRUE(guest_now.has_value());
  EXPECT_EQ(guest_now.value(), cycles{0});
}

TEST_F(VmTimeManagerTest, TryGuestNowReflectsHostAdvancingAfterRebind) {
  host_.next_value = cycles{1'000};
  ASSERT_TRUE(mgr_.rebind(&vcpu_, cycles{1'500}).has_value());

  host_.next_value = cycles{1'200}; // host advanced 200 cycles since rebind
  auto guest_now = mgr_.try_guest_now(&vcpu_);
  ASSERT_TRUE(guest_now.has_value());
  EXPECT_EQ(guest_now.value(), cycles{1'700}); // guest advances by the same 200 cycles
}

TEST_F(VmTimeManagerTest, RebindFailsWhenHostCounterReadFails) {
  host_.next_value = unexpected(error::unsupported_operation);
  auto rebound = mgr_.rebind(&vcpu_, cycles{0});
  ASSERT_FALSE(rebound.has_value());
  EXPECT_EQ(rebound.error(), error::unsupported_operation);
}

TEST_F(VmTimeManagerTest, TryGuestNowFailsWhenHostCounterReadFails) {
  host_.next_value = unexpected(error::unsupported_operation);
  auto guest_now = mgr_.try_guest_now(&vcpu_);
  ASSERT_FALSE(guest_now.has_value());
  EXPECT_EQ(guest_now.error(), error::unsupported_operation);
}

TEST_F(VmTimeManagerTest, TryGuestNowFailsWhenOffsetReadBackFails) {
  host_.next_value = cycles{1'000};
  vcpu_.next_read_offset = unexpected(error::io_error);
  auto guest_now = mgr_.try_guest_now(&vcpu_);
  ASSERT_FALSE(guest_now.has_value());
  EXPECT_EQ(guest_now.error(), error::io_error);
}

TEST_F(VmTimeManagerTest, HostCounterAccessorReturnsTheBoundRef) {
  host_.caps.clock_hz = 24'000'000ULL;
  auto caps = mgr_.host_counter().capabilities();
  ASSERT_TRUE(caps.has_value());
  EXPECT_EQ(caps.value().clock_hz, 24'000'000ULL);
}

TEST_F(VmTimeManagerTest, PauseResumeRoundTripPreservesApparentGuestTime) {
  // vCPU starts running from cycles{0}.
  host_.next_value = cycles{5'000};
  ASSERT_TRUE(mgr_.reset(&vcpu_).has_value());

  // Guest runs for a while; host advances 300 cycles before the vCPU is descheduled.
  host_.next_value = cycles{5'300};
  auto snapshot = mgr_.try_guest_now(&vcpu_);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot.value(), cycles{300});

  // A long pause elapses on the host (e.g. the vCPU thread was descheduled, or this is actually a different
  // host after live migration) -- host counter jumps far ahead, but the guest must not observe that gap.
  host_.next_value = cycles{999'000};
  ASSERT_TRUE(mgr_.rebind(&vcpu_, snapshot.value()).has_value());

  auto guest_now = mgr_.try_guest_now(&vcpu_);
  ASSERT_TRUE(guest_now.has_value());
  EXPECT_EQ(guest_now.value(), cycles{300}); // still 300: no time appeared to pass during the pause
}

TEST_F(VmTimeManagerTest, OneSharedManagerKeepsMultipleVcpusMutuallyConsistent) {
  // Two vCPUs of the same VM, each with its own offset register, both bound through the one shared
  // vm_time_manager (and therefore the one shared host counter) -- see the @file-level docs' multi-vCPU
  // section. Resetting them back-to-back at the same host reading must leave both reading the same
  // guest-visible value, exactly as a guest SMP kernel assumes of its own cores.
  fake_vcpu_register vcpu_b;
  host_.next_value = cycles{7'000};
  ASSERT_TRUE(mgr_.reset(&vcpu_).has_value());
  ASSERT_TRUE(mgr_.reset(&vcpu_b).has_value());

  auto now_a = mgr_.try_guest_now(&vcpu_);
  auto now_b = mgr_.try_guest_now(&vcpu_b);
  ASSERT_TRUE(now_a.has_value());
  ASSERT_TRUE(now_b.has_value());
  EXPECT_EQ(now_a.value(), now_b.value());

  // Host advances; both vCPUs must observe the same elapsed time, since both offsets were computed from
  // the same host counter.
  host_.next_value = cycles{7'500};
  now_a = mgr_.try_guest_now(&vcpu_);
  now_b = mgr_.try_guest_now(&vcpu_b);
  ASSERT_TRUE(now_a.has_value());
  ASSERT_TRUE(now_b.has_value());
  EXPECT_EQ(now_a.value(), cycles{500});
  EXPECT_EQ(now_b.value(), cycles{500});
}

namespace {

// A minimal forward-only, intrusive singly-linked list of vCPU handles -- proves pause_all/resume_all only
// ever need begin()/end()/size() on the vcpus range, never random access/operator[], matching how a real
// hypervisor's vCPU bookkeeping is more likely to be structured (see the @file-level docs' VCPUs tparam).
class intrusive_vcpu_list {
public:
  struct node {
    fake_vcpu_register *vcpu;
    node *next = nullptr;
  };

  class iterator {
  public:
    explicit iterator(node *n) noexcept : n_(n) {}
    fake_vcpu_register *operator*() const noexcept { return n_->vcpu; }
    iterator &operator++() noexcept {
      n_ = n_->next;
      return *this;
    }
    bool operator!=(const iterator &other) const noexcept { return n_ != other.n_; }

  private:
    node *n_;
  };

  void push_front(node &n) noexcept {
    n.next = head_;
    head_ = &n;
    ++count_;
  }

  iterator begin() const noexcept { return iterator(head_); }
  iterator end() const noexcept { return iterator(nullptr); }
  std::size_t size() const noexcept { return count_; }

private:
  node *head_ = nullptr;
  std::size_t count_ = 0;
};

} // namespace

TEST_F(VmTimeManagerTest, PauseAllThenResumeAllPreservesApparentTimeAcrossAllVcpus) {
  fake_vcpu_register vcpu_b;
  intrusive_vcpu_list::node node_a{&vcpu_}, node_b{&vcpu_b};
  intrusive_vcpu_list vcpus;
  vcpus.push_front(node_b);
  vcpus.push_front(node_a); // list order: vcpu_, vcpu_b

  host_.next_value = cycles{1'000};
  ASSERT_TRUE(mgr_.reset(&vcpu_).has_value());
  host_.next_value = cycles{1'100};
  ASSERT_TRUE(mgr_.reset(&vcpu_b).has_value());

  host_.next_value = cycles{1'400}; // both vCPUs have been running for a while
  cycles snapshots[2];
  ASSERT_TRUE(mgr_.pause_all(vcpus, reloco::span<cycles>(snapshots, 2)).has_value());
  EXPECT_EQ(snapshots[0], cycles{400}); // vcpu_: reset at 1'000, now 1'400 => guest reads 400
  EXPECT_EQ(snapshots[1], cycles{300}); // vcpu_b: reset at 1'100, now 1'400 => guest reads 300

  // A long pause elapses on the host (e.g. the whole VM was checkpointed) -- neither vCPU should observe it.
  host_.next_value = cycles{999'000};
  ASSERT_TRUE(mgr_.resume_all(vcpus, reloco::span<const cycles>(snapshots, 2)).has_value());

  auto now_a = mgr_.try_guest_now(&vcpu_);
  auto now_b = mgr_.try_guest_now(&vcpu_b);
  ASSERT_TRUE(now_a.has_value());
  ASSERT_TRUE(now_b.has_value());
  EXPECT_EQ(now_a.value(), cycles{400});
  EXPECT_EQ(now_b.value(), cycles{300});
}

TEST_F(VmTimeManagerTest, PauseAllFailsOnLengthMismatch) {
  intrusive_vcpu_list::node node_a{&vcpu_};
  intrusive_vcpu_list vcpus;
  vcpus.push_front(node_a);

  cycles snapshots[2]; // one too many
  auto paused = mgr_.pause_all(vcpus, reloco::span<cycles>(snapshots, 2));
  ASSERT_FALSE(paused.has_value());
  EXPECT_EQ(paused.error(), error::invalid_argument);
}

TEST_F(VmTimeManagerTest, ResumeAllFailsOnLengthMismatch) {
  intrusive_vcpu_list::node node_a{&vcpu_};
  intrusive_vcpu_list vcpus;
  vcpus.push_front(node_a);

  cycles snapshots[2] = {cycles{0}, cycles{0}}; // one too many
  auto resumed = mgr_.resume_all(vcpus, reloco::span<const cycles>(snapshots, 2));
  ASSERT_FALSE(resumed.has_value());
  EXPECT_EQ(resumed.error(), error::invalid_argument);
}

TEST_F(VmTimeManagerTest, PauseAllStopsAtFirstFailingVcpu) {
  fake_vcpu_register vcpu_b;
  vcpu_b.next_read_offset = unexpected(error::io_error);
  intrusive_vcpu_list::node node_a{&vcpu_}, node_b{&vcpu_b};
  intrusive_vcpu_list vcpus;
  vcpus.push_front(node_b);
  vcpus.push_front(node_a);

  host_.next_value = cycles{1'000};
  cycles snapshots[2];
  auto paused = mgr_.pause_all(vcpus, reloco::span<cycles>(snapshots, 2));
  ASSERT_FALSE(paused.has_value());
  EXPECT_EQ(paused.error(), error::io_error);
}

namespace {

// A second, ARM-shaped fake backend, to exercise a Traits whose native register uses the opposite
// (subtract) sign convention and must negate at the boundary -- see the @file-level docs' ARM example.
struct fake_arm_cntvoff_register {
  std::uint64_t cntvoff_el2 = 0; // natively: guest = host - cntvoff_el2
};

struct fake_arm_vm_timer_traits {
  using vcpu_handle = fake_arm_cntvoff_register *;

  static void apply_offset(vcpu_handle vcpu, cycles offset) noexcept {
    vcpu->cntvoff_el2 = cycles{0}.wrapping_sub(offset).raw();
  }

  static result<cycles> try_read_offset(vcpu_handle vcpu) noexcept {
    return cycles{0}.wrapping_sub(cycles{vcpu->cntvoff_el2});
  }
};

} // namespace

TEST(VmTimeManagerArmConventionTest, SubtractConventionBackendRoundTripsThroughTheCommonOffsetConvention) {
  fake_host_counter host;
  host.next_value = cycles{10'000};
  time_source_ref host_ref(host);
  fake_arm_cntvoff_register vcpu;
  vm_time_manager<fake_arm_vm_timer_traits> mgr(host_ref);

  ASSERT_TRUE(mgr.rebind(&vcpu, cycles{10'500}).has_value());
  // guest = host - cntvoff_el2 => cntvoff_el2 = host - guest = 10'000 - 10'500 (wrapping).
  EXPECT_EQ(cycles{vcpu.cntvoff_el2}, cycles{10'000}.wrapping_sub(cycles{10'500}));

  auto guest_now = mgr.try_guest_now(&vcpu);
  ASSERT_TRUE(guest_now.has_value());
  EXPECT_EQ(guest_now.value(), cycles{10'500});
}
