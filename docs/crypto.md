# Crypto wrappers (`structo/crypto/`)

Traits-based, allocation-free-in-structo handles over a crypto library, in the same style as
`allocator_ref`: a `Tag` selects a `*_traits<Tag>` specialization, an optional `context_type` carries
library state, and the handle is a small type-erased ref.

| Header | Provides |
|---|---|
| `crypto_types.hpp` | `hash_algorithm`, `signature_scheme`, `digest_size()`, `constant_time_equal()`, `STRUCTO_CRYPTO_OPERATION_STORAGE` |
| `hash_ref.hpp` | `hash_traits<Tag>`, `hash_ref`, `hash_operation` |
| `signature_verifier_ref.hpp` | `signature_verifier_traits<Tag>`, `signature_verifier_ref` |
| `image_verify.hpp` | `try_check_digest`, `try_verify_image` |
| `backends/mbedtls.hpp` | mbedTLS 3.x backend (PSA hashing, `mbedtls_pk` verification); not header-checked |

```cpp
// Library-wide state; calls psa_crypto_init(). Must outlive every ref and operation built from it.
structo::crypto::mbedtls_context lib;

// Refs are non-owning; the (Tag, Context&) form binds the context, the Tag-only form is for stateless backends.
structo::crypto::hash_ref hash(structo::crypto::mbedtls_hash_tag{}, lib);
structo::crypto::signature_verifier_ref verifier(structo::crypto::mbedtls_verifier_tag{});

// Streaming hash. `op` OWNS the backend state: movable, not copyable, released when dropped.
auto op = hash.try_start(structo::crypto::hash_algorithm::sha256);
(void)op->try_update(chunk);                 // feed data incrementally
reloco::array<std::byte, structo::crypto::max_digest_size> digest;  // must be >= digest_size(alg)
auto n = op->try_finish(digest);             // consumes `op`; digest[0..*n) is valid

// One-shot: hash an image and verify its signature.
// key = DER SubjectPublicKeyInfo; ok == error::security_violation for a wrong signature,
// error::invalid_argument for a malformed key/signature or scheme/key mismatch.
auto ok = structo::crypto::try_verify_image(hash, verifier, structo::crypto::signature_scheme::ecdsa_der,
                                            structo::crypto::hash_algorithm::sha256, key, image, sig);
```

Notes:
- `hash_operation` stores the backend `state_type` inline (`STRUCTO_CRYPTO_OPERATION_STORAGE`, default 512 bytes);
  `state_type` must be nothrow-move-constructible and nothrow-destructible.
- Inactive operations (default, moved-from, finished) fail with `error::invalid_state`; an output buffer smaller
  than the digest fails with `error::capacity_exceeded` and leaves the operation usable.
- The mbedTLS verifier allocates internally while parsing the key (freed before returning).
- To test the real backend: configure with `-DJPLCZ_STRUCTO_MBEDTLS_INCLUDE_DIR=<mbedtls>/include
  -DJPLCZ_STRUCTO_MBEDTLS_LIBRARY=<libmbedcrypto.a or similar>` and build `jplcz_structo_mbedtls_tests`.
