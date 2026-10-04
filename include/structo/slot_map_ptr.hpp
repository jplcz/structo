// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file slot_map_ptr.hpp
 * @brief `structo::slot_map_ptr<T, Mapper, PhysInt>` / `structo::slot_map_mapper<...>`:
 * the dynamically-remapped counterpart to `dmap_ptr`/`dmap_mapper`
 * (`phys_addr.hpp`), for targets where a permanent, whole-range direct map
 * is not available or not desirable -- e.g. a 32-bit system with far more
 * physical memory than spare virtual address space, or a TEE/secure-world
 * image that must only ever expose a small, on-demand window into
 * Non-secure (REE) RAM rather than mapping the entire Normal World address
 * space into Secure World page tables.
 *
 * Where `dmap_ptr::try_get()` returns a raw `T*` that stays valid for as
 * long as the (permanent) direct map itself is valid, `slot_map_ptr` has no
 * persistent virtual address at all: `try_map()` borrows one of a small,
 * fixed pool of virtual "slots", programs it (via `Mapper`) to point at
 * this pointer's physical address, and returns an RAII `slot_map_ptr::guard`
 * -- the only way to obtain a `T*`. The slot is released (unmapped,
 * TLB-invalidated) automatically when the guard is destroyed, so the
 * mapping's lifetime can never outlive the scope that requested it --
 * mirroring Linux's `kmap_atomic()`/`kunmap_atomic()` or FreeBSD's
 * `pmap_quick_enter_page()`/`pmap_quick_remove_page()`, expressed as a
 * move-only C++ RAII handle instead of a pair of free functions the caller
 * must remember to pair up correctly.
 *
 * ## `Mapper` contract
 *
 * `slot_map_ptr<T, Mapper, PhysInt>` requires `Mapper` to provide:
 * - `using space_tag = /``*`` a `phys_addr` SpaceTag ``*``/;`
 * - `struct mapped_slot { std::size_t index; void *vaddr; };`
 * - `template <typename T> static bool validate_phys(phys_addr<T, space_tag, PhysInt> p) noexcept;`
 * - `static result<mapped_slot> acquire(phys_addr<void, space_tag, PhysInt> phys, std::size_t size) noexcept;`
 * - `static void release(std::size_t slot) noexcept;`
 *
 * `structo::slot_map_mapper<SlotCount, ArchHooks, ExpectedSpace, PhysInt>` is
 * a ready-made `Mapper`: it implements the slot bookkeeping (lock-free
 * acquire/release over a fixed-size pool of `SlotCount` slots) on top of an
 * architecture-supplied `ArchHooks` policy, which must provide:
 * - `static constexpr std::size_t slot_size;` (bytes per slot; a page or more)
 * - `static void *slot_base(std::size_t slot) noexcept;` (fixed VA of a slot)
 * - `static result<void> program(std::size_t slot, PhysInt phys_aligned) noexcept;`
 *   (points `slot`'s fixed VA window at the `slot_size`-aligned physical
 *   page `phys_aligned`, performing whatever page-table write and TLB
 *   shootdown the architecture requires)
 * - `static void unprogram(std::size_t slot) noexcept;` (tears the mapping
 *   back down; must also be safe to call on an already-torn-down slot)
 *
 * @code
 * // Secure-world example: a handful of fixed VA windows used to peek into
 * // Non-secure (REE) RAM one page at a time, without ever mapping the
 * // entire NS address space into the Secure page tables.
 * struct ns_peek_hooks {
 *   static constexpr std::size_t slot_size = 4096;
 *
 *   static void *slot_base(std::size_t slot) noexcept {
 *     return reinterpret_cast<void *>(NS_PEEK_WINDOW_BASE + slot * slot_size);
 *   }
 *   static reloco::result<void> program(std::size_t slot, std::uint64_t phys_aligned) noexcept {
 *     return arch_map_ns_page(slot_base(slot), phys_aligned); // arch-specific PTE write + TLBI
 *   }
 *   static void unprogram(std::size_t slot) noexcept { arch_unmap_page(slot_base(slot)); }
 * };
 *
 * using ns_peek_mapper = structo::slot_map_mapper<4, ns_peek_hooks, structo::nonsecure_phys_space>;
 *
 * auto ptr = structo::slot_map_ptr<uint32_t, ns_peek_mapper>::from_paddr(ns_phys);
 * if (ptr) {
 *   auto guard = (*ptr).try_map(); // maps one of the 4 slots for this scope only
 *   if (guard) {
 *     uint32_t value = **guard;
 *   } // slot is unmapped here, even on early return/exception
 * }
 * @endcode
 */

#include "phys_addr.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <type_traits>
#include <utility>

