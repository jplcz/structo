// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file backends/mbedtls.hpp
 * @brief mbedTLS (3.x) backend for `hash_ref` and `signature_verifier_ref`.
 *
 * Not part of the header-check set: it needs the mbedTLS headers and library. Hashing goes through the PSA
 * API (`psa_hash_*`, inline state, no heap); signature verification goes through the `mbedtls_pk_*` API, which
 * allocates internally while parsing the public key (freed before `try_verify` returns).
 *
 * @code
 * #include <structo/crypto/backends/mbedtls.hpp>
 *
 * structo::crypto::mbedtls_context lib;          // calls psa_crypto_init(); keep alive while refs/ops are in use
 * if (!lib)                                      // false if psa_crypto_init() failed
 *   return;
 * structo::crypto::hash_ref hash(structo::crypto::mbedtls_hash_tag{}, lib);                 // needs the context
 * structo::crypto::signature_verifier_ref verifier(structo::crypto::mbedtls_verifier_tag{}); // stateless
 *
 * // Hash a firmware image and verify the signature over the digest with an SPKI DER public key.
 * auto ok = structo::crypto::try_verify_image(hash, verifier, structo::crypto::signature_scheme::rsa_pkcs1_v15,
 *                                             structo::crypto::hash_algorithm::sha256,
 *                                             public_key_der, image, signature);
 * @endcode
 */

#include <cstddef>
#include <cstdint>

#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <psa/crypto.h>

#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>
#include <reloco/value_ref.hpp>
#include <structo/crypto/crypto_types.hpp>
#include <structo/crypto/hash_ref.hpp>
#include <structo/crypto/signature_verifier_ref.hpp>

namespace structo::crypto {

/** @brief Library-wide mbedTLS state: initializes PSA crypto once on construction. */
class mbedtls_context {
public:
  mbedtls_context() noexcept : ready_(psa_crypto_init() == PSA_SUCCESS) {}
  mbedtls_context(const mbedtls_context &) = delete;
  mbedtls_context &operator=(const mbedtls_context &) = delete;

  [[nodiscard]] explicit operator bool() const noexcept { return ready_; }

private:
  bool ready_;
};

/** @brief Owns a `psa_hash_operation_t`; aborts it on destruction. Move uses `psa_hash_clone`. */
class mbedtls_hash_state {
public:
  explicit mbedtls_hash_state(psa_algorithm_t alg) noexcept : op_(PSA_HASH_OPERATION_INIT) {
    active_ = psa_hash_setup(&op_, alg) == PSA_SUCCESS;
  }
  mbedtls_hash_state(const mbedtls_hash_state &) = delete;
  mbedtls_hash_state &operator=(const mbedtls_hash_state &) = delete;
  mbedtls_hash_state(mbedtls_hash_state &&other) noexcept : op_(PSA_HASH_OPERATION_INIT) {
    if (other.active_) {
      active_ = psa_hash_clone(&other.op_, &op_) == PSA_SUCCESS;
      other.reset();
    }
  }
  ~mbedtls_hash_state() { reset(); }

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] psa_hash_operation_t &raw() noexcept { return op_; }
  void reset() noexcept {
    if (active_)
      (void)psa_hash_abort(&op_);
    active_ = false;
  }

private:
  psa_hash_operation_t op_;
  bool active_ = false;
};

namespace detail {

[[nodiscard]] constexpr psa_algorithm_t to_psa_alg(hash_algorithm a) noexcept {
  switch (a) {
  case hash_algorithm::sha1:
    return PSA_ALG_SHA_1;
  case hash_algorithm::sha256:
    return PSA_ALG_SHA_256;
  case hash_algorithm::sha384:
    return PSA_ALG_SHA_384;
  case hash_algorithm::sha512:
    return PSA_ALG_SHA_512;
  }
  return PSA_ALG_NONE;
}

[[nodiscard]] constexpr mbedtls_md_type_t to_md_type(hash_algorithm a) noexcept {
  switch (a) {
  case hash_algorithm::sha1:
    return MBEDTLS_MD_SHA1;
  case hash_algorithm::sha256:
    return MBEDTLS_MD_SHA256;
  case hash_algorithm::sha384:
    return MBEDTLS_MD_SHA384;
  case hash_algorithm::sha512:
    return MBEDTLS_MD_SHA512;
  }
  return MBEDTLS_MD_NONE;
}

[[nodiscard]] inline reloco::error map_pk_error(int rc) noexcept {
  switch (rc) {
  case MBEDTLS_ERR_RSA_VERIFY_FAILED:
  case MBEDTLS_ERR_ECP_VERIFY_FAILED:
  case MBEDTLS_ERR_PK_SIG_LEN_MISMATCH:
    return reloco::error::security_violation;
  case MBEDTLS_ERR_PK_TYPE_MISMATCH:
  case MBEDTLS_ERR_PK_BAD_INPUT_DATA:
  case MBEDTLS_ERR_PK_INVALID_PUBKEY:
  case MBEDTLS_ERR_PK_KEY_INVALID_FORMAT:
  case MBEDTLS_ERR_PK_KEY_INVALID_VERSION:
  case MBEDTLS_ERR_PK_UNKNOWN_PK_ALG:
  case MBEDTLS_ERR_PK_INVALID_ALG:
  case MBEDTLS_ERR_RSA_BAD_INPUT_DATA:
    return reloco::error::invalid_argument;
  case MBEDTLS_ERR_PK_ALLOC_FAILED:
    return reloco::error::allocation_failed;
  default:
    return reloco::error::io_error;
  }
}

class pk_guard {
public:
  pk_guard() noexcept { mbedtls_pk_init(&ctx_); }
  pk_guard(const pk_guard &) = delete;
  pk_guard &operator=(const pk_guard &) = delete;
  ~pk_guard() { mbedtls_pk_free(&ctx_); }
  [[nodiscard]] mbedtls_pk_context &get() noexcept { return ctx_; }

private:
  mbedtls_pk_context ctx_;
};

} // namespace detail

