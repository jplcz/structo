// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hash_ref.hpp
 * @brief `structo::crypto::hash_ref`, a type-erased, non-owning handle over a
 * hash implementation, and `hash_operation`, the movable, owning handle to one
 * in-progress hash computation.
 *
 * ## Customization point: `hash_traits<Tag>`
 *
 * @code
 * struct my_hash_tag {};                       // identifies the backend
 * template <> struct structo::crypto::hash_traits<my_hash_tag> {
 *   using context_type = my_library_context;   // shared library state; `void` if the backend is stateless
 *   // The backend's per-computation state. Owned by a hash_operation: it is move-constructed when the
 *   // operation moves and destroyed (releasing library resources) when the operation is dropped.
 *   using state_type = my_library_hash_state;
 *   // Begin a computation of `alg` (e.g. sha256). Omit the `ctx` parameter if context_type is void.
 *   static reloco::result<state_type> try_start(reloco::value_ref<context_type> ctx, hash_algorithm alg) noexcept;
 *   // Feed `data` into the computation.
 *   static reloco::result<void> try_update(state_type &state, reloco::span<const std::byte> data) noexcept;
 *   // Write the digest to `out` (already checked to be >= digest_size(alg)), return its length.
 *   static reloco::result<std::size_t> try_finish(state_type &state, reloco::span<std::byte> out) noexcept;
 *   // Optional: whether `alg` is compiled in. Absent means "assume supported".
 *   static bool is_supported(reloco::value_ref<context_type> ctx, hash_algorithm alg) noexcept;
 * };
 *
 * my_library_context lib;                                   // must outlive the ref
 * structo::crypto::hash_ref hash(my_hash_tag{}, lib);        // bind (omit `lib` if context_type is void)
 * auto op = hash.try_start(structo::crypto::hash_algorithm::sha256);   // op owns the backend state
 * (void)op->try_update(chunk1);                              // feed data incrementally
 * (void)op->try_update(chunk2);
 * reloco::array<std::byte, 32> digest;
 * auto n = op->try_finish(digest);                           // consumes op; digest[0..n) is valid
 * @endcode
 *
 * `state_type` must be nothrow-move-constructible and nothrow-destructible, and fit
 * `STRUCTO_CRYPTO_OPERATION_STORAGE` bytes (checked at compile time).
 */

#include <cstddef>
#include <memory>
#include <new>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/span.hpp>
#include <reloco/value_ref.hpp>
#include <structo/crypto/crypto_types.hpp>
#include <type_traits>

namespace structo::crypto {

using namespace reloco;

template <typename Tag> struct hash_traits;

class hash_ref;

/** @brief A hash computation in progress. Owns the backend's state; movable, not copyable. A default-constructed,
 * moved-from, finished, or failed-to-start operation is inactive and every call fails with
 * `error::invalid_state`. Dropping an active operation releases the backend state. */
class hash_operation {
public:
  hash_operation() noexcept = default;
  hash_operation(const hash_operation &) = delete;
  hash_operation &operator=(const hash_operation &) = delete;

  hash_operation(hash_operation &&other) noexcept { take(other); }
  hash_operation &operator=(hash_operation &&other) noexcept {
    if (this != &other) {
      reset();
      take(other);
    }
    return *this;
  }
  ~hash_operation() { reset(); }

  [[nodiscard]] explicit operator bool() const noexcept { return vtbl_ != nullptr; }
  [[nodiscard]] hash_algorithm algorithm() const noexcept { return alg_; }
  [[nodiscard]] std::size_t digest_size() const noexcept { return crypto::digest_size(alg_); }

  /** @brief Feeds @p data into the computation. */
  [[nodiscard]] result<void> try_update(span<const std::byte> data) noexcept {
    if (!vtbl_)
      return unexpected(error::invalid_state);
    return vtbl_->update(storage_, data);
  }

  /** @brief Completes the computation: writes the digest to the front of @p out and yields its length. The
   * operation is consumed (inactive) on success or backend failure; if @p out is smaller than `digest_size()`
   * it fails with `error::capacity_exceeded` and the operation stays usable. */
  [[nodiscard]] result<std::size_t> try_finish(span<std::byte> out) noexcept {
    if (!vtbl_)
      return unexpected(error::invalid_state);
    if (out.size() < digest_size())
      return unexpected(error::capacity_exceeded);
    auto r = vtbl_->finish(storage_, out);
    reset();
    return r;
  }

  /** @brief Abandons the computation and releases the backend state. */
  void reset() noexcept {
    if (vtbl_) {
      vtbl_->destroy(storage_);
      vtbl_ = nullptr;
    }
  }

private:
  friend class hash_ref;

  struct vtable {
    result<void> (*update)(void *state, span<const std::byte> data) noexcept;
    result<std::size_t> (*finish)(void *state, span<std::byte> out) noexcept;
    void (*destroy)(void *state) noexcept;
    void (*move_to)(void *dst, void *src) noexcept;
  };