namespace structo {

using namespace reloco;

// ============================================================================
// slot_map_mapper: architecture-agnostic slot-pool bookkeeping over ArchHooks
// ============================================================================

/**
 * @brief Ready-made `Mapper` for `slot_map_ptr`: a fixed-size pool of
 * `SlotCount` virtual-address slots, lock-free acquired/released by this
 * class, with the actual page-table programming delegated to `ArchHooks`.
 * See the @file-level docs for `ArchHooks`'s contract.
 */
template <std::size_t SlotCount, typename ArchHooks, typename ExpectedSpace = default_phys_space,
          typename PhysInt = std::uint64_t>
struct slot_map_mapper {
  static_assert(SlotCount > 0, "slot_map_mapper requires at least one slot");

  using space_tag = ExpectedSpace;

  /** @brief A slot borrowed from the pool, mapped to the requested physical range. */
  struct mapped_slot {
    std::size_t index;
    void *vaddr;
  };

  /** @brief No fixed window to bound-check against; `ArchHooks::program()` is authoritative. */
  template <typename T>
  [[nodiscard]] static bool validate_phys(phys_addr<T, ExpectedSpace, PhysInt> /*p*/) noexcept {
    return true;
  }

  /**
   * @brief Claims a free slot and programs it to cover `[phys, phys + size)`.
   * @return The claimed slot plus the mapped virtual pointer for `phys`
   * (i.e. already adjusted for `phys`'s offset within its aligned slot
   * page), or `error::out_of_range` if `size` cannot fit within one slot,
   * `error::busy` if every slot is currently in use, or `ArchHooks::program`'s
   * own error.
   */
  [[nodiscard]] static result<mapped_slot> acquire(phys_addr<void, ExpectedSpace, PhysInt> phys,
                                                    std::size_t size) noexcept {
    if (size == 0 || size > ArchHooks::slot_size)
      return unexpected(error::out_of_range);

    const PhysInt slot_mask = static_cast<PhysInt>(ArchHooks::slot_size) - 1;
    const PhysInt phys_aligned = static_cast<PhysInt>(phys.value & ~slot_mask);
    const std::size_t page_offset = static_cast<std::size_t>(phys.value & slot_mask);
    if (page_offset + size > ArchHooks::slot_size)
      return unexpected(error::out_of_range); // Crosses a slot-sized boundary.

    for (std::size_t i = 0; i < SlotCount; ++i) {
      bool expected = false;
      if (busy_table()[i].compare_exchange_strong(expected, true, std::memory_order_acquire)) {
        auto res = ArchHooks::program(i, phys_aligned);
        if (!res) {
          busy_table()[i].store(false, std::memory_order_release);
          return unexpected(res.error());
        }
        RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
        void *vaddr = static_cast<std::byte *>(ArchHooks::slot_base(i)) + page_offset;
        RELOCO_END_UNSAFE_BUFFER_USAGE
        return mapped_slot{i, vaddr};
      }
    }
    return unexpected(error::busy);
  }

