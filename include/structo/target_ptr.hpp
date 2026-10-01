// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file target_ptr.hpp
 * @brief `structo::target_ptr<T, SpaceTag, PtrType>`: a typed pointer into
 * some *other* virtual address space -- user space, kernel space, a Realm,
 * a guest VM, ... -- that can only be dereferenced through an explicit,
 * fallible copy, never through the host CPU's own load/store instructions.
 *
 * This is the virtual-address-space counterpart to `phys_addr`/`dmap_ptr`
 * (`phys_addr.hpp`): `dmap_ptr` is for physical addresses that *are*
 * directly accessible once translated through a direct map, while
 * `target_ptr` is for addresses that are never directly accessible at all
 * from the code holding the handle -- the classic syscall-handler shape,
 * where a kernel thread receives a `void *` argument that is only
 * meaningful in the calling *user* process's own page tables, or a
 * hypervisor VMM receives a guest-virtual pointer that is only meaningful
 * once walked through the guest's own (Stage-1) page tables.
 *
 * ## "Current" context only
 *
 * A `target_ptr` carries no handle to the address space it names -- no
 * process ID, no `vmid`/VCPU reference, nothing. It is only ever valid
 * for whatever "current" context the CPU happens to be running in at the
 * moment a `try_materialize*`/`try_store*` call is made: exactly the Linux
 * `copy_from_user`/`copy_to_user` or FreeBSD `copyin`/`copyout` contract,
 * where the implicit, ambient "current address space" is the one
 * `%cr3`/`TTBR0_EL1`/... already points at. A syscall handler reads a
 * `target_ptr<T, user_space>` argument *while still running on behalf of
 * the calling thread*; stash the raw `raw_address()` integer and come back
 * to it after a context switch and it names something else entirely (or
 * nothing). `target_ptr_space_traits<SpaceTag>::try_read`/`try_write` are
 * the one hook this header leaves to the embedder for however "current" is
 * actually resolved on a given kernel/hypervisor (reading `%cr3`,
 * `curthread`, the active VCPU, ...).
 *
 * ## Why there is no `cast_space()`
 *
 * `phys_addr::cast_space()` exists (guarded by `RELOCO_UNSAFE_BUFFER_USAGE`)
 * because two physical address spaces can coincide (e.g. an
 * identity-mapped IOMMU), so reinterpreting the same integer in a
 * different space tag is sometimes exactly what the caller wants.
 * `target_ptr` has no such escape hatch, not even an unsafe one: a user
 * address and a kernel address are both small integers but name entries
 * in *different page tables*, so the same bit pattern essentially never
 * means the same byte of physical memory in two different
 * `target_ptr` spaces. There is nothing to cast -- only a real
 * translation (performed by the kernel's own address-space/VMA/page-table
 * machinery) can turn a `target_ptr<T, user_space>` into anything
 * meaningful in `kernel_space`, and that translation is squarely outside
 * what a header-only type can express.
 *
 * ## `_nofault` accessors
 *
 * Every materialize/store operation comes in two flavors:
 *
 * - The plain accessor (`try_materialize`, `try_store`, ...) may block
 *   resolving a page fault against the target address space (demand
 *   paging, swapped-out pages, lazily-populated guest memory, ...),
 *   exactly like an ordinary `copy_from_user`/`copy_to_user`.
 * - The `_nofault` accessor (`try_materialize_nofault`,
 *   `try_store_nofault`, ...) forbids that: if servicing the access would
 *   require taking a page fault, it fails immediately with
 *   `reloco::error::page_fault` instead of resolving it. This is the
 *   `copyin_nofault`/`copyout_nofault` (FreeBSD) or
 *   `pagefault_disable()`-wrapped `__get_user`/`__put_user` (Linux) shape:
 *   interrupt handlers, code already holding a spinlock, NMI/machine-check
 *   context, and anywhere else sleeping to page something in would be
 *   unsound must use these instead.
 *
 * Both flavors route through the same `target_ptr_space_traits<SpaceTag>`
 * specialization (`try_read`/`try_read_nofault`, `try_write`/
 * `try_write_nofault`); see its own doc comment below for the full
 * contract a specialization must satisfy, including the optional
 * `min_value`/`max_value` valid-range bounds.
 *
 * ## Rust-flavored pointer arithmetic
 *
 * Unlike a raw `T *`, `target_ptr` never silently wraps or invokes
 * undefined behavior on overflow: `operator+`/`operator-` are not
 * provided at all. Callers instead pick the explicit Rust `checked_add`/
 * `wrapping_add`/`saturating_add` family (and their `_sub` counterparts),
 * matching `<reloco/int_ops.hpp>`'s integer equivalents and Rust's own
 * `<*const T>::checked_add`/`wrapping_add`/... -- the caller states up
 * front what should happen on overflow instead of it being whatever the
 * compiler's optimizer decided was convenient for a well-defined program.
 */

