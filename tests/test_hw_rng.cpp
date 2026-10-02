// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/hw/rng.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <deque>

namespace {

using namespace structo;
using namespace structo::hw;

// --------------------------------------------------------------------
// A fake hardware RNG backend: a fixed queue of pre-scripted draws
// (values and/or errors), plus a settable `available` flag. Implements
// the optional `is_available` probe.
// --------------------------------------------------------------------
struct fake_rng {
  std::deque<result<std::uint64_t>> draws;
  bool available = true;
};

} // namespace

template <> struct structo::hw::hw_rng_traits<fake_rng> {
  static reloco::result<std::uint64_t> try_generate64(fake_rng &b) noexcept {
    if (b.draws.empty())
      return reloco::unexpected(reloco::error::try_again);
    auto v = b.draws.front();
    b.draws.pop_front();
    return v;
  }
  static bool is_available(fake_rng &b) noexcept { return b.available; }
};

namespace {

// --------------------------------------------------------------------
// A minimal backend with NO `is_available`, exercising the
// optional-trait fallback (assumed available whenever bound).
// --------------------------------------------------------------------
struct bare_rng {
  std::uint64_t next = 0;
};

} // namespace

template <> struct structo::hw::hw_rng_traits<bare_rng> {
  static reloco::result<std::uint64_t> try_generate64(bare_rng &b) noexcept { return b.next++; }
};

namespace {

TEST(HwRngRefTest, UnboundRefFailsEveryOperation) {
  hw_rng_ref unbound;
  EXPECT_FALSE(static_cast<bool>(unbound));
  EXPECT_FALSE(unbound.is_available());

  auto word = unbound.try_generate64();
  ASSERT_FALSE(word.has_value());
  EXPECT_EQ(word.error(), error::unsupported_operation);

  auto word32 = unbound.try_generate32();
  ASSERT_FALSE(word32.has_value());
  EXPECT_EQ(word32.error(), error::unsupported_operation);

  std::array<std::byte, 8> buf{};
  auto filled = unbound.try_fill(buf);
  ASSERT_FALSE(filled.has_value());
  EXPECT_EQ(filled.error(), error::unsupported_operation);
}

TEST(HwRngRefTest, BoundRefReportsTrue) {
  fake_rng dev;
  hw_rng_ref ref(dev);
  EXPECT_TRUE(static_cast<bool>(ref));
}

TEST(HwRngRefTest, IsAvailableForwardsToBackendProbe) {
  fake_rng dev;
  hw_rng_ref ref(dev);

  dev.available = true;
  EXPECT_TRUE(ref.is_available());

  dev.available = false;
  EXPECT_FALSE(ref.is_available());
}

TEST(HwRngRefTest, IsAvailableAssumedTrueWithoutOptionalProbe) {
  bare_rng dev;
  hw_rng_ref ref(dev);
  EXPECT_TRUE(ref.is_available());
}

TEST(HwRngRefTest, TryGenerate64ReturnsScriptedValue) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{0x0123456789ABCDEFull});
  hw_rng_ref ref(dev);

  auto word = ref.try_generate64();
  ASSERT_TRUE(word.has_value());
  EXPECT_EQ(word.value(), 0x0123456789ABCDEFull);
}

TEST(HwRngRefTest, TryGenerate64RetriesOnTryAgainThenSucceeds) {
  fake_rng dev;
  dev.draws.push_back(reloco::unexpected(error::try_again));
  dev.draws.push_back(reloco::unexpected(error::try_again));
  dev.draws.push_back(std::uint64_t{42});
  hw_rng_ref ref(dev);

  auto word = ref.try_generate64();
  ASSERT_TRUE(word.has_value());
  EXPECT_EQ(word.value(), 42u);
  EXPECT_TRUE(dev.draws.empty());
}

TEST(HwRngRefTest, TryGenerate64FailsWithTryAgainWhenRetriesExhausted) {
  fake_rng dev; // every draw reports try_again (empty queue)
  hw_rng_ref ref(dev);

  auto word = ref.try_generate64(/*max_retries=*/4);
  ASSERT_FALSE(word.has_value());
  EXPECT_EQ(word.error(), error::try_again);
}

TEST(HwRngRefTest, TryGenerate64PropagatesNonTransientErrorImmediately) {
  fake_rng dev;
  dev.draws.push_back(reloco::unexpected(error::io_error));
  hw_rng_ref ref(dev);

  auto word = ref.try_generate64(/*max_retries=*/16);
  ASSERT_FALSE(word.has_value());
  EXPECT_EQ(word.error(), error::io_error);
  // A hard failure must not be retried: exactly one draw was consumed.
  EXPECT_TRUE(dev.draws.empty());
}

TEST(HwRngRefTest, TryGenerate32ReturnsLowHalfOf64BitDraw) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{0xAABBCCDD11223344ull});
  hw_rng_ref ref(dev);

  auto word = ref.try_generate32();
  ASSERT_TRUE(word.has_value());
  EXPECT_EQ(word.value(), 0x11223344u);
}

TEST(HwRngRefTest, TryGenerate32PropagatesError) {
  fake_rng dev; // empty queue -> always try_again
  hw_rng_ref ref(dev);

  auto word = ref.try_generate32(/*max_retries=*/1);
  ASSERT_FALSE(word.has_value());
  EXPECT_EQ(word.error(), error::try_again);
}

TEST(HwRngRefTest, TryFillExactMultipleOfWordSize) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{0x0001020304050607ull});
  dev.draws.push_back(std::uint64_t{0x08090A0B0C0D0E0Full});
  hw_rng_ref ref(dev);

  std::array<std::byte, 16> buf{};
  auto r = ref.try_fill(buf);
  ASSERT_TRUE(r.has_value());

  std::uint64_t w0, w1;
  std::memcpy(&w0, buf.data(), sizeof(w0));
  std::memcpy(&w1, buf.data() + 8, sizeof(w1));
  EXPECT_EQ(w0, 0x0001020304050607ull);
  EXPECT_EQ(w1, 0x08090A0B0C0D0E0Full);
}

TEST(HwRngRefTest, TryFillPartialTrailingWord) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{0x1122334455667788ull});
  hw_rng_ref ref(dev);

  std::array<std::byte, 3> buf{};
  auto r = ref.try_fill(buf);
  ASSERT_TRUE(r.has_value());

  // try_fill memcpy's the low-addressed bytes of the host-endian 64-bit
  // word; only the first 3 bytes of that representation matter here.
  std::uint64_t raw = 0x1122334455667788ull;
  std::array<std::byte, 3> expected{};
  std::memcpy(expected.data(), &raw, 3);
  EXPECT_EQ(buf, expected);
}

TEST(HwRngRefTest, TryFillEmptySpanSucceedsWithoutDrawing) {
  fake_rng dev; // no scripted draws at all
  hw_rng_ref ref(dev);

  auto r = ref.try_fill(span<std::byte>{});
  EXPECT_TRUE(r.has_value());
}

TEST(HwRngRefTest, TryFillStopsAtFirstFailureLeavingBufferPartiallyFilled) {
  fake_rng dev;
  dev.draws.push_back(std::uint64_t{0xFFFFFFFFFFFFFFFFull});
  dev.draws.push_back(reloco::unexpected(error::io_error));
  hw_rng_ref ref(dev);

  std::array<std::byte, 16> buf{};
  auto r = ref.try_fill(buf);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error(), error::io_error);

  std::uint64_t w0;
  std::memcpy(&w0, buf.data(), sizeof(w0));
  EXPECT_EQ(w0, 0xFFFFFFFFFFFFFFFFull);
}

} // namespace
