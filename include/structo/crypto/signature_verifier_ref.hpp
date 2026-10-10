// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file signature_verifier_ref.hpp
 * @brief `structo::crypto::signature_verifier_ref`, a type-erased, non-owning
 * handle over a public-key signature verifier (RSA PKCS#1 v1.5 / PSS, ECDSA),
 * plus the `signature_verifier_traits<Tag>` customization point.
 *
 * Verification is one-shot over an already computed digest (hash the image
 * first with `hash_ref`, or use `try_verify_image` from `image_verify.hpp`),
 * so the handle keeps no per-call state; whatever the library allocates while
 * parsing the key lives and dies inside the backend's `try_verify`.
 *
 * @code
 * struct my_verifier_tag {};
 * template <> struct structo::crypto::signature_verifier_traits<my_verifier_tag> {
 *   using context_type = my_library_context;   // `void` if stateless
 *   // Check that `signature` is a valid `scheme` signature over `digest` (computed with `alg`) by the
 *   // public key `public_key` (DER SubjectPublicKeyInfo). Return error::security_violation for a
 *   // well-formed but wrong signature, error::invalid_argument for a malformed key/signature/key-type
 *   // mismatch. Omit `ctx` if context_type is void.
 *   static reloco::result<void> try_verify(reloco::value_ref<context_type> ctx, signature_scheme scheme,
 *                                          hash_algorithm alg, reloco::span<const std::byte> public_key,
 *                                          reloco::span<const std::byte> digest,
 *                                          reloco::span<const std::byte> signature) noexcept;
 *   // Optional: whether `scheme` is compiled in. Absent means "assume supported".
 *   static bool is_supported(reloco::value_ref<context_type> ctx, signature_scheme scheme) noexcept;
 * };
 *
 * my_library_context lib;                                          // must outlive the ref
 * structo::crypto::signature_verifier_ref verifier(my_verifier_tag{}, lib);
 * auto ok = verifier.try_verify(structo::crypto::signature_scheme::ecdsa_der,
 *                               structo::crypto::hash_algorithm::sha256,
 *                               trusted_spki_der,   // the platform's root-of-trust public key
 *                               digest,             // from hash_ref::try_hash
 *                               signature);         // from the image's signature block
 * if (!ok)
 *   return;                                         // reject the image
 * @endcode
 */

#include <memory>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <reloco/value_ref.hpp>
#include <structo/crypto/crypto_types.hpp>
#include <type_traits>

namespace structo::crypto {

using namespace reloco;

template <typename Tag> struct signature_verifier_traits;

class RELOCO_POINTER signature_verifier_ref {
public:
  struct vtable {
    result<void> (*verify)(void *ctx, signature_scheme scheme, hash_algorithm alg, span<const std::byte> key,
                           span<const std::byte> digest, span<const std::byte> signature) noexcept;
    bool (*is_supported)(void *ctx, signature_scheme scheme) noexcept;
  };

  constexpr signature_verifier_ref() noexcept = default;

  /** @brief Binds a stateless backend (`context_type` is `void`). */
  template <typename Tag, typename Traits = signature_verifier_traits<Tag>,
            std::enable_if_t<std::is_void_v<typename Traits::context_type>, int> = 0>
  constexpr explicit signature_verifier_ref(Tag) noexcept : vtbl_(&s_vtbl<Tag>) {}

  /** @brief Binds a stateful backend to @p ctx. */
  template <typename Tag, typename Context, typename Traits = signature_verifier_traits<Tag>,
            std::enable_if_t<!std::is_void_v<typename Traits::context_type> &&
                                 std::is_convertible_v<Context *, typename Traits::context_type *>,
                             int> = 0>
  constexpr signature_verifier_ref(Tag, Context &ctx RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(ctx)), vtbl_(&s_vtbl<Tag>) {}

  template <typename Tag, typename Context, std::enable_if_t<!std::is_lvalue_reference_v<Context>, int> = 0>
  signature_verifier_ref(Tag, Context &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  [[nodiscard]] bool is_supported(signature_scheme scheme) const noexcept {
    return vtbl_ && vtbl_->is_supported(ctx_, scheme);
  }

  /** @brief Verifies @p signature over @p digest. `error::unsupported_operation` if unbound or the scheme is not
   * supported, `error::invalid_argument` if @p digest is not `digest_size(alg)` bytes, else the backend's result
   * (`error::security_violation` means "signature does not match"). */
  [[nodiscard]] result<void> try_verify(signature_scheme scheme, hash_algorithm alg, span<const std::byte> public_key,
                                        span<const std::byte> digest, span<const std::byte> signature) const noexcept {
    if (!vtbl_ || !vtbl_->is_supported(ctx_, scheme))
      return unexpected(error::unsupported_operation);
    if (digest.size() != digest_size(alg))
      return unexpected(error::invalid_argument);
    return vtbl_->verify(ctx_, scheme, alg, public_key, digest, signature);
  }

private:
  template <typename Traits>
  static constexpr auto has_is_supported(int) -> decltype((void)&Traits::is_supported, true) {
    return true;
  }
  template <typename Traits> static constexpr bool has_is_supported(...) { return false; }

  template <typename Tag>
  static result<void> verify_entry(void *ctx, signature_scheme scheme, hash_algorithm alg, span<const std::byte> key,
                                   span<const std::byte> digest, span<const std::byte> signature) noexcept {
    using traits = signature_verifier_traits<Tag>;
    if constexpr (std::is_void_v<typename traits::context_type>) {
      (void)ctx;
      return traits::try_verify(scheme, alg, key, digest, signature);
    } else {
      return traits::try_verify(
          value_ref<typename traits::context_type>(*static_cast<typename traits::context_type *>(ctx)), scheme, alg,
          key, digest, signature);
    }
  }

  template <typename Tag> static bool is_supported_entry(void *ctx, signature_scheme scheme) noexcept {
    using traits = signature_verifier_traits<Tag>;
    if constexpr (has_is_supported<traits>(0)) {
      if constexpr (std::is_void_v<typename traits::context_type>) {
        (void)ctx;
        return traits::is_supported(scheme);
      } else {
        return traits::is_supported(
            value_ref<typename traits::context_type>(*static_cast<typename traits::context_type *>(ctx)), scheme);
      }
    } else {
      (void)ctx;
      (void)scheme;
      return true;
    }
  }

  template <typename Tag> static constexpr vtable s_vtbl{&verify_entry<Tag>, &is_supported_entry<Tag>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace structo::crypto