#include <reloco/bytes.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/detail/assert.hpp>
#include <reloco/detail/compat.hpp>
#include <reloco/error.hpp>
#include <reloco/int_ops.hpp>
#include <reloco/span.hpp>
#include <reloco/unique_ptr.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace structo {

using namespace reloco;

// ============================================================================
// Target Virtual Address Space Tags
// ============================================================================

/** @brief A userland process's own virtual address space. */
struct user_space {};

/** @brief The kernel's own virtual address space. */
struct kernel_space {};

/** @brief A guest VM's virtual address space (post Stage-1, pre Stage-2). */
struct guest_vm_space {};

/** @brief An Arm CCA Realm's virtual address space (RL-PAS-backed). */
struct realm_space {};

/** @brief An Arm TrustZone Secure World's virtual address space (S-EL1/S-EL0). */
struct secure_world_space {};

// ============================================================================
// Pointer Integer Traits
// ============================================================================

/**
 * @brief Integer representation traits for a `target_ptr`'s raw address.
 * @tparam PtrType The integer type used to store the address.
 */
template <typename PtrType> struct ptr_type_traits {
  using address_type = PtrType;
  static constexpr address_type null_value = PtrType(0);
};

// ============================================================================
// Target Address Space Access Traits
// ============================================================================

/**
 * @brief Per-`SpaceTag` hook `target_ptr` routes every materialize/store
 * operation through. Deliberately left undefined by default (matching
 * `phys_addr.hpp`'s `dmap_mapper`/microfmt's `address_space_traits<Tag>`
 * pattern): a `target_ptr<T, SpaceTag>` only becomes usable for anything
 * beyond holding/comparing/casting a raw address once the embedder
 * provides a full specialization for `SpaceTag`, e.g.:
 *
 * @code
 * template <> struct structo::target_ptr_space_traits<my_user_space_tag> {
 *   using address_type = std::uintptr_t;
 *
 *   // Optional: the valid address range for this space (e.g. a process's
 *   // TASK_SIZE on a given architecture). Omitted entirely if the whole
 *   // address_type range is potentially valid.
 *   static constexpr address_type min_value = 0x1000; // first page is never mapped
 *   static constexpr address_type max_value = 0x0000'7FFF'FFFF'FFFFull;
 *
 *   // May block resolving a page fault (demand paging/swap-in) against
 *   // the *current* task's address space.
 *   static reloco::result<void> try_read(address_type addr, reloco::span<std::byte> dst) noexcept;
 *   static reloco::result<void> try_write(address_type addr, reloco::span<const std::byte> src) noexcept;
 *
 *   // Must never block on a page fault: fail with reloco::error::page_fault
 *   // instead of resolving one (interrupt/NMI/spinlock-held context).
 *   static reloco::result<void> try_read_nofault(address_type addr, reloco::span<std::byte> dst) noexcept;
 *   static reloco::result<void> try_write_nofault(address_type addr, reloco::span<const std::byte> src) noexcept;
 * };
 * @endcode
 *
 * All four functions operate against whatever address space is "current"
 * at the time of the call (see the file-level doc comment) -- none of
 * them take an explicit process/VCPU handle.
 */
