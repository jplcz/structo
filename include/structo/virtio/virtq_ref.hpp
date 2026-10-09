// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file virtq_ref.hpp
 * @brief `virtq_driver_ref<BufSpace>` / `virtq_device_ref<BufSpace>`:
 * type-erased, non-owning handles over a split *or* packed virtqueue end, so
 * a device front-end (virtio-blk, virtio-net, ...) is written once against the
 * ring-independent API and works with whichever ring layout was negotiated
 * (`VIRTIO_F_RING_PACKED`).
 *
 * Only `BufSpace` (what descriptor addresses mean) is part of the handle type;
 * the ring layout, ring space, memory backend and barrier policy are erased.
 *
 * ## Customization point: `virtq_driver_traits<Driver>` / `virtq_device_traits<Device>`
 *
 * The primary templates forward to the member functions of
 * `split_virtq_driver`/`packed_virtq_driver` (resp. `*_device`), so those bind
 * with no further work. A custom queue implementation either provides the same
 * members or specializes the traits:
 *
 * @code
 * template <> struct structo::virtio::virtq_driver_traits<my_queue> {
 *   using sg_type = structo::sg_entry<my_buf_space, std::uint64_t>;
 *   static reloco::result<void> try_add(my_queue &, reloco::span<const sg_type> out,
 *                                       reloco::span<const sg_type> in, std::uintptr_t token) noexcept;
 *   static reloco::result<void> try_publish(my_queue &) noexcept;  // optional; no-op if absent
 *   static reloco::result<bool> needs_notify(my_queue &) noexcept;
 *   static reloco::result<reloco::optional<virtq_completion>> try_get_used(my_queue &) noexcept;
 *   static reloco::result<void> try_set_interrupts_enabled(my_queue &, bool) noexcept;
 *   static std::uint32_t free_descriptors(const my_queue &) noexcept;
 *   static std::uint32_t queue_size(const my_queue &) noexcept;
 *   static bool is_broken(const my_queue &) noexcept;
 * };
 * @endcode
 *
 * Indirect descriptors and EVENT_IDX tuning are ring-specific and stay on the
 * concrete types; the handles expose the common request/completion path.
 *
 * ## Example
 *
 * @code
 * void submit(structo::virtio::virtq_driver_ref<buf_space> q, ...) {
 *   (void)q.try_add(out, in, token);
 *   (void)q.try_publish();                       // no-op on a packed ring
 *   if (auto n = q.needs_notify(); n && *n) ring_doorbell();
 * }
 * @endcode
 */

#include "packed_ring.hpp"
#include "split_ring.hpp"
#include "virtq_chain.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <structo/sg_list.hpp>
#include <type_traits>

