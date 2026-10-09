// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file crypto_types.hpp
 * @brief Vocabulary shared by the `structo::crypto` handles: hash and signature
 * algorithm identifiers, digest sizes, a constant-time comparison, and the
 * inline storage budget of backend operation state.
 *
 * The handles (`hash_ref`, `signature_verifier_ref`) follow the
 * `allocator_ref` shape: a `*_traits<Tag>` customization point with a
 * `context_type`, bound through `ref(Tag, context)`. A crypto library such
 * as mbedTLS is adapted once (see `crypto/backends/mbedtls.hpp`) and the rest
 * of a bootloader stays library-agnostic.
 */

#include <cstddef>
#include <cstdint>
#include <reloco/span.hpp>

/** @brief Bytes of inline storage inside every `*_operation` for the backend's state. Override before including
 * if a backend's state is larger (a too-small budget is a compile-time error at bind time). */
#ifndef STRUCTO_CRYPTO_OPERATION_STORAGE
#define STRUCTO_CRYPTO_OPERATION_STORAGE 512
#endif

namespace structo::crypto {

inline constexpr std::size_t operation_storage_size = STRUCTO_CRYPTO_OPERATION_STORAGE;
inline constexpr std::size_t operation_storage_align = alignof(std::max_align_t);

enum class hash_algorithm : uint8_t { sha1, sha256, sha384, sha512 };

/** @brief Digest length in bytes. */
[[nodiscard]] constexpr std::size_t digest_size(hash_algorithm alg) noexcept {
  switch (alg) {
  case hash_algorithm::sha1:
    return 20;
  case hash_algorithm::sha256:
    return 32;
  case hash_algorithm::sha384:
    return 48;
  case hash_algorithm::sha512:
    return 64;
  }
  return 0;
}

inline constexpr std::size_t max_digest_size = 64;

/** @brief How a signature over a digest is encoded/padded. The key is always a DER `SubjectPublicKeyInfo`. */
enum class signature_scheme : uint8_t {
  rsa_pkcs1_v15, ///< RSASSA-PKCS1-v1_5; signature is the raw modulus-sized value.
  rsa_pss,       ///< RSASSA-PSS with MGF1 over the same hash; any salt length accepted.
  ecdsa_der,     ///< ECDSA; signature is a DER `SEQUENCE { r, s }`.
};

/** @brief Compares @p a and @p b without early exit on the first mismatch (lengths are not secret). */
[[nodiscard]] constexpr bool constant_time_equal(reloco::span<const std::byte> a, reloco::span<const std::byte> b) noexcept {
  if (a.size() != b.size())
    return false;
  unsigned diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    diff |= static_cast<unsigned>(a[i]) ^ static_cast<unsigned>(b[i]);
  return diff == 0;
}

} // namespace structo::crypto
