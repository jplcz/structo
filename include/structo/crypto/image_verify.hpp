// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file image_verify.hpp
 * @brief Loader-side glue over `hash_ref` and `signature_verifier_ref`: check a
 * payload against a known digest (a FIT/FIP/uImage hash property, a pinned
 * measurement) or against a signature by a trusted key.
 *
 * @code
 * // hash / verifier are bound as shown in hash_ref.hpp / signature_verifier_ref.hpp.
 * // image: the kernel payload, e.g. structo::boot::uboot::image_view::payload.
 * auto r = structo::crypto::try_verify_image(
 *     hash, verifier,
 *     structo::crypto::signature_scheme::rsa_pss,    // how the image was signed
 *     structo::crypto::hash_algorithm::sha256,       // digest the signature covers
 *     root_of_trust_spki_der,                         // trusted public key, DER SubjectPublicKeyInfo
 *     image,                                          // bytes that were signed
 *     image_signature);                               // signature bytes
 * if (!r)
 *   halt();                                           // error::security_violation: do not boot it
 *
 * // Pinned-digest variant: compare against a digest stored in a verified manifest.
 * auto same = structo::crypto::try_check_digest(hash, structo::crypto::hash_algorithm::sha256, image, expected_digest);
 * @endcode
 */

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>
#include <structo/crypto/crypto_types.hpp>
#include <structo/crypto/hash_ref.hpp>
#include <structo/crypto/signature_verifier_ref.hpp>

namespace structo::crypto {

/** @brief Hashes @p data with @p alg and compares to @p expected in constant time. `error::security_violation` on
 * mismatch, `error::invalid_argument` if @p expected has the wrong length. */
[[nodiscard]] inline result<void> try_check_digest(const hash_ref &hash, hash_algorithm alg, span<const std::byte> data,
                                                   span<const std::byte> expected) noexcept {
  if (expected.size() != digest_size(alg))
    return unexpected(error::invalid_argument);
  array<std::byte, max_digest_size> digest{};
  auto n = hash.try_hash(alg, data, span<std::byte>(digest));
  if (!n)
    return unexpected(n.error());
  if (!constant_time_equal(span<const std::byte>(digest).first(*n), expected))
    return unexpected(error::security_violation);
  return {};
}

/** @brief Hashes @p data with @p alg and verifies @p signature over the digest with @p public_key. */
[[nodiscard]] inline result<void> try_verify_image(const hash_ref &hash, const signature_verifier_ref &verifier,
                                                   signature_scheme scheme, hash_algorithm alg,
                                                   span<const std::byte> public_key, span<const std::byte> data,
                                                   span<const std::byte> signature) noexcept {
  array<std::byte, max_digest_size> digest{};
  auto n = hash.try_hash(alg, data, span<std::byte>(digest));
  if (!n)
    return unexpected(n.error());
  return verifier.try_verify(scheme, alg, public_key, span<const std::byte>(digest).first(*n), signature);
}

} // namespace structo::crypto