namespace structo::virtio {

namespace detail {
template <typename Driver, typename = void> struct driver_has_publish : std::false_type {};
template <typename Driver>
struct driver_has_publish<Driver, std::void_t<decltype(std::declval<Driver &>().try_publish())>> : std::true_type {};
} // namespace detail

/** @brief Customization point for `virtq_driver_ref`; the primary template forwards to the members. */
template <typename Driver> struct virtq_driver_traits {
  using sg_type = typename Driver::sg_type;

  static reloco::result<void> try_add(Driver &d, reloco::span<const sg_type> out, reloco::span<const sg_type> in,
                                      std::uintptr_t token) noexcept {
    return d.try_add(out, in, token);
  }
  static reloco::result<void> try_publish(Driver &d) noexcept {
    if constexpr (detail::driver_has_publish<Driver>::value)
      return d.try_publish();
    else
      return {};
  }
  static reloco::result<bool> needs_notify(Driver &d) noexcept { return d.needs_notify(); }
  static reloco::result<reloco::optional<virtq_completion>> try_get_used(Driver &d) noexcept {
    return d.try_get_used();
  }
  static reloco::result<void> try_set_interrupts_enabled(Driver &d, bool on) noexcept {
    return d.try_set_interrupts_enabled(on);
  }
  static std::uint32_t free_descriptors(const Driver &d) noexcept { return d.free_descriptors(); }
  static std::uint32_t queue_size(const Driver &d) noexcept { return d.queue_size(); }
  static bool is_broken(const Driver &d) noexcept { return d.is_broken(); }
};

/** @brief Customization point for `virtq_device_ref`; the primary template forwards to the members. */
template <typename Device> struct virtq_device_traits {
  using segment = typename Device::segment;
  using chain = typename Device::chain;

  static reloco::result<reloco::optional<chain>> try_pop(Device &d, reloco::span<segment> storage) noexcept {
    return d.try_pop(storage);
  }
  static reloco::result<void> try_push_used(Device &d, const chain &c, std::uint32_t written) noexcept {
    return d.try_push_used(c, written);
  }
  static reloco::result<bool> should_interrupt(Device &d) noexcept { return d.should_interrupt(); }
  static reloco::result<void> try_set_notify_enabled(Device &d, bool on) noexcept {
    return d.try_set_notify_enabled(on);
  }
  static std::uint32_t queue_size(const Device &d) noexcept { return d.queue_size(); }
  static bool is_broken(const Device &d) noexcept { return d.is_broken(); }
};

namespace detail {
template <typename Driver, typename = void> struct has_virtq_driver_traits : std::false_type {};
template <typename Driver>
struct has_virtq_driver_traits<Driver, std::void_t<typename virtq_driver_traits<Driver>::sg_type,
                                                   decltype(virtq_driver_traits<Driver>::try_get_used)>>
    : std::true_type {};
template <typename Device, typename = void> struct has_virtq_device_traits : std::false_type {};
template <typename Device>
struct has_virtq_device_traits<
    Device, std::void_t<typename virtq_device_traits<Device>::chain, decltype(virtq_device_traits<Device>::try_pop)>>
    : std::true_type {};
} // namespace detail

// ============================================================================
// Driver handle
// ============================================================================

/** @brief Type-erased driver end. Unbound refs fail with `error::unsupported_operation`. */
template <typename BufSpace> class RELOCO_POINTER virtq_driver_ref {
public:
  using sg_type = sg_entry<BufSpace, std::uint64_t>;
  using completion = virtq_completion;

  struct vtable {
    reloco::result<void> (*add)(void *, reloco::span<const sg_type>, reloco::span<const sg_type>,
                                std::uintptr_t) noexcept;
    reloco::result<void> (*publish)(void *) noexcept;
    reloco::result<bool> (*needs_notify)(void *) noexcept;
    reloco::result<reloco::optional<completion>> (*get_used)(void *) noexcept;
    reloco::result<void> (*set_interrupts)(void *, bool) noexcept;
    std::uint32_t (*free_descriptors)(const void *) noexcept;
    std::uint32_t (*queue_size)(const void *) noexcept;
    bool (*is_broken)(const void *) noexcept;
  };

  constexpr virtq_driver_ref() noexcept = default;

  // The handle's own type is excluded so copying a (non-const) handle never re-binds to the handle itself.
  template <typename Driver,
            std::enable_if_t<!std::is_same_v<std::remove_cv_t<Driver>, virtq_driver_ref> &&
                                 detail::has_virtq_driver_traits<Driver>::value &&
                                 std::is_same_v<typename virtq_driver_traits<Driver>::sg_type, sg_type>,
                             int> = 0>
  constexpr explicit virtq_driver_ref(Driver &d RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(d)), vtbl_(&s_vtbl<Driver>) {}

  template <typename Driver, std::enable_if_t<!std::is_lvalue_reference_v<Driver>, int> = 0>
  virtq_driver_ref(Driver &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  [[nodiscard]] reloco::result<void> try_add(reloco::span<const sg_type> out, reloco::span<const sg_type> in,
                                             std::uintptr_t token) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->add(ctx_, out, in, token);
  }
  /** @brief Makes staged requests visible (split ring); a no-op for rings that publish in `try_add`. */
  [[nodiscard]] reloco::result<void> try_publish() const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->publish(ctx_);
  }
  [[nodiscard]] reloco::result<bool> needs_notify() const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->needs_notify(ctx_);
  }
  [[nodiscard]] reloco::result<reloco::optional<completion>> try_get_used() const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->get_used(ctx_);
  }
  [[nodiscard]] reloco::result<void> try_set_interrupts_enabled(bool enabled) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->set_interrupts(ctx_, enabled);
  }
  /** @brief `0` if unbound. */
  [[nodiscard]] std::uint32_t free_descriptors() const noexcept { return vtbl_ ? vtbl_->free_descriptors(ctx_) : 0; }
  /** @brief `0` if unbound. */
  [[nodiscard]] std::uint32_t queue_size() const noexcept { return vtbl_ ? vtbl_->queue_size(ctx_) : 0; }
  /** @brief `true` if unbound. */
  [[nodiscard]] bool is_broken() const noexcept { return vtbl_ ? vtbl_->is_broken(ctx_) : true; }

