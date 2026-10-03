// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/otp_storage.hpp>

#include <array>
#include <cstddef>
#include <cstring>

namespace {

using namespace structo;
using namespace structo::hw;

// --------------------------------------------------------------------
// A fake OTP/eFuse backend: a fixed in-memory byte array with OR-only
// programming semantics, plus a single lockable range and a settable
// `available` flag. Implements both optional probes (`is_locked`,
// `try_lock`) and the optional `is_available` probe.
// --------------------------------------------------------------------
struct fake_otp {
  std::array<std::byte, 16> mem{};
  std::size_t locked_from = 0;
  std::size_t locked_len = 0;
  bool available = true;
  bool next_program_fails = false;
};

} // namespace

template <> struct structo::hw::otp_storage_traits<fake_otp> {
  static std::size_t size_bytes(fake_otp &b) noexcept { return b.mem.size(); }

  static reloco::result<void> try_read(fake_otp &b, std::size_t offset, reloco::span<std::byte> dst) noexcept {
    std::memcpy(dst.data(), b.mem.data() + offset, dst.size());
    return {};
  }

  static reloco::result<void> try_program(fake_otp &b, std::size_t offset,
                                          reloco::span<const std::byte> bits) noexcept {
    if (b.next_program_fails)
      return reloco::unexpected(reloco::error::busy);
    for (std::size_t i = 0; i < bits.size(); ++i)
      b.mem[offset + i] |= bits[i];
    return {};
  }

  static bool is_locked(fake_otp &b, std::size_t offset, std::size_t len) noexcept {
    return offset < b.locked_from + b.locked_len && offset + len > b.locked_from;
  }

  static reloco::result<void> try_lock(fake_otp &b, std::size_t offset, std::size_t len) noexcept {
    b.locked_from = offset;
    b.locked_len = len;
    return {};
  }

  static bool is_available(fake_otp &b) noexcept { return b.available; }
};

namespace {

// --------------------------------------------------------------------
// A minimal backend with NO optional members, exercising every
// optional-trait fallback.
// --------------------------------------------------------------------
struct bare_otp {
  std::array<std::byte, 8> mem{};
};

} // namespace

template <> struct structo::hw::otp_storage_traits<bare_otp> {
  static std::size_t size_bytes(bare_otp &b) noexcept { return b.mem.size(); }
  static reloco::result<void> try_read(bare_otp &b, std::size_t offset, reloco::span<std::byte> dst) noexcept {
    std::memcpy(dst.data(), b.mem.data() + offset, dst.size());
    return {};
  }
  static reloco::result<void> try_program(bare_otp &b, std::size_t offset,
                                          reloco::span<const std::byte> bits) noexcept {
    for (std::size_t i = 0; i < bits.size(); ++i)
      b.mem[offset + i] |= bits[i];
    return {};
  }
};

namespace {

TEST(OtpStorageRefTest, UnboundRefFailsEveryOperation) {
  otp_storage_ref unbound;
  EXPECT_FALSE(static_cast<bool>(unbound));
  EXPECT_FALSE(unbound.is_available());
  EXPECT_EQ(unbound.size_bytes(), 0u);
  EXPECT_FALSE(unbound.is_locked(0, 1));

  std::array<std::byte, 4> buf{};
  auto r = unbound.try_read(0, buf);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::unsupported_operation);

  auto p = unbound.try_program(0, buf);
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), error::unsupported_operation);

  auto l = unbound.try_lock(0, 1);
  ASSERT_FALSE(l.has_value());
  EXPECT_EQ(l.error(), error::unsupported_operation);
}

TEST(OtpStorageRefTest, BoundRefReportsSizeAndAvailability) {
  fake_otp dev;
  otp_storage_ref ref(dev);
  EXPECT_TRUE(static_cast<bool>(ref));
  EXPECT_EQ(ref.size_bytes(), 16u);

  dev.available = true;
  EXPECT_TRUE(ref.is_available());
  dev.available = false;
  EXPECT_FALSE(ref.is_available());
}

TEST(OtpStorageRefTest, IsAvailableAssumedTrueWithoutOptionalProbe) {
  bare_otp dev;
  otp_storage_ref ref(dev);
  EXPECT_TRUE(ref.is_available());
}

