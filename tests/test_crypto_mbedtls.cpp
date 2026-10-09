// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/crypto/backends/mbedtls.hpp>
#include <structo/crypto/image_verify.hpp>

#include <reloco/array.hpp>

#include <utility>

using namespace structo::crypto;
using reloco::error;
using reloco::span;

namespace {

using cbytes_t = span<const std::byte>;

// ECDSA P-256 / SHA-256 over "structo", generated with openssl (SPKI DER key, DER signature).
const reloco::array<std::byte, 91> ec_pub = {std::byte{0x30}, std::byte{0x59}, std::byte{0x30}, std::byte{0x13}, std::byte{0x06}, std::byte{0x07}, std::byte{0x2a}, std::byte{0x86}, std::byte{0x48}, std::byte{0xce}, std::byte{0x3d}, std::byte{0x02}, std::byte{0x01}, std::byte{0x06}, std::byte{0x08}, std::byte{0x2a}, std::byte{0x86}, std::byte{0x48}, std::byte{0xce}, std::byte{0x3d}, std::byte{0x03}, std::byte{0x01}, std::byte{0x07}, std::byte{0x03}, std::byte{0x42}, std::byte{0x00}, std::byte{0x04}, std::byte{0x26}, std::byte{0x6e}, std::byte{0x1e}, std::byte{0x5b}, std::byte{0xef}, std::byte{0xb1}, std::byte{0xb7}, std::byte{0x69}, std::byte{0x08}, std::byte{0x1c}, std::byte{0x9b}, std::byte{0x8a}, std::byte{0x58}, std::byte{0xa1}, std::byte{0x47}, std::byte{0xfc}, std::byte{0xdc}, std::byte{0x30}, std::byte{0x95}, std::byte{0x4b}, std::byte{0xb1}, std::byte{0x6b}, std::byte{0xe0}, std::byte{0x92}, std::byte{0xeb}, std::byte{0x5a}, std::byte{0x0d}, std::byte{0x70}, std::byte{0xac}, std::byte{0xeb}, std::byte{0xeb}, std::byte{0x29}, std::byte{0xf2}, std::byte{0x60}, std::byte{0x76}, std::byte{0xcc}, std::byte{0x7b}, std::byte{0x70}, std::byte{0x2a}, std::byte{0xaf}, std::byte{0x6d}, std::byte{0x32}, std::byte{0x80}, std::byte{0xb1}, std::byte{0x07}, std::byte{0xc4}, std::byte{0xbb}, std::byte{0xb6}, std::byte{0xb6}, std::byte{0x7e}, std::byte{0xd4}, std::byte{0x6f}, std::byte{0x57}, std::byte{0xf4}, std::byte{0x77}, std::byte{0x08}, std::byte{0x59}, std::byte{0x8d}, std::byte{0x0b}, std::byte{0xf0}, std::byte{0x2b}, std::byte{0x1b}, std::byte{0x30}, std::byte{0x3b}};
const reloco::array<std::byte, 71> ec_sig = {std::byte{0x30}, std::byte{0x45}, std::byte{0x02}, std::byte{0x21}, std::byte{0x00}, std::byte{0xda}, std::byte{0x51}, std::byte{0xa9}, std::byte{0x8a}, std::byte{0x06}, std::byte{0x0d}, std::byte{0x7c}, std::byte{0xcb}, std::byte{0xd7}, std::byte{0x80}, std::byte{0x00}, std::byte{0x7b}, std::byte{0xcb}, std::byte{0x41}, std::byte{0x03}, std::byte{0x01}, std::byte{0xb6}, std::byte{0xb6}, std::byte{0xd2}, std::byte{0x87}, std::byte{0x1b}, std::byte{0xa2}, std::byte{0x50}, std::byte{0x46}, std::byte{0x4c}, std::byte{0x03}, std::byte{0x2f}, std::byte{0x21}, std::byte{0x54}, std::byte{0x0c}, std::byte{0xc9}, std::byte{0xfc}, std::byte{0x02}, std::byte{0x20}, std::byte{0x5a}, std::byte{0xe5}, std::byte{0xe4}, std::byte{0x39}, std::byte{0x75}, std::byte{0x3e}, std::byte{0xa6}, std::byte{0x70}, std::byte{0x17}, std::byte{0x5e}, std::byte{0xd0}, std::byte{0x4a}, std::byte{0xac}, std::byte{0xe4}, std::byte{0xfc}, std::byte{0x02}, std::byte{0x4d}, std::byte{0x17}, std::byte{0x1e}, std::byte{0x25}, std::byte{0x2e}, std::byte{0x11}, std::byte{0x5d}, std::byte{0xec}, std::byte{0x14}, std::byte{0x10}, std::byte{0xc5}, std::byte{0x2d}, std::byte{0xf2}, std::byte{0xaf}, std::byte{0x20}, std::byte{0xe8}};

const reloco::array<std::byte, 7> message = {std::byte{'s'}, std::byte{'t'}, std::byte{'r'}, std::byte{'u'},
                                             std::byte{'c'}, std::byte{'t'}, std::byte{'o'}};
const reloco::array<std::byte, 3> abc = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};