private:
  template <typename Driver>
  static constexpr vtable s_vtbl = {
      [](void *c, reloco::span<const sg_type> o, reloco::span<const sg_type> i, std::uintptr_t t) noexcept {
        return virtq_driver_traits<Driver>::try_add(*static_cast<Driver *>(c), o, i, t);
      },
      [](void *c) noexcept { return virtq_driver_traits<Driver>::try_publish(*static_cast<Driver *>(c)); },
      [](void *c) noexcept { return virtq_driver_traits<Driver>::needs_notify(*static_cast<Driver *>(c)); },
      [](void *c) noexcept { return virtq_driver_traits<Driver>::try_get_used(*static_cast<Driver *>(c)); },
      [](void *c, bool on) noexcept {
        return virtq_driver_traits<Driver>::try_set_interrupts_enabled(*static_cast<Driver *>(c), on);
      },
      [](const void *c) noexcept {
        return virtq_driver_traits<Driver>::free_descriptors(*static_cast<const Driver *>(c));
      },
      [](const void *c) noexcept { return virtq_driver_traits<Driver>::queue_size(*static_cast<const Driver *>(c)); },
      [](const void *c) noexcept { return virtq_driver_traits<Driver>::is_broken(*static_cast<const Driver *>(c)); },
  };

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

// ============================================================================
// Device handle
// ============================================================================

/** @brief Type-erased device end. Unbound refs fail with `error::unsupported_operation`. */
template <typename BufSpace> class RELOCO_POINTER virtq_device_ref {
public:
  using segment = chain_segment<BufSpace>;
  using chain = avail_chain<BufSpace>;

  struct vtable {
    reloco::result<reloco::optional<chain>> (*pop)(void *, reloco::span<segment>) noexcept;
    reloco::result<void> (*push_used)(void *, const chain &, std::uint32_t) noexcept;
    reloco::result<bool> (*should_interrupt)(void *) noexcept;
    reloco::result<void> (*set_notify)(void *, bool) noexcept;
    std::uint32_t (*queue_size)(const void *) noexcept;
    bool (*is_broken)(const void *) noexcept;
  };

  constexpr virtq_device_ref() noexcept = default;

  template <typename Device, std::enable_if_t<!std::is_same_v<std::remove_cv_t<Device>, virtq_device_ref> &&
                                                  detail::has_virtq_device_traits<Device>::value &&
                                                  std::is_same_v<typename virtq_device_traits<Device>::chain, chain>,
                                              int> = 0>
  constexpr explicit virtq_device_ref(Device &d RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(d)), vtbl_(&s_vtbl<Device>) {}

  template <typename Device, std::enable_if_t<!std::is_lvalue_reference_v<Device>, int> = 0>
  virtq_device_ref(Device &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  [[nodiscard]] reloco::result<reloco::optional<chain>> try_pop(reloco::span<segment> storage) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->pop(ctx_, storage);
  }
  [[nodiscard]] reloco::result<void> try_push_used(const chain &c, std::uint32_t written) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->push_used(ctx_, c, written);
  }
  [[nodiscard]] reloco::result<bool> should_interrupt() const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->should_interrupt(ctx_);
  }
  [[nodiscard]] reloco::result<void> try_set_notify_enabled(bool enabled) const noexcept {
    if (!vtbl_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    return vtbl_->set_notify(ctx_, enabled);
  }
  /** @brief `0` if unbound. */
  [[nodiscard]] std::uint32_t queue_size() const noexcept { return vtbl_ ? vtbl_->queue_size(ctx_) : 0; }
  /** @brief `true` if unbound. */
  [[nodiscard]] bool is_broken() const noexcept { return vtbl_ ? vtbl_->is_broken(ctx_) : true; }

private:
  template <typename Device>
  static constexpr vtable s_vtbl = {
      [](void *c, reloco::span<segment> s) noexcept {
        return virtq_device_traits<Device>::try_pop(*static_cast<Device *>(c), s);
      },
      [](void *c, const chain &ch, std::uint32_t w) noexcept {
        return virtq_device_traits<Device>::try_push_used(*static_cast<Device *>(c), ch, w);
      },
      [](void *c) noexcept { return virtq_device_traits<Device>::should_interrupt(*static_cast<Device *>(c)); },
      [](void *c, bool on) noexcept {
        return virtq_device_traits<Device>::try_set_notify_enabled(*static_cast<Device *>(c), on);
      },
      [](const void *c) noexcept { return virtq_device_traits<Device>::queue_size(*static_cast<const Device *>(c)); },
      [](const void *c) noexcept { return virtq_device_traits<Device>::is_broken(*static_cast<const Device *>(c)); },
  };

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

} // namespace structo::virtio
