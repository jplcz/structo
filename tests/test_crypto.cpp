// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/crypto/hash_ref.hpp>
#include <structo/crypto/image_verify.hpp>
#include <structo/crypto/signature_verifier_ref.hpp>

#include <reloco/array.hpp>

#include <utility>

using reloco::error;
using reloco::span;
using namespace structo::crypto;

namespace {

using cbytes_t = span<const std::byte>;

// Fake backend context: counts live states so tests can prove ownership.
struct fake_context {
  int live = 0;
  int created = 0;
  bool sha512 = false;
};

// Fake state: a one-byte additive checksum.
struct fake_state {
  fake_context *ctx;
  std::uint8_t sum = 0;

  explicit fake_state(fake_context &c) noexcept : ctx(&c) {
    ++ctx->live;
    ++ctx->created;
  }
  fake_state(fake_state &&o) noexcept : ctx(o.ctx), sum(o.sum) { ++ctx->live; }
  fake_state(const fake_state &) = delete;
  fake_state &operator=(const fake_state &) = delete;
  ~fake_state() { --ctx->live; }
};

struct fake_hash_tag {};
struct fake_verifier_tag {};

} // namespace

template <> struct structo::crypto::hash_traits<fake_hash_tag> {
  using context_type = fake_context;
  using state_type = fake_state;

  static reloco::result<state_type> try_start(reloco::value_ref<context_type> ctx, hash_algorithm) noexcept {
    return state_type(*ctx);
  }
  static reloco::result<void> try_update(state_type &s, cbytes_t data) noexcept {
    for (auto b : data)
      s.sum = static_cast<std::uint8_t>(s.sum + static_cast<std::uint8_t>(b));
    return {};
  }
  static reloco::result<std::size_t> try_finish(state_type &s, span<std::byte> out) noexcept {
    for (auto &b : out)
      b = std::byte{0};
    out[0] = static_cast<std::byte>(s.sum);
    return std::size_t{32};
  }
  static bool is_supported(reloco::value_ref<context_type> ctx, hash_algorithm alg) noexcept {
    return alg != hash_algorithm::sha512 || ctx->sha512;
  }
};

// "Signature" is valid iff signature[0] == digest[0] and the key is non-empty.
template <> struct structo::crypto::signature_verifier_traits<fake_verifier_tag> {
  using context_type = void;

  static reloco::result<void> try_verify(signature_scheme, hash_algorithm, cbytes_t key, cbytes_t digest,
                                         cbytes_t sig) noexcept {
    if (key.empty() || sig.empty() || digest.empty())
      return reloco::unexpected(error::invalid_argument);
    if (sig[0] != digest[0])
      return reloco::unexpected(error::security_violation);
    return {};
  }
};