struct mbedtls_hash_tag {};
struct mbedtls_verifier_tag {};

template <> struct hash_traits<mbedtls_hash_tag> {
  using context_type = mbedtls_context;
  using state_type = mbedtls_hash_state;

  static reloco::result<state_type> try_start(reloco::value_ref<context_type> ctx, hash_algorithm alg) noexcept {
    if (!*ctx)
      return reloco::unexpected(reloco::error::not_initialized);
    state_type st(detail::to_psa_alg(alg));
    if (!st.active())
      return reloco::unexpected(reloco::error::unsupported_operation);
    return st;
  }

  static reloco::result<void> try_update(state_type &st, reloco::span<const std::byte> data) noexcept {
    if (!st.active())
      return reloco::unexpected(reloco::error::invalid_state);
    if (psa_hash_update(&st.raw(), reinterpret_cast<const std::uint8_t *>(data.data()), data.size()) != PSA_SUCCESS)
      return reloco::unexpected(reloco::error::io_error);
    return {};
  }

  static reloco::result<std::size_t> try_finish(state_type &st, reloco::span<std::byte> out) noexcept {
    if (!st.active())
      return reloco::unexpected(reloco::error::invalid_state);
    std::size_t len = 0;
    const auto rc = psa_hash_finish(&st.raw(), reinterpret_cast<std::uint8_t *>(out.data()), out.size(), &len);
    st.reset();
    if (rc != PSA_SUCCESS)
      return reloco::unexpected(reloco::error::io_error);
    return len;
  }

  static bool is_supported(reloco::value_ref<context_type> ctx, hash_algorithm alg) noexcept {
    if (!*ctx)
      return false;
    psa_hash_operation_t probe = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&probe, detail::to_psa_alg(alg)) != PSA_SUCCESS)
      return false;
    (void)psa_hash_abort(&probe);
    return true;
  }
};

template <> struct signature_verifier_traits<mbedtls_verifier_tag> {
  using context_type = void;

  static reloco::result<void> try_verify(signature_scheme scheme, hash_algorithm alg,
                                         reloco::span<const std::byte> public_key, reloco::span<const std::byte> digest,
                                         reloco::span<const std::byte> signature) noexcept {
    if (digest.size() != digest_size(alg))
      return reloco::unexpected(reloco::error::invalid_argument);

    detail::pk_guard pk;
    int rc = mbedtls_pk_parse_public_key(&pk.get(), reinterpret_cast<const unsigned char *>(public_key.data()),
                                         public_key.size());
    // Parse failures are PK codes OR-ed with low-level ASN.1 codes, so anything but OOM means a bad key.
    if (rc != 0)
      return reloco::unexpected(rc == MBEDTLS_ERR_PK_ALLOC_FAILED ? reloco::error::allocation_failed
                                                                  : reloco::error::invalid_argument);

    const auto md = detail::to_md_type(alg);
    const auto *dig = reinterpret_cast<const unsigned char *>(digest.data());
    const auto *sig = reinterpret_cast<const unsigned char *>(signature.data());

    switch (scheme) {
    case signature_scheme::rsa_pkcs1_v15:
      if (!mbedtls_pk_can_do(&pk.get(), MBEDTLS_PK_RSA))
        return reloco::unexpected(reloco::error::invalid_argument);
      rc = mbedtls_pk_verify(&pk.get(), md, dig, digest.size(), sig, signature.size());
      break;
    case signature_scheme::rsa_pss: {
      if (!mbedtls_pk_can_do(&pk.get(), MBEDTLS_PK_RSA))
        return reloco::unexpected(reloco::error::invalid_argument);
      mbedtls_pk_rsassa_pss_options opts;
      opts.mgf1_hash_id = md;
      opts.expected_salt_len = MBEDTLS_RSA_SALT_LEN_ANY;
      rc =
          mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS, &opts, &pk.get(), md, dig, digest.size(), sig, signature.size());
      break;
    }
    case signature_scheme::ecdsa_der:
      if (!mbedtls_pk_can_do(&pk.get(), MBEDTLS_PK_ECDSA))
        return reloco::unexpected(reloco::error::invalid_argument);
      rc = mbedtls_pk_verify(&pk.get(), md, dig, digest.size(), sig, signature.size());
      break;
    default:
      return reloco::unexpected(reloco::error::unsupported_operation);
    }

    if (rc != 0)
      return reloco::unexpected(detail::map_pk_error(rc));
    return {};
  }

  static bool is_supported(signature_scheme scheme) noexcept {
    switch (scheme) {
    case signature_scheme::rsa_pkcs1_v15:
    case signature_scheme::rsa_pss:
#if defined(MBEDTLS_RSA_C)
      return true;
#else
      return false;
#endif
    case signature_scheme::ecdsa_der:
#if defined(MBEDTLS_ECDSA_C)
      return true;
#else
      return false;
#endif
    }
    return false;
  }
};

static_assert(sizeof(mbedtls_hash_state) <= operation_storage_size, "raise STRUCTO_CRYPTO_OPERATION_STORAGE");

} // namespace structo::crypto