TEST(OtpStorageRefTest, TryProgramThenReadBackSucceeds) {
  fake_otp dev;
  otp_storage_ref ref(dev);

  std::array<std::byte, 2> bits{std::byte{0b0000'1111}, std::byte{0xFF}};
  auto p = ref.try_program(0, bits);
  ASSERT_TRUE(p.has_value());

  std::array<std::byte, 2> readback{};
  auto r = ref.try_read(0, readback);
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(readback[0], std::byte{0b0000'1111});
  EXPECT_EQ(readback[1], std::byte{0xFF});
}

TEST(OtpStorageRefTest, TryProgramIsOrOnlyAndIdempotentOnOverlappingBits) {
  fake_otp dev;
  otp_storage_ref ref(dev);

  std::array<std::byte, 1> first{std::byte{0b0000'0001}};
  std::array<std::byte, 1> second{std::byte{0b0000'0010}};
  ASSERT_TRUE(ref.try_program(0, first).has_value());
  ASSERT_TRUE(ref.try_program(0, second).has_value());
  // Re-programming the same bit again must be a harmless no-op.
  ASSERT_TRUE(ref.try_program(0, first).has_value());

  std::array<std::byte, 1> readback{};
  ASSERT_TRUE(ref.try_read(0, readback).has_value());
  EXPECT_EQ(readback[0], std::byte{0b0000'0011});
}

TEST(OtpStorageRefTest, TryReadOutOfRangeFails) {
  fake_otp dev;
  otp_storage_ref ref(dev);

  std::array<std::byte, 4> buf{};
  auto r = ref.try_read(15, buf);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::out_of_range);
}

TEST(OtpStorageRefTest, TryProgramOutOfRangeFails) {
  fake_otp dev;
  otp_storage_ref ref(dev);

  std::array<std::byte, 4> bits{};
  auto p = ref.try_program(14, bits);
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), error::out_of_range);
}

TEST(OtpStorageRefTest, TryProgramPropagatesBackendError) {
  fake_otp dev;
  dev.next_program_fails = true;
  otp_storage_ref ref(dev);

  std::array<std::byte, 1> bits{std::byte{1}};
  auto p = ref.try_program(0, bits);
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), error::busy);
}

TEST(OtpStorageRefTest, TryLockThenIsLockedReportsLockedRange) {
  fake_otp dev;
  otp_storage_ref ref(dev);

  EXPECT_FALSE(ref.is_locked(0, 4));
  auto l = ref.try_lock(0, 4);
  ASSERT_TRUE(l.has_value());
  EXPECT_TRUE(ref.is_locked(0, 2));
  EXPECT_TRUE(ref.is_locked(2, 2));
  EXPECT_FALSE(ref.is_locked(4, 4));
}

TEST(OtpStorageRefTest, TryProgramFailsWithPermissionDeniedInsideLockedRange) {
  fake_otp dev;
  otp_storage_ref ref(dev);
  ASSERT_TRUE(ref.try_lock(0, 4).has_value());

  std::array<std::byte, 2> bits{std::byte{1}, std::byte{1}};
  auto p = ref.try_program(0, bits);
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error(), error::permission_denied);

  // Programming entirely outside the locked range must still succeed.
  auto p2 = ref.try_program(4, bits);
  EXPECT_TRUE(p2.has_value());
}

TEST(OtpStorageRefTest, IsLockedAndTryLockFallBackWithoutOptionalProbes) {
  bare_otp dev;
  otp_storage_ref ref(dev);

  EXPECT_FALSE(ref.is_locked(0, 1));
  auto l = ref.try_lock(0, 1);
  ASSERT_FALSE(l.has_value());
  EXPECT_EQ(l.error(), error::unsupported_operation);
}

TEST(OtpStorageRefTest, TryProgramVerifySucceedsWhenBitsTake) {
  fake_otp dev;
  otp_storage_ref ref(dev);

  std::array<std::byte, 2> bits{std::byte{0b0001'0000}, std::byte{0}};
  std::array<std::byte, 2> scratch{};
  auto v = ref.try_program_verify(0, bits, scratch);
  EXPECT_TRUE(v.has_value());
}

TEST(OtpStorageRefTest, TryProgramVerifyToleratesPreviouslyProgrammedUnrelatedBits) {
  fake_otp dev;
  otp_storage_ref ref(dev);

  std::array<std::byte, 1> earlier{std::byte{0b1000'0000}};
  ASSERT_TRUE(ref.try_program(0, earlier).has_value());

  std::array<std::byte, 1> bits{std::byte{0b0000'0001}};
  std::array<std::byte, 1> scratch{};
  auto v = ref.try_program_verify(0, bits, scratch);
  EXPECT_TRUE(v.has_value());
}

TEST(OtpStorageRefTest, TryProgramVerifyFailsWhenScratchTooSmall) {
  fake_otp dev;
  otp_storage_ref ref(dev);

  std::array<std::byte, 2> bits{};
  std::array<std::byte, 1> scratch{};
  auto v = ref.try_program_verify(0, bits, scratch);
  ASSERT_FALSE(v.has_value());
  EXPECT_EQ(v.error(), error::invalid_argument);
}

} // namespace