namespace {

class CryptoTest : public ::testing::Test {
protected:
  fake_context ctx;
  static cbytes_t bytes(const reloco::array<std::byte, 3> &a) { return cbytes_t(a); }
  reloco::array<std::byte, 3> msg = {std::byte{1}, std::byte{2}, std::byte{3}}; // sum = 6
};

TEST_F(CryptoTest, OperationOwnsStateAndReleasesIt) {
  hash_ref hash(fake_hash_tag{}, ctx);
  {
    auto op = hash.try_start(hash_algorithm::sha256);
    ASSERT_TRUE(op);
    EXPECT_EQ(ctx.live, 1);
  }
  EXPECT_EQ(ctx.live, 0);
}

TEST_F(CryptoTest, MoveTransfersOwnership) {
  hash_ref hash(fake_hash_tag{}, ctx);
  auto r = hash.try_start(hash_algorithm::sha256);
  ASSERT_TRUE(r);
  hash_operation a = std::move(*r);
  hash_operation b = std::move(a);
  EXPECT_FALSE(a);
  EXPECT_TRUE(b);
  EXPECT_EQ(ctx.live, 1);
  EXPECT_EQ(a.try_update(bytes(msg)).error(), error::invalid_state);

  // Move-assign resets the destination's previous state first.
  auto r2 = hash.try_start(hash_algorithm::sha256);
  ASSERT_TRUE(r2);
  EXPECT_EQ(ctx.live, 2);
  b = std::move(*r2);
  EXPECT_EQ(ctx.live, 1);
}

TEST_F(CryptoTest, FinishConsumesOperation) {
  hash_ref hash(fake_hash_tag{}, ctx);
  auto op = hash.try_start(hash_algorithm::sha256);
  ASSERT_TRUE(op);
  ASSERT_TRUE(op->try_update(bytes(msg)));
  reloco::array<std::byte, 32> out{};
  auto n = op->try_finish(span<std::byte>(out));
  ASSERT_TRUE(n);
  EXPECT_EQ(*n, 32u);
  EXPECT_EQ(out[0], std::byte{6});
  EXPECT_FALSE(*op);
  EXPECT_EQ(op->try_finish(span<std::byte>(out)).error(), error::invalid_state);
}

TEST_F(CryptoTest, SmallOutputFailsAndKeepsOperationUsable) {
  hash_ref hash(fake_hash_tag{}, ctx);
  auto op = hash.try_start(hash_algorithm::sha256);
  ASSERT_TRUE(op);
  reloco::array<std::byte, 8> small{};
  EXPECT_EQ(op->try_finish(span<std::byte>(small)).error(), error::capacity_exceeded);
  EXPECT_TRUE(*op);
}

TEST_F(CryptoTest, UnsupportedAlgorithmAndUnboundRef) {
  hash_ref hash(fake_hash_tag{}, ctx);
  EXPECT_FALSE(hash.is_supported(hash_algorithm::sha512));
  EXPECT_EQ(hash.try_start(hash_algorithm::sha512).error(), error::unsupported_operation);
  ctx.sha512 = true;
  EXPECT_TRUE(hash.is_supported(hash_algorithm::sha512));

  hash_ref unbound;
  EXPECT_FALSE(unbound);
  EXPECT_EQ(unbound.try_start(hash_algorithm::sha256).error(), error::unsupported_operation);
}

TEST_F(CryptoTest, CheckDigestAndVerifyImage) {
  hash_ref hash(fake_hash_tag{}, ctx);
  reloco::array<std::byte, 32> good{};
  good[0] = std::byte{6};
  EXPECT_TRUE(try_check_digest(hash, hash_algorithm::sha256, bytes(msg), cbytes_t(good)));
  good[31] = std::byte{1};
  EXPECT_EQ(try_check_digest(hash, hash_algorithm::sha256, bytes(msg), cbytes_t(good)).error(),
            error::security_violation);
  EXPECT_EQ(try_check_digest(hash, hash_algorithm::sha256, bytes(msg), bytes(msg)).error(), error::invalid_argument);

  signature_verifier_ref verifier{fake_verifier_tag{}};
  reloco::array<std::byte, 1> key = {std::byte{9}};
  reloco::array<std::byte, 1> sig = {std::byte{6}};
  EXPECT_TRUE(try_verify_image(hash, verifier, signature_scheme::ecdsa_der, hash_algorithm::sha256, cbytes_t(key),
                               bytes(msg), cbytes_t(sig)));
  sig[0] = std::byte{7};
  EXPECT_EQ(try_verify_image(hash, verifier, signature_scheme::ecdsa_der, hash_algorithm::sha256, cbytes_t(key),
                             bytes(msg), cbytes_t(sig))
                .error(),
            error::security_violation);
  EXPECT_EQ(ctx.live, 0);
}

TEST_F(CryptoTest, ConstantTimeEqual) {
  reloco::array<std::byte, 3> same = {std::byte{1}, std::byte{2}, std::byte{3}};
  reloco::array<std::byte, 3> diff = {std::byte{1}, std::byte{2}, std::byte{4}};
  EXPECT_TRUE(constant_time_equal(bytes(msg), cbytes_t(same)));
  EXPECT_FALSE(constant_time_equal(bytes(msg), cbytes_t(diff)));
  EXPECT_FALSE(constant_time_equal(bytes(msg), bytes(msg).first(2)));
}

} // namespace