template <typename SpaceTag> struct target_ptr_space_traits;

namespace detail {

// Detects an optional `Traits::min_value`/`Traits::max_value` bound,
// letting target_ptr_space_traits<SpaceTag> specializations opt into
// range validation without forcing every specialization to declare it.
template <typename Traits, typename = void> struct target_ptr_has_min_value : std::false_type {};
template <typename Traits>
struct target_ptr_has_min_value<Traits, std::void_t<decltype(Traits::min_value)>> : std::true_type {};

template <typename Traits, typename = void> struct target_ptr_has_max_value : std::false_type {};
template <typename Traits>
struct target_ptr_has_max_value<Traits, std::void_t<decltype(Traits::max_value)>> : std::true_type {};

// void has no object representation to read/write a byte span into/out
// of; every other (required trivially copyable) T steps by sizeof(T).
template <typename T> struct target_ptr_element_size {
  static constexpr std::size_t value = sizeof(T);
};
template <> struct target_ptr_element_size<void> {
  static constexpr std::size_t value = 1;
};

// Reinterprets a (mutable) span<T> as a writable byte span, the
// materialize-side counterpart to reloco::span<T>::as_bytes() (which only
// ever returns a read-only span<const std::byte>, regardless of T's own
// constness). Single checked boundary, matching phys_addr.hpp's own
// reinterpret_cast call sites.
template <typename T> [[nodiscard]] span<std::byte> as_writable_byte_span(span<T> s) noexcept {
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  return span<std::byte>(reinterpret_cast<std::byte *>(s.data()), s.size() * sizeof(T));
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

} // namespace detail

// ============================================================================
// Typed Target Pointer
// ============================================================================

/**
 * @brief Strong type for a pointer into another virtual address space.
 *
 * @tparam T The type this pointer points to (`void` is supported for
 * type-erased handles, but disables every materialize/store accessor --
 * cast to a concrete type first via `cast_type`).
 * @tparam SpaceTag The address-space domain (`user_space`, `kernel_space`,
 * `guest_vm_space`, `realm_space`, `secure_world_space`, or a
 * caller-defined tag), preventing a `user_space` pointer from being mixed
 * up with a `kernel_space` one at compile time.
 * @tparam PtrType The integer type used to store the address (default:
 * `std::uintptr_t`).
 */
template <typename T, typename SpaceTag, typename PtrType = std::uintptr_t> class target_ptr {
  static_assert(std::is_void_v<T> || std::is_trivially_copyable_v<T>,
                "target_ptr<T, ...> requires T to be void or trivially copyable: "
                "materialize/store memcpy T's raw bytes across a trust boundary, "
                "so T can never run its own constructor/destructor on that side");

public:
  using ptr_traits = ptr_type_traits<PtrType>;
  using address_type = PtrType;
  using value_type = T;
  using space_tag = SpaceTag;
  using space_traits = target_ptr_space_traits<SpaceTag>;
  using difference_type = std::make_signed_t<PtrType>;

  /** @brief Byte stride of one `T` element (`1` for `T = void`). */
  static constexpr std::size_t element_size = detail::target_ptr_element_size<T>::value;

  constexpr target_ptr() noexcept = default;
  constexpr target_ptr(std::nullptr_t) noexcept {}
  constexpr explicit target_ptr(address_type val) noexcept : value_(val) {}

  [[nodiscard]] constexpr bool is_null() const noexcept { return value_ == ptr_traits::null_value; }
  constexpr explicit operator bool() const noexcept { return !is_null(); }

  [[nodiscard]] constexpr address_type raw_address() const noexcept { return value_; }

  /**
   * @brief Whether the raw address falls within `space_traits`'
   * optional `min_value`/`max_value` bounds. Always `true` if
   * `space_traits` declares neither bound.
   */
  [[nodiscard]] constexpr bool is_in_range() const noexcept {
    if constexpr (detail::target_ptr_has_min_value<space_traits>::value) {
      if (value_ < space_traits::min_value)
        return false;
    }
    if constexpr (detail::target_ptr_has_max_value<space_traits>::value) {
      if (value_ > space_traits::max_value)
        return false;
    }
    return true;
  }

  /**
   * @brief Reinterprets the pointed-to type, staying in the same
   * `SpaceTag`. There is deliberately no `cast_space()` -- see the
   * file-level doc comment for why crossing address spaces can never be
   * a bare reinterpretation.
   */
  template <typename U> [[nodiscard]] constexpr target_ptr<U, SpaceTag, PtrType> cast_type() const noexcept {
    return target_ptr<U, SpaceTag, PtrType>{value_};
  }

  // --------------------------------------------------------------------
  // Comparisons. Only ever defined between two target_ptr instantiated
  // with the *same* T/SpaceTag/PtrType -- there is no cross-space
  // overload to accidentally call, so mixing e.g. user_space and
  // kernel_space pointers is a compile error, not a logic bug.
  // --------------------------------------------------------------------

  [[nodiscard]] friend constexpr bool operator==(const target_ptr &a, const target_ptr &b) noexcept {
    return a.value_ == b.value_;
  }
  [[nodiscard]] friend constexpr bool operator!=(const target_ptr &a, const target_ptr &b) noexcept {
    return a.value_ != b.value_;
  }
  [[nodiscard]] friend constexpr bool operator<(const target_ptr &a, const target_ptr &b) noexcept {
    return a.value_ < b.value_;
  }
  [[nodiscard]] friend constexpr bool operator<=(const target_ptr &a, const target_ptr &b) noexcept {
    return a.value_ <= b.value_;
  }
  [[nodiscard]] friend constexpr bool operator>(const target_ptr &a, const target_ptr &b) noexcept {
    return a.value_ > b.value_;
  }
  [[nodiscard]] friend constexpr bool operator>=(const target_ptr &a, const target_ptr &b) noexcept {
    return a.value_ >= b.value_;
  }

  // --------------------------------------------------------------------
  // Rust-flavored checked/wrapping/saturating pointer arithmetic.
  // --------------------------------------------------------------------

  /**
   * @brief Advances by `n` elements, failing with
   * `reloco::error::integer_overflow` instead of wrapping if either the
   * `n * element_size` byte offset or the resulting address would
   * overflow. Matches Rust's `<*const T>::checked_add`/`checked_sub`
   * (`n` may be negative, matching `offset`'s signed `isize`).
   */
  [[nodiscard]] result<target_ptr> checked_add(difference_type n) const noexcept {
    auto byte_offset = reloco::checked_mul<difference_type>(n, static_cast<difference_type>(element_size));
    if (!byte_offset)
      return unexpected(byte_offset.error());
    if (byte_offset.value() >= 0) {
      auto sum = reloco::checked_add<address_type>(value_, static_cast<address_type>(byte_offset.value()));
      if (!sum)
        return unexpected(sum.error());
      return target_ptr{sum.value()};
    }
    auto magnitude = reloco::checked_neg(byte_offset.value());
    if (!magnitude)
      return unexpected(magnitude.error());
    auto diff = reloco::checked_sub<address_type>(value_, static_cast<address_type>(magnitude.value()));
    if (!diff)
      return unexpected(diff.error());
    return target_ptr{diff.value()};
  }

  /** @copydoc checked_add */
  [[nodiscard]] result<target_ptr> checked_sub(difference_type n) const noexcept {
    auto neg_n = reloco::checked_neg(n);
    if (!neg_n)
      return unexpected(neg_n.error());
    return checked_add(neg_n.value());
  }

  /**
   * @brief Advances by `n` elements with well-defined modulo-2^N
   * wraparound, matching Rust's `wrapping_add`/`wrapping_sub`.
   */
  [[nodiscard]] constexpr target_ptr wrapping_add(difference_type n) const noexcept {
    auto byte_offset =
        static_cast<address_type>(reloco::wrapping_mul<difference_type>(n, static_cast<difference_type>(element_size)));
    return target_ptr{reloco::wrapping_add<address_type>(value_, byte_offset)};
  }

  /** @copydoc wrapping_add */
  [[nodiscard]] constexpr target_ptr wrapping_sub(difference_type n) const noexcept {
    auto byte_offset =
        static_cast<address_type>(reloco::wrapping_mul<difference_type>(n, static_cast<difference_type>(element_size)));
    return target_ptr{reloco::wrapping_sub<address_type>(value_, byte_offset)};
  }

  /**
   * @brief Advances by `n` elements, clamping to the representable
   * `address_type` range instead of overflowing, matching Rust's
   * `saturating_add`/`saturating_sub`.
   */
  [[nodiscard]] target_ptr saturating_add(difference_type n) const noexcept {
    auto res = checked_add(n);
    if (res)
      return res.value();
    return target_ptr{static_cast<address_type>(n >= 0 ? ~address_type(0) : address_type(0))};
  }

  /** @copydoc saturating_add */
  [[nodiscard]] target_ptr saturating_sub(difference_type n) const noexcept {
    auto res = checked_sub(n);
    if (res)
      return res.value();
    return target_ptr{static_cast<address_type>(n >= 0 ? address_type(0) : ~address_type(0))};
  }

  /**
   * @brief Element-wise distance from `origin` to `*this`, failing with
   * `reloco::error::invalid_argument` if the byte distance is not an
   * exact multiple of `element_size`, or `reloco::error::integer_overflow`
   * if it doesn't fit in `difference_type`. Matches Rust's
   * `<*const T>::offset_from`.
   */
  [[nodiscard]] result<difference_type> checked_offset_from(const target_ptr &origin) const noexcept {
    const bool negative = value_ < origin.value_;
    const address_type byte_diff = negative ? (origin.value_ - value_) : (value_ - origin.value_);
    if (byte_diff % element_size != 0)
      return unexpected(error::invalid_argument);
    auto scaled = reloco::checked_cast<difference_type>(byte_diff / element_size);
    if (!scaled)
      return unexpected(scaled.error());
    if (!negative)
      return scaled.value();
    return reloco::checked_neg(scaled.value());
  }

  // --------------------------------------------------------------------
  // Materialize / store. Disabled entirely for T = void (cast_type<U>()
  // to a concrete type first). Every overload comes in a plain and a
  // "_nofault" flavor -- see the file-level doc comment.
  // --------------------------------------------------------------------

  /** @brief Reads `buf.size()` elements, may fault in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<void> try_materialize_to(span<U> buf) const noexcept {
    return materialize_bytes<false>(detail::as_writable_byte_span(buf));
  }

  /** @brief Like `try_materialize_to`, but never faults in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<void> try_materialize_to_nofault(span<U> buf) const noexcept {
    return materialize_bytes<true>(detail::as_writable_byte_span(buf));
  }

  /** @brief Writes `buf.size()` elements, may fault in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<void> try_store(span<const U> buf) const noexcept {
    return store_bytes<false>(buf.as_bytes());
  }

  /** @brief Like `try_store`, but never faults in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<void> try_store_nofault(span<const U> buf) const noexcept {
    return store_bytes<true>(buf.as_bytes());
  }

  /** @brief Convenience single-element overload of `try_store`. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<void> try_store(const U &val) const noexcept {
    return try_store(span<const U>(&val, 1));
  }

  /** @brief Convenience single-element overload of `try_store_nofault`. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<void> try_store_nofault(const U &val) const noexcept {
    return try_store_nofault(span<const U>(&val, 1));
  }

  /** @brief Reads and returns a single `T` by value, may fault in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>> result<U> try_materialize() const noexcept {
    return with_scratch_buffer(sizeof(U), [this](span<std::byte> storage) -> result<U> {
      if (auto res = materialize_bytes<false>(storage); !res)
        return unexpected(res.error());
      U out;
      std::memcpy(&out, storage.data(), sizeof(U));
      return out;
    });
  }

  /** @brief Like `try_materialize`, but never faults in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<U> try_materialize_nofault() const noexcept {
    return with_scratch_buffer(sizeof(U), [this](span<std::byte> storage) -> result<U> {
      if (auto res = materialize_bytes<true>(storage); !res)
        return unexpected(res.error());
      U out;
      std::memcpy(&out, storage.data(), sizeof(U));
      return out;
    });
  }

  /** @brief Allocates a `U` on the heap and reads into it, may fault in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<unique_ptr<U>> try_materialize_ptr(allocator_ref alloc = default_allocator()) const noexcept {
    static_assert(std::is_default_constructible_v<U>, "try_materialize_ptr requires a default-constructible T");
    auto box = unique_ptr<U>::try_allocate(alloc);
    if (!box)
      return unexpected(box.error());
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    auto res = materialize_bytes<false>(span<std::byte>(reinterpret_cast<std::byte *>(box.value().get()), sizeof(U)));
    RELOCO_END_UNSAFE_BUFFER_USAGE
    if (!res)
      return unexpected(res.error());
    return std::move(box.value());
  }

  /** @brief Like `try_materialize_ptr`, but never faults in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<unique_ptr<U>> try_materialize_ptr_nofault(allocator_ref alloc = default_allocator()) const noexcept {
    static_assert(std::is_default_constructible_v<U>, "try_materialize_ptr_nofault requires a default-constructible T");
    auto box = unique_ptr<U>::try_allocate(alloc);
    if (!box)
      return unexpected(box.error());
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    auto res = materialize_bytes<true>(span<std::byte>(reinterpret_cast<std::byte *>(box.value().get()), sizeof(U)));
    RELOCO_END_UNSAFE_BUFFER_USAGE
    if (!res)
      return unexpected(res.error());
    return std::move(box.value());
  }

  /** @brief Copies `sizeof(T)` bytes into a fresh, immutable `bytes` snapshot. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<bytes> try_as_bytes(allocator_ref alloc = default_allocator()) const noexcept {
    return with_scratch_buffer(sizeof(U), [this, alloc](span<std::byte> storage) -> result<bytes> {
      if (auto res = materialize_bytes<false>(storage); !res)
        return unexpected(res.error());
      return bytes::try_copy_from(span<const std::byte>(storage.data(), storage.size()), alloc);
    });
  }

  /** @brief Like `try_as_bytes`, but never faults in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<bytes> try_as_bytes_nofault(allocator_ref alloc = default_allocator()) const noexcept {
    return with_scratch_buffer(sizeof(U), [this, alloc](span<std::byte> storage) -> result<bytes> {
      if (auto res = materialize_bytes<true>(storage); !res)
        return unexpected(res.error());
      return bytes::try_copy_from(span<const std::byte>(storage.data(), storage.size()), alloc);
    });
  }

  /** @brief Copies `sizeof(T)` bytes into a fresh, mutable `bytes_mut` snapshot. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<bytes_mut> try_as_bytes_mut(allocator_ref alloc = default_allocator()) const noexcept {
    return as_bytes_mut_impl<U, false>(alloc);
  }

  /** @brief Like `try_as_bytes_mut`, but never faults in missing pages. */
  template <typename U = T, typename = std::enable_if_t<!std::is_void_v<U>>>
  result<bytes_mut> try_as_bytes_mut_nofault(allocator_ref alloc = default_allocator()) const noexcept {
    return as_bytes_mut_impl<U, true>(alloc);
  }

private:
  template <bool NoFault> result<void> materialize_bytes(span<std::byte> dst) const noexcept {
    if (is_null())
      return unexpected(error::invalid_argument);
    if (!is_in_range())
      return unexpected(error::out_of_range);
    if constexpr (NoFault) {
      return space_traits::try_read_nofault(value_, dst);
    } else {
      return space_traits::try_read(value_, dst);
    }
  }

  template <bool NoFault> result<void> store_bytes(span<const std::byte> src) const noexcept {
    if (is_null())
      return unexpected(error::invalid_argument);
    if (!is_in_range())
      return unexpected(error::out_of_range);
    if constexpr (NoFault) {
      return space_traits::try_write_nofault(value_, src);
    } else {
      return space_traits::try_write(value_, src);
    }
  }

  template <typename U, bool NoFault> result<bytes_mut> as_bytes_mut_impl(allocator_ref alloc) const noexcept {
    auto buf_res = bytes_mut::try_allocate(alloc, sizeof(U));
    if (!buf_res)
      return unexpected(buf_res.error());
    bytes_mut buf = std::move(buf_res.value());
    // bytes_mut exposes no direct "resize"; extend to sizeof(U) with a
    // placeholder zero-filled span (capacity was already reserved above,
    // so this never reallocates) and then overwrite every byte in place
    // below. The zero placeholder itself is produced by
    // `with_scratch_buffer`, so it only lives on the stack for "small"
    // U; a large, caller-controlled-size U instead gets a short-lived
    // heap zero buffer, keeping this call's stack frame bounded.
    auto extend_res = with_scratch_buffer(sizeof(U), [&buf](span<std::byte> zero_storage) -> result<void> {
      std::memset(zero_storage.data(), 0, zero_storage.size());
      return buf.try_put_slice(span<const std::byte>(zero_storage.data(), zero_storage.size()));
    });
    if (!extend_res)
      return unexpected(extend_res.error());
    if (auto res = materialize_bytes<NoFault>(buf.as_span()); !res)
      return unexpected(res.error());
    return buf;
  }

  /**
   * @brief Maximum size, in bytes, for which `with_scratch_buffer` uses an
   * on-stack buffer. Larger sizes fall back to a short-lived heap
   * allocation instead, so a caller-controlled-size `T` can never grow a
   * call's stack frame by an arbitrary amount.
   */
  static constexpr std::size_t stack_inline_threshold = 32;

  /**
   * @brief Provides @p fill_and_consume with a scratch `span<std::byte>` of
   * exactly @p size bytes, choosing the storage strategy at compile time:
   * "small" requests (`size <= stack_inline_threshold`) are served from an
   * on-stack buffer (the common case for syscall-sized arguments and
   * structs); anything larger is served from a temporary heap allocation
   * obtained via `default_allocator()` instead. @p fill_and_consume is
   * responsible for both populating the buffer and producing the final
   * `result<R>` to return.
   */
  template <typename F> auto with_scratch_buffer(std::size_t size, F &&fill_and_consume) const noexcept {
    if (size <= stack_inline_threshold) {
      std::byte storage[stack_inline_threshold];
      return fill_and_consume(span<std::byte>(storage, size));
    } else {
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
      auto block = default_allocator().allocate(size, alignof(std::max_align_t));
      RELOCO_END_UNSAFE_BUFFER_USAGE
      if (!block)
        return decltype(fill_and_consume(span<std::byte>{}))(unexpected(block.error()));
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
      auto *raw = reinterpret_cast<std::byte *>(block.value().ptr);
      RELOCO_END_UNSAFE_BUFFER_USAGE
      auto out = fill_and_consume(span<std::byte>(raw, size));
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
      default_allocator().deallocate(raw, size);
      RELOCO_END_UNSAFE_BUFFER_USAGE
      return out;
    }
  }

  address_type value_{ptr_traits::null_value};
};

static_assert(sizeof(target_ptr<int, user_space>) == sizeof(std::uintptr_t), "target_ptr must be zero-overhead");

} // namespace structo