class CryptoMbedtlsTest : public ::testing::Test {
protected:
  mbedtls_context lib;
  hash_ref hash{mbedtls_hash_tag{}, lib};
  signature_verifier_ref verifier{mbedtls_verifier_tag{}};
};

TEST_F(CryptoMbedtlsTest, Sha256KnownAnswer) {
  ASSERT_TRUE(lib);
  reloco::array<std::byte, 32> out{};
  auto n = hash.try_hash(hash_algorithm::sha256, cbytes_t(abc), span<std::byte>(out));
  ASSERT_TRUE(n);
  EXPECT_EQ(*n, 32u);
  EXPECT_EQ(out[0], std::byte{0xba});
  EXPECT_EQ(out[1], std::byte{0x78});
  EXPECT_EQ(out[2], std::byte{0x16});
  EXPECT_EQ(out[31], std::byte{0xad});
}

TEST_F(CryptoMbedtlsTest, MovedOperationKeepsState) {
  auto r = hash.try_start(hash_algorithm::sha256);
  ASSERT_TRUE(r);
  hash_operation a = std::move(*r);
  ASSERT_TRUE(a.try_update(cbytes_t(abc).first(1)));
  hash_operation b = std::move(a);
  ASSERT_TRUE(b.try_update(cbytes_t(abc).subspan(1)));
  EXPECT_FALSE(a);
  reloco::array<std::byte, 32> out{};
  ASSERT_TRUE(b.try_finish(span<std::byte>(out)));
  EXPECT_EQ(out[0], std::byte{0xba});
  EXPECT_EQ(out[31], std::byte{0xad});
}

TEST_F(CryptoMbedtlsTest, EcdsaVerifyImage) {
  EXPECT_TRUE(try_verify_image(hash, verifier, signature_scheme::ecdsa_der, hash_algorithm::sha256, cbytes_t(ec_pub),
                               cbytes_t(message), cbytes_t(ec_sig)));
  // Wrong data -> signature mismatch.
  EXPECT_EQ(try_verify_image(hash, verifier, signature_scheme::ecdsa_der, hash_algorithm::sha256, cbytes_t(ec_pub),
                             cbytes_t(abc), cbytes_t(ec_sig))
                .error(),
            error::security_violation);
  // EC key with an RSA scheme -> malformed request.
  EXPECT_EQ(try_verify_image(hash, verifier, signature_scheme::rsa_pkcs1_v15, hash_algorithm::sha256,
                             cbytes_t(ec_pub), cbytes_t(message), cbytes_t(ec_sig))
                .error(),
            error::invalid_argument);
  // Garbage key.
  EXPECT_EQ(try_verify_image(hash, verifier, signature_scheme::ecdsa_der, hash_algorithm::sha256, cbytes_t(abc),
                             cbytes_t(message), cbytes_t(ec_sig))
                .error(),
            error::invalid_argument);
}

} // namespace