  /** @brief Tears down and returns slot `slot` to the free pool. */
  static void release(std::size_t slot) noexcept {
    ArchHooks::unprogram(slot);
    busy_table()[slot].store(false, std::memory_order_release);
  }

private:
  // Function-local static: avoids a separate out-of-line definition for the
  // pool's storage while still giving every instantiation of this template
  // its own pool (one per distinct <SlotCount, ArchHooks, ExpectedSpace, PhysInt>).
  [[nodiscard]] static std::atomic<bool> *busy_table() noexcept {
    static std::atomic<bool> table[SlotCount]{};
    return table;
  }
};

// ============================================================================
// Smart Pointer for Scoped Dynamically-Remapped Access
// ============================================================================

/**
 * @brief Pointer-like, dynamically-remapped view of a physical address.
 *
 * Unlike `dmap_ptr`, this type never exposes a `T*` with unbounded
 * lifetime: `try_map()` returns an RAII `guard` that owns the underlying
 * slot for its own lifetime only. See the @file-level docs for the
 * rationale and `Mapper` contract.
 *
 * @tparam T Pointed-to type.
 * @tparam Mapper Dynamic slot-mapping policy (see `slot_map_mapper`).
 * @tparam PhysInt Physical-address integer type.
 */
template <typename T, typename Mapper, typename PhysInt = std::uint64_t> class slot_map_ptr {
public:
  using space_tag = typename Mapper::space_tag;
  using phys_type = phys_addr<T, space_tag, PhysInt>;

  constexpr slot_map_ptr() noexcept = default;
  constexpr slot_map_ptr(std::nullptr_t) noexcept {}

  [[nodiscard]] constexpr phys_type phys() const noexcept { return paddr_; }
  [[nodiscard]] constexpr bool is_null() const noexcept { return paddr_.is_null(); }
  constexpr explicit operator bool() const noexcept { return !paddr_.is_null(); }

  [[nodiscard]] static result<slot_map_ptr> from_paddr(phys_type phys) noexcept {
    if (phys.is_null()) {
      return slot_map_ptr{};
    }
    if (!Mapper::validate_phys(phys))
      return unexpected(error::security_violation);
    return slot_map_ptr(phys);
  }

  /**
   * @brief RAII handle to a live, dynamically-remapped slot.
   *
   * Move-only: exactly one `guard` owns a given slot at a time. The slot
   * is unmapped on destruction, on an explicit `reset()`, or when
   * move-assigned/moved-from (the moved-from guard becomes empty and its
   * destructor becomes a no-op).
   */
  class [[nodiscard]] RELOCO_POINTER guard {
  public:
    constexpr guard() noexcept = default;
    ~guard() noexcept { reset(); }

    guard(const guard &) = delete;
    guard &operator=(const guard &) = delete;

    guard(guard &&other) noexcept
        : m_slot(std::exchange(other.m_slot, invalid_slot)), m_ptr(std::exchange(other.m_ptr, nullptr)) {}

    guard &operator=(guard &&other) noexcept {
      if (this != &other) {
        reset();
        m_slot = std::exchange(other.m_slot, invalid_slot);
        m_ptr = std::exchange(other.m_ptr, nullptr);
      }
      return *this;
    }

    [[nodiscard]] constexpr bool is_null() const noexcept { return m_slot == invalid_slot; }
    constexpr explicit operator bool() const noexcept { return m_slot != invalid_slot; }

    [[nodiscard]] T *get() const noexcept RELOCO_LIFETIMEBOUND { return m_ptr; }
    [[nodiscard]] T *operator->() const noexcept RELOCO_LIFETIMEBOUND { return m_ptr; }
    // `std::add_lvalue_reference_t<T>` (not plain `T&`) so this declaration stays
    // well-formed for `T = void` (yielding `void`); the body is only instantiated
    // if actually called, mirroring `std::shared_ptr<void>::operator*()`.
    [[nodiscard]] std::add_lvalue_reference_t<T> operator*() const noexcept RELOCO_LIFETIMEBOUND { return *m_ptr; }

    /** @brief Early explicit unmap before scope exit. Idempotent. */
    void reset() noexcept {
      if (m_slot != invalid_slot) {
        Mapper::release(m_slot);
        m_slot = invalid_slot;
        m_ptr = nullptr;
      }
    }

  private:
    friend class slot_map_ptr;
    guard(std::size_t slot, T *ptr) noexcept : m_slot(slot), m_ptr(ptr) {}

    static constexpr std::size_t invalid_slot = ~std::size_t(0);
    std::size_t m_slot{invalid_slot};
    T *m_ptr{nullptr};
  };

  /**
   * @brief Borrows a free slot and maps it to `[phys(), phys() + size)` for
   * the lifetime of the returned `guard`.
   * @param size Byte span to map, starting at `phys()`. Must fit within a
   * single `Mapper` slot.
   */
  [[nodiscard]] result<guard> try_map(std::size_t size) const noexcept {
    if (is_null())
      return unexpected(error::invalid_argument);
    auto res = Mapper::acquire(paddr_.template cast_type<void>(), size);
    if (!res.has_value())
      return unexpected(res.error());
    return guard(res.value().index, static_cast<T *>(res.value().vaddr));
  }

  /** @brief `try_map(std::size_t)` overload defaulting `size` to `sizeof(T)`. */
  template <typename U = T> [[nodiscard]] result<guard> try_map() const noexcept {
    static_assert(!std::is_void_v<U>, "slot_map_ptr<void, ...>::try_map() requires an explicit size argument");
    return try_map(sizeof(U));
  }

private:
  constexpr explicit slot_map_ptr(phys_type v) : paddr_(v) {}

  phys_type paddr_{nullptr}; // Automatically uses the ~0 initialization
};

namespace detail {
// Never instantiated beyond this type-only probe: slot_map_mapper's
// space_tag alias (the only thing the static_assert below needs) does not
// require ArchHooks to have any members, since none of its methods are
// odr-used here.
struct slot_map_zero_overhead_probe_hooks {};
} // namespace detail

static_assert(sizeof(slot_map_ptr<void, slot_map_mapper<1, detail::slot_map_zero_overhead_probe_hooks>>) ==
                  sizeof(std::uint64_t),
              "slot_map_ptr must have zero overhead");

} // namespace structo
