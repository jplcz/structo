// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file tlb_flush.hpp
 * @brief `structo::arch::tlb_flush_traits<Arch>`: the per-architecture
 * TLB-maintenance customization point, plus `structo::arch::tlb_flusher<
 * Arch>`, the architecture-agnostic dispatcher that routes every flush
 * request through it -- the concrete, callable realization of the
 * `arch_flush_tlb_all()`/`arch_flush_tlb_asid()`/
 * `arch_flush_tlb_page_asid()`/`arch_has_broadcast_tlbi` pseudocode
 * `asid_allocator.hpp`'s and `mm_asid_context.hpp`'s examples have been
 * sketching all along.
 *
 * ## Why this exists
 *
 * Every piece of bookkeeping elsewhere in this library
 * (`asid_allocator`, `fixed_asid_allocator`, `mm_asid_context`) is
 * deliberately agnostic about how a TLB is actually invalidated -- they
 * only ever hand back a raw ASID/VMID value and leave "now go flush it"
 * as a documented caller obligation. This header is the other half: a
 * single, uniform customization point a concrete architecture
 * specializes *once*, so every one of those bookkeeping headers' worked
 * examples becomes real, callable code instead of pseudocode.
 *
 * ## Spaces: more than one kind of TLB to invalidate
 *
 * A flush is never just "the TLB" -- which cached translations even
 * *exist* to invalidate depends on which translation regime produced
 * them. `tlb_flush_traits<Arch>` is parameterized per call not just by
 * architecture but by a `Space` tag identifying that regime, so one
 * architecture's trait can model as many (or as few) of these as it
 * implements hardware support for:
 *
 * - `untagged_tlb_space` -- a flat TLB with no software-visible tag at
 *   all (e.g. ARMv4/v5, or any MMU design -- real or a unit test fake --
 *   that only ever supports invalidating everything at once).
 * - `process_tlb_space` -- the common case: an ordinary per-task address
 *   space tagged by a hardware ASID/PCID.
 * - `guest_tlb_space` -- a hypervisor's stage-2/nested (IPA-or-GPA to PA)
 *   translations, tagged by VMID/VPID/EPT-or-NPT context ID.
 * - `hypervisor_tlb_space` -- the hypervisor's own EL2/VMX-root
 *   translations -- typically untagged even on hardware that tags guest
 *   stage-1 translations.
 * - `secure_tlb_space` / `nonsecure_tlb_space` -- ARM TrustZone worlds.
 * - `root_tlb_space` / `realm_tlb_space` -- ARM Realm Management
 *   Extension (RME) worlds; Root (EL3 monitor) is untagged, Realm is
 *   ASID-tagged like `process_tlb_space` but in the isolated Realm
 *   world.
 * - `gpt_tlb_space` -- RME's Granule Protection Table cache: addressed
 *   by physical address, with no ASID/VMID tag at all, EL3/RMM-only
 *   (e.g. ARM `TLBI PAALL`/`PAALLOS`/`RPAOS`/`RPALOS`).
 *
 * A `Space` an architecture's `tlb_flush_traits` specialization never
 * mentions simply isn't supported there -- `tlb_flusher<Arch>` reports
 * that as a `static_assert`, not a runtime error, since "which spaces
 * exist" is always a compile-time architectural fact.
 *
 * ## The customization point: `tlb_flush_traits<Arch>`
 *
 * Left undefined for any `Arch` that hasn't opted in (mirroring
 * `io_space_traits<Backend>`/`page_table_entry_traits<Tag>`). A real
 * specialization (added per-architecture in a follow-up, e.g.
 * `structo::arch::arm64::tlb_flush_traits` wiring up real `TLBI`
 * instructions) supplies a subset of these `static` member templates,
 * each itself templated on `Space`:
 *
 * @code
 * template <> struct structo::arch::tlb_flush_traits<my_arch_tag> {
 *   static constexpr bool supports_broadcast = true; // this PE can broadcast a TLBI to its shareability domain
 *
 *   // Optional: caps how large an [addr_begin, addr_end) range may be
 *   // before tlb_flusher gives up on a precise range flush and flushes
 *   // the whole tag/Space instead -- see "Oversized ranges" below.
 *   // Absent means "no limit, always attempt a precise range flush".
 *   static constexpr std::uint64_t max_range_bytes = 64 * 1024 * 1024;
 *
 *   // Mandatory: every Space this trait supports must define this one.
 *   template <typename Space> static void flush_all() noexcept;
 *
 *   // Optional (any absent one falls back -- see "Precision fallback" below):
 *   template <typename Space> static void flush_tag(std::uint64_t tag) noexcept;
 *   template <typename Space> static void flush_page(std::uint64_t addr) noexcept;
 *   template <typename Space> static void flush_page_tag(std::uint64_t addr, std::uint64_t tag) noexcept;
 *   template <typename Space> static void flush_range(std::uint64_t addr_begin, std::uint64_t addr_end) noexcept;
 *   template <typename Space> static void flush_range_tag(std::uint64_t addr_begin, std::uint64_t addr_end,
 *                                                          std::uint64_t tag) noexcept;
 *
 *   // Optional, only when supports_broadcast: the broadcast twin of any
 *   // subset of the six above (e.g. ARM's "...IS"/"...OS" encodings).
 *   template <typename Space> static void flush_all_broadcast() noexcept;
 *   template <typename Space> static void flush_tag_broadcast(std::uint64_t tag) noexcept;
 *   template <typename Space> static void flush_page_broadcast(std::uint64_t addr) noexcept;
 *   template <typename Space> static void flush_page_tag_broadcast(std::uint64_t addr, std::uint64_t tag) noexcept;
 *   template <typename Space> static void flush_range_broadcast(std::uint64_t addr_begin, std::uint64_t addr_end) noexcept;
 *   template <typename Space> static void flush_range_tag_broadcast(std::uint64_t addr_begin, std::uint64_t addr_end,
 *                                                                    std::uint64_t tag) noexcept;
 * };
 * @endcode
 *
 * `addr`/`addr_begin`/`addr_end`/`tag` are always raw, already-decoded
 * hardware bit patterns -- a VA, IPA, or PA depending on `Space`, and an
 * ASID/VMID/PCID depending on `Space` -- exactly the kind of value
 * `asid_allocator::asid_of()`/`fixed_asid_allocator::asid_of()` already
 * hand back. This header never validates or interprets them.
 *
 * ## Precision fallback: coarsening is always safe, so it is automatic
 *
 * A TLB flush has exactly one safety direction: invalidating *more* than
 * strictly necessary is always correct (merely slower), invalidating
 * *less* is never correct. `tlb_flusher<Arch>` exploits this: if a
 * `Space`'s trait doesn't implement a precise operation, it transparently
 * falls back to the next-coarsest one it does implement, all the way
 * down to the one every supported `Space` must define, `flush_all`:
 *
 * @code
 * flush_page_tag  -> flush_tag -> flush_page -> flush_all   // first fallback found wins, left to right
 * flush_range_tag -> flush_tag -> flush_range -> flush_all
 * flush_range     -> flush_all
 * flush_page      -> flush_all
 * flush_tag       -> flush_all
 * @endcode
 *
 * This is exactly how `process_tlb_space` on hardware with no ASID
 * tagging at all (e.g. legacy ARM) keeps working correctly through this
 * same API: a trait that never defines `flush_tag`/`flush_page_tag` for
 * that `Space` simply has every tagged call degrade to the untagged
 * `flush_all`/`flush_page` it does provide -- over-invalidating a few
 * extra entries, never under-invalidating any.
 *
 * ## Broadcast: an orthogonal axis, never silently faked
 *
 * Unlike precision, *reach* (one core vs. the architecture's whole
 * shareability domain) has no safe direction to coarsen into
 * automatically: silently serving a `..._broadcast()` call with a
 * local-only flush would make the caller believe every core is
 * consistent when only this one is, which is a real correctness bug, not
 * a harmless over-invalidation. So broadcast support is never
 * synthesized: `tlb_flush_traits<Arch>::supports_broadcast` is a plain
 * informational constant for the *caller* to branch on (exactly the
 * `arch_has_broadcast_tlbi` constant `asid_allocator.hpp`'s own examples
 * already assume) -- `tlb_flusher`'s `..._broadcast()` methods apply the
 * same precision-fallback chain as their local counterparts *among
 * themselves* (e.g. `flush_page_tag_broadcast` may fall back to
 * `flush_tag_broadcast`), but a `Space`/`Arch` pair with no broadcast
 * operation defined for it at all fails to compile with a clear
 * `static_assert`, not a silent local-only flush. An architecture with no
 * hardware broadcast at all (x86, RISC-V without a custom extension)
 * simply never defines any `..._broadcast` member -- every broadcast
 * call for it is a compile error, and cross-core consistency is the
 * caller's job via its own IPI-driven shootdown, exactly as
 * `asid_allocator.hpp`'s `if constexpr (!arch_has_broadcast_tlbi) {
 * arch_wait_for_shootdown_acks(targets); }` branch already documents.
 *
 * ## Range flushes are first-class, not an afterthought
 *
 * `flush_range`/`flush_range_tag` (and their broadcast twins) are
 * guaranteed-present operations on `tlb_flusher`, exactly like
 * `flush_all`/`flush_tag`/`flush_page` -- never an optional add-on a
 * caller has to feature-test for. An architecture with a native
 * range-invalidate instruction (e.g. ARM `FEAT_TLBIRANGE`'s `TLBI
 * RVAE1IS`) wires it up directly for maximum precision; one without
 * still gets correct (if coarser) behavior for free through the same
 * fallback chain as every other operation here, down to a single
 * `flush_tag`/`flush_all` call.
 *
 * ## Oversized ranges: a huge range is not worth flushing precisely
 *
 * A native range-invalidate instruction still costs roughly one hardware
 * operation per page (or per however many entries its encoding packs
 * into one instruction) -- past some crossover point, walking a huge
 * range one chunk at a time costs more than a single `flush_tag`/
 * `flush_all` that over-invalidates everything at once. An architecture
 * states that crossover as the optional `max_range_bytes` trait
 * constant; whenever `(addr_end - addr_begin)` exceeds it,
 * `tlb_flusher` skips the range op entirely and flushes the coarser
 * equivalent instead -- the whole `tag` for `flush_range_tag`/
 * `flush_range_tag_broadcast`, or the whole `Space` for `flush_range`/
 * `flush_range_broadcast` (which have no tag to fall back to). This
 * check runs before, and independently of, whether `Space` even has a
 * native range op -- a trait with no `max_range_bytes` at all (the
 * default) always attempts the most precise op it has, exactly as
 * before this existed.
 */

#include <cstdint>
#include <type_traits>

namespace structo::arch {

// ============================================================================
// TLB Space Tags
// ============================================================================

/** @brief A flat TLB with no software-visible tag at all (e.g. legacy ARM without ASID support). */
struct untagged_tlb_space {};

/** @brief Ordinary per-task address space, tagged by a hardware ASID/PCID. */
struct process_tlb_space {};

/** @brief Hypervisor-managed guest stage-2/nested translations, tagged by VMID/VPID. */
struct guest_tlb_space {};

/** @brief The hypervisor's own EL2/VMX-root translations -- typically untagged. */
struct hypervisor_tlb_space {};

/** @brief ARM TrustZone Secure world translations. */
struct secure_tlb_space {};

/** @brief ARM TrustZone Non-secure world translations. */
struct nonsecure_tlb_space {};

/** @brief ARM Realm Management Extension (RME) Root world (EL3 monitor) translations -- untagged. */
struct root_tlb_space {};

/** @brief ARM RME Realm world translations -- ASID-tagged, like @ref process_tlb_space, but in the isolated Realm world. */
struct realm_tlb_space {};

/** @brief ARM RME Granule Protection Table cache -- PA-addressed, untagged, EL3/RMM-only. */
struct gpt_tlb_space {};

// ============================================================================
// Customization Point
// ============================================================================

/**
 * @brief Per-architecture TLB-maintenance customization point. See the
 * @file-level docs for the full required/optional member-template list
 * and the precision-fallback/broadcast rules `tlb_flusher<Arch>` applies
 * on top of it. Intentionally left undefined for any `Arch` that hasn't
 * been adapted, mirroring `io_space_traits<Backend>`.
 */
template <typename Arch> struct tlb_flush_traits;

namespace detail {

template <typename, typename, typename = void> struct tlb_has_flush_all : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_all<Traits, Space, std::void_t<decltype(Traits::template flush_all<Space>())>>
    : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_tag : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_tag<Traits, Space,
                          std::void_t<decltype(Traits::template flush_tag<Space>(std::declval<std::uint64_t>()))>>
    : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_page : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_page<Traits, Space,
                           std::void_t<decltype(Traits::template flush_page<Space>(std::declval<std::uint64_t>()))>>
    : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_page_tag : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_page_tag<Traits, Space,
                               std::void_t<decltype(Traits::template flush_page_tag<Space>(
                                   std::declval<std::uint64_t>(), std::declval<std::uint64_t>()))>> : std::true_type {
};

template <typename, typename, typename = void> struct tlb_has_flush_range : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_range<Traits, Space,
                            std::void_t<decltype(Traits::template flush_range<Space>(
                                std::declval<std::uint64_t>(), std::declval<std::uint64_t>()))>> : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_range_tag : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_range_tag<Traits, Space,
                                std::void_t<decltype(Traits::template flush_range_tag<Space>(
                                    std::declval<std::uint64_t>(), std::declval<std::uint64_t>(),
                                    std::declval<std::uint64_t>()))>> : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_all_broadcast : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_all_broadcast<Traits, Space,
                                    std::void_t<decltype(Traits::template flush_all_broadcast<Space>())>>
    : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_tag_broadcast : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_tag_broadcast<
    Traits, Space, std::void_t<decltype(Traits::template flush_tag_broadcast<Space>(std::declval<std::uint64_t>()))>>
    : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_page_broadcast : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_page_broadcast<
    Traits, Space,
    std::void_t<decltype(Traits::template flush_page_broadcast<Space>(std::declval<std::uint64_t>()))>>
    : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_page_tag_broadcast : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_page_tag_broadcast<Traits, Space,
                                         std::void_t<decltype(Traits::template flush_page_tag_broadcast<Space>(
                                             std::declval<std::uint64_t>(), std::declval<std::uint64_t>()))>>
    : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_range_broadcast : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_range_broadcast<Traits, Space,
                                      std::void_t<decltype(Traits::template flush_range_broadcast<Space>(
                                          std::declval<std::uint64_t>(), std::declval<std::uint64_t>()))>>
    : std::true_type {};

template <typename, typename, typename = void> struct tlb_has_flush_range_tag_broadcast : std::false_type {};
template <typename Traits, typename Space>
struct tlb_has_flush_range_tag_broadcast<
    Traits, Space,
    std::void_t<decltype(Traits::template flush_range_tag_broadcast<Space>(
        std::declval<std::uint64_t>(), std::declval<std::uint64_t>(), std::declval<std::uint64_t>()))>>
    : std::true_type {};

template <typename, typename = void> struct tlb_has_max_range_bytes : std::false_type {};
template <typename Traits>
struct tlb_has_max_range_bytes<Traits, std::void_t<decltype(Traits::max_range_bytes)>> : std::true_type {};

} // namespace detail

// ============================================================================
// Dispatcher
// ============================================================================

/**
 * @brief Architecture-agnostic TLB-flush dispatcher: routes every call
 * through `tlb_flush_traits<Arch>`, applying the precision-fallback
 * chain documented at file scope. Stateless -- every member is `static`,
 * exactly like `barrier_traits`.
 */
template <typename Arch> struct tlb_flusher {
  using traits = tlb_flush_traits<Arch>;

  /** @brief Whether `Arch` can broadcast a flush to its whole shareability domain in hardware. Purely informational -- see "Broadcast" in the @file docs. */
  static constexpr bool supports_broadcast = traits::supports_broadcast;

  // --- Local (this core only) ---------------------------------------------

  /** @brief Invalidates every entry in `Space`, every tag, local core only. */
  template <typename Space> static void flush_all() noexcept {
    static_assert(detail::tlb_has_flush_all<traits, Space>::value,
                  "tlb_flush_traits<Arch> does not support this Space at all -- this architecture doesn't model it");
    if constexpr (detail::tlb_has_flush_all<traits, Space>::value) {
      traits::template flush_all<Space>();
    }
  }

  /** @brief Invalidates every entry tagged `tag` in `Space`, local core only. Falls back to @ref flush_all if `Space` has no tagged flush. */
  template <typename Space> static void flush_tag(std::uint64_t tag) noexcept {
    if constexpr (detail::tlb_has_flush_tag<traits, Space>::value) {
      traits::template flush_tag<Space>(tag);
    } else {
      flush_all<Space>();
    }
  }

  /** @brief Invalidates `addr` in `Space`, every tag, local core only. Falls back to @ref flush_all if `Space` has no by-address flush. */
  template <typename Space> static void flush_page(std::uint64_t addr) noexcept {
    if constexpr (detail::tlb_has_flush_page<traits, Space>::value) {
      traits::template flush_page<Space>(addr);
    } else {
      flush_all<Space>();
    }
  }

  /**
   * @brief Invalidates `addr` tagged `tag` in `Space`, local core only.
   * Falls back to @ref flush_tag (dropping address precision), then
   * @ref flush_page (dropping tag precision), then @ref flush_all.
   */
  template <typename Space> static void flush_page_tag(std::uint64_t addr, std::uint64_t tag) noexcept {
    if constexpr (detail::tlb_has_flush_page_tag<traits, Space>::value) {
      traits::template flush_page_tag<Space>(addr, tag);
    } else if constexpr (detail::tlb_has_flush_tag<traits, Space>::value) {
      traits::template flush_tag<Space>(tag);
    } else if constexpr (detail::tlb_has_flush_page<traits, Space>::value) {
      traits::template flush_page<Space>(addr);
    } else {
      flush_all<Space>();
    }
  }

  /**
   * @brief Whether `[addr_begin, addr_end)` is too large to be worth a
   * precise range flush, per `tlb_flush_traits<Arch>::max_range_bytes`
   * -- see "Oversized ranges" in the @file docs. Always `false` if the
   * trait leaves `max_range_bytes` undefined (no limit).
   */
  static bool range_is_oversized(std::uint64_t addr_begin, std::uint64_t addr_end) noexcept {
    if constexpr (detail::tlb_has_max_range_bytes<traits>::value) {
      return (addr_end - addr_begin) > traits::max_range_bytes;
    } else {
      return false;
    }
  }

  /**
   * @brief Invalidates `[addr_begin, addr_end)` in `Space`, every tag,
   * local core only. Falls back to @ref flush_all if `Space` has no
   * native range flush, or if the range is oversized -- see "Oversized
   * ranges" in the @file docs.
   */
  template <typename Space> static void flush_range(std::uint64_t addr_begin, std::uint64_t addr_end) noexcept {
    if (range_is_oversized(addr_begin, addr_end)) {
      flush_all<Space>();
      return;
    }
    if constexpr (detail::tlb_has_flush_range<traits, Space>::value) {
      traits::template flush_range<Space>(addr_begin, addr_end);
    } else {
      flush_all<Space>();
    }
  }

  /**
   * @brief Invalidates `[addr_begin, addr_end)` tagged `tag` in `Space`,
   * local core only. Falls back to @ref flush_tag (dropping range
   * precision), then @ref flush_all -- also via @ref flush_tag if the
   * range is oversized, see "Oversized ranges" in the @file docs.
   */
  template <typename Space>
  static void flush_range_tag(std::uint64_t addr_begin, std::uint64_t addr_end, std::uint64_t tag) noexcept {
    if (range_is_oversized(addr_begin, addr_end)) {
      flush_tag<Space>(tag);
      return;
    }
    if constexpr (detail::tlb_has_flush_range_tag<traits, Space>::value) {
      traits::template flush_range_tag<Space>(addr_begin, addr_end, tag);
    } else if constexpr (detail::tlb_has_flush_tag<traits, Space>::value) {
      traits::template flush_tag<Space>(tag);
    } else {
      flush_all<Space>();
    }
  }

  // --- Broadcast (whole shareability domain) ------------------------------
  //
  // No fallback to a local-only flush, ever -- see "Broadcast" in the
  // @file docs. A `Space` with no broadcast operation defined at all is a
  // static_assert, pointing the caller at its own IPI-driven shootdown.

  /** @brief Broadcast twin of @ref flush_all. Requires `Space` to define a broadcast flush (directly or via its own fallback chain). */
  template <typename Space> static void flush_all_broadcast() noexcept {
    static_assert(detail::tlb_has_flush_all_broadcast<traits, Space>::value,
                  "tlb_flush_traits<Arch> has no broadcast flush for this Space -- "
                  "flush locally on every core yourself (e.g. via an IPI-driven shootdown)");
    if constexpr (detail::tlb_has_flush_all_broadcast<traits, Space>::value) {
      traits::template flush_all_broadcast<Space>();
    }
  }

  /** @brief Broadcast twin of @ref flush_tag. Falls back to @ref flush_all_broadcast; never to a local-only flush. */
  template <typename Space> static void flush_tag_broadcast(std::uint64_t tag) noexcept {
    if constexpr (detail::tlb_has_flush_tag_broadcast<traits, Space>::value) {
      traits::template flush_tag_broadcast<Space>(tag);
    } else {
      flush_all_broadcast<Space>();
    }
  }

  /** @brief Broadcast twin of @ref flush_page. Falls back to @ref flush_all_broadcast; never to a local-only flush. */
  template <typename Space> static void flush_page_broadcast(std::uint64_t addr) noexcept {
    if constexpr (detail::tlb_has_flush_page_broadcast<traits, Space>::value) {
      traits::template flush_page_broadcast<Space>(addr);
    } else {
      flush_all_broadcast<Space>();
    }
  }

  /** @brief Broadcast twin of @ref flush_page_tag. Falls back to @ref flush_tag_broadcast, then @ref flush_page_broadcast, then @ref flush_all_broadcast; never to a local-only flush. */
  template <typename Space> static void flush_page_tag_broadcast(std::uint64_t addr, std::uint64_t tag) noexcept {
    if constexpr (detail::tlb_has_flush_page_tag_broadcast<traits, Space>::value) {
      traits::template flush_page_tag_broadcast<Space>(addr, tag);
    } else if constexpr (detail::tlb_has_flush_tag_broadcast<traits, Space>::value) {
      traits::template flush_tag_broadcast<Space>(tag);
    } else if constexpr (detail::tlb_has_flush_page_broadcast<traits, Space>::value) {
      traits::template flush_page_broadcast<Space>(addr);
    } else {
      flush_all_broadcast<Space>();
    }
  }

  /**
   * @brief Broadcast twin of @ref flush_range. Falls back to
   * @ref flush_all_broadcast -- also if the range is oversized, see
   * "Oversized ranges" in the @file docs; never to a local-only flush.
   */
  template <typename Space>
  static void flush_range_broadcast(std::uint64_t addr_begin, std::uint64_t addr_end) noexcept {
    if (range_is_oversized(addr_begin, addr_end)) {
      flush_all_broadcast<Space>();
      return;
    }
    if constexpr (detail::tlb_has_flush_range_broadcast<traits, Space>::value) {
      traits::template flush_range_broadcast<Space>(addr_begin, addr_end);
    } else {
      flush_all_broadcast<Space>();
    }
  }

  /**
   * @brief Broadcast twin of @ref flush_range_tag. Falls back to
   * @ref flush_tag_broadcast, then @ref flush_all_broadcast -- also via
   * @ref flush_tag_broadcast if the range is oversized, see "Oversized
   * ranges" in the @file docs; never to a local-only flush.
   */
  template <typename Space>
  static void flush_range_tag_broadcast(std::uint64_t addr_begin, std::uint64_t addr_end, std::uint64_t tag) noexcept {
    if (range_is_oversized(addr_begin, addr_end)) {
      flush_tag_broadcast<Space>(tag);
      return;
    }
    if constexpr (detail::tlb_has_flush_range_tag_broadcast<traits, Space>::value) {
      traits::template flush_range_tag_broadcast<Space>(addr_begin, addr_end, tag);
    } else if constexpr (detail::tlb_has_flush_tag_broadcast<traits, Space>::value) {
      traits::template flush_tag_broadcast<Space>(tag);
    } else {
      flush_all_broadcast<Space>();
    }
  }
};

} // namespace structo::arch