  template <typename Tag> struct entries {
    using traits = hash_traits<Tag>;
    using state_type = typename traits::state_type;
    static_assert(sizeof(state_type) <= operation_storage_size, "backend state exceeds STRUCTO_CRYPTO_OPERATION_STORAGE");
    static_assert(alignof(state_type) <= operation_storage_align, "backend state over-aligned");
    static_assert(std::is_nothrow_move_constructible_v<state_type> && std::is_nothrow_destructible_v<state_type>,
                  "backend state must be nothrow movable and destructible");

    static result<void> update(void *s, span<const std::byte> d) noexcept {
      return traits::try_update(*static_cast<state_type *>(s), d);
    }
    static result<std::size_t> finish(void *s, span<std::byte> out) noexcept {
      return traits::try_finish(*static_cast<state_type *>(s), out);
    }
    static void destroy(void *s) noexcept { static_cast<state_type *>(s)->~state_type(); }
    static void move_to(void *dst, void *src) noexcept {
      ::new (dst) state_type(static_cast<state_type &&>(*static_cast<state_type *>(src)));
    }
    static constexpr vtable table{&update, &finish, &destroy, &move_to};
  };

  void take(hash_operation &other) noexcept {
    if (!other.vtbl_)
      return;
    other.vtbl_->move_to(storage_, other.storage_);
    other.vtbl_->destroy(other.storage_);
    vtbl_ = other.vtbl_;
    alg_ = other.alg_;
    other.vtbl_ = nullptr;
  }

  alignas(operation_storage_align) std::byte storage_[operation_storage_size];
  const vtable *vtbl_ = nullptr;
  hash_algorithm alg_ = hash_algorithm::sha256;
};

/** @brief Type-erased, non-owning handle to a hash backend (see the file comment). Unbound refs fail with
 * `error::unsupported_operation`. Lifetime: the bound context must outlive the ref and every operation it started. */
class RELOCO_POINTER hash_ref {
public:
  struct vtable {
    result<hash_operation> (*start)(void *ctx, hash_algorithm alg) noexcept;
    bool (*is_supported)(void *ctx, hash_algorithm alg) noexcept;
  };

  constexpr hash_ref() noexcept = default;

  /** @brief Binds a stateless backend (`context_type` is `void`). */
  template <typename Tag, typename Traits = hash_traits<Tag>,
            std::enable_if_t<std::is_void_v<typename Traits::context_type>, int> = 0>
  constexpr explicit hash_ref(Tag) noexcept : vtbl_(&s_vtbl<Tag>) {}

  /** @brief Binds a stateful backend to @p ctx. */
  template <typename Tag, typename Context, typename Traits = hash_traits<Tag>,
            std::enable_if_t<!std::is_void_v<typename Traits::context_type> &&
                                 std::is_convertible_v<Context *, typename Traits::context_type *>,
                             int> = 0>
  constexpr hash_ref(Tag, Context &ctx RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(ctx)), vtbl_(&s_vtbl<Tag>) {}

  template <typename Tag, typename Context, std::enable_if_t<!std::is_lvalue_reference_v<Context>, int> = 0>
  hash_ref(Tag, Context &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief Whether the backend implements @p alg. `false` if unbound. */
  [[nodiscard]] bool is_supported(hash_algorithm alg) const noexcept { return vtbl_ && vtbl_->is_supported(ctx_, alg); }

  /** @brief Starts a computation; the returned operation owns the backend state. */
  [[nodiscard]] result<hash_operation> try_start(hash_algorithm alg) const noexcept {
    if (!vtbl_ || !vtbl_->is_supported(ctx_, alg))
      return unexpected(error::unsupported_operation);
    return vtbl_->start(ctx_, alg);
  }

  /** @brief One-shot digest of @p data into the front of @p out; yields the digest length. */
  [[nodiscard]] result<std::size_t> try_hash(hash_algorithm alg, span<const std::byte> data,
                                             span<std::byte> out) const noexcept {
    auto op = try_start(alg);
    if (!op)
      return unexpected(op.error());
    if (auto r = op->try_update(data); !r)
      return unexpected(r.error());
    return op->try_finish(out);
  }

private:
  template <typename Traits> static constexpr auto has_is_supported(int) -> decltype((void)&Traits::is_supported, true) {
    return true;
  }
  template <typename Traits> static constexpr bool has_is_supported(...) { return false; }

  template <typename Tag> static result<hash_operation> start_entry(void *ctx, hash_algorithm alg) noexcept {
    using traits = hash_traits<Tag>;
    using entry = hash_operation::entries<Tag>;
    result<typename traits::state_type> state = [&]() {
      if constexpr (std::is_void_v<typename traits::context_type>) {
        (void)ctx;
        return traits::try_start(alg);
      } else {
        return traits::try_start(value_ref<typename traits::context_type>(*static_cast<typename traits::context_type *>(ctx)),
                                 alg);
      }
    }();
    if (!state)
      return unexpected(state.error());
    hash_operation op;
    entry::move_to(op.storage_, std::addressof(*state));
    op.vtbl_ = &entry::table;
    op.alg_ = alg;
    return op;
  }

  template <typename Tag> static bool is_supported_entry(void *ctx, hash_algorithm alg) noexcept {
    using traits = hash_traits<Tag>;
    if constexpr (has_is_supported<traits>(0)) {
      if constexpr (std::is_void_v<typename traits::context_type>) {
        (void)ctx;
        return traits::is_supported(alg);
      } else {
        return traits::is_supported(
            value_ref<typename traits::context_type>(*static_cast<typename traits::context_type *>(ctx)), alg);
      }
    } else {
      (void)ctx;
      (void)alg;
      return true;
    }
  }

  template <typename Tag> static constexpr vtable s_vtbl{&start_entry<Tag>, &is_supported_entry<Tag>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace structo::crypto
