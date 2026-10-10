// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file protection.hpp
 * @brief Architecture-independent, type-safe memory protection flags with
 * separate privileged ("kernel") and unprivileged ("user") access.
 *
 * Every architecture format of `page_mapper` accepts a `protection` and
 * *legalizes* it internally: the request is turned into the nearest encoding the
 * hardware can express that is **never more permissive** than what was asked.
 * Kernel and user permissions therefore do not have to be representable
 * independently on every architecture (x86 has a single XD bit, RISC-V a
 * single U bit, AArch64 has no "kernel RW, user RO" encoding, ...).
 *
 * @code
 * using namespace structo::arch;
 *
 * // Kernel read/execute, no user access (typical kernel .text).
 * protection text = protection::kernel_text();
 *
 * // Builder style: kernel RW + user RO, write-back, per-address-space.
 * protection p = protection{}.with_kernel(kprot::write).with_user(uprot::read);
 *
 * // Cross-category operator| builds a protection; mixing within one category
 * // (kprot | kprot, cache_mode | cache_mode) deliberately does not compile.
 * protection q = kprot::write | uprot::read | cache_mode::uncached;
 *
 * // WXN / UWXN: apply the SCTLR policy the mapper's target runs with, so the
 * // mapping you query back equals what the hardware will enforce.
 * protection r = protection::kernel_data().with_kernel(kprot::exec).enforce_policy<mmu_policy<true>>();
 * @endcode
 */

#include <cstdint>

namespace structo::arch {

/** Privileged access. Each level implies the previous one (exec => write is
 * NOT implied; `write` implies read; `exec` is execute-only-capable, add
 * `kprot::read_exec` for r-x). */
enum class kprot : std::uint8_t {
  none = 0,
  read = 1,
  write = 2, // read + write
  exec = 4,  // execute (read implied)
  read_exec = 5,
  write_exec = 6 // read + write + execute (rejected under WXN)
};

/** Unprivileged access; same encoding as `kprot`. */
enum class uprot : std::uint8_t { none = 0, read = 1, write = 2, exec = 4, read_exec = 5, write_exec = 6 };

/** Whether the translation is private to the address space (tagged by ASID/PCID)
 * or shared by all (global / nG=0 / G=1). */
enum class scope : std::uint8_t { per_address_space = 0, global = 1 };

/** Memory type. Architectures map these onto PAT / MAIR / PBMT / MemAttr; types
 * a format cannot express make `legalize()` / `make_leaf()` fail with
 * `unsupported_operation` instead of silently downgrading. */
enum class cache_mode : std::uint8_t {
  write_back = 0,
  write_through,
  uncached,
  write_combining,
  device,        // device, gathering/reordering/early-ack disabled where available
  device_ordered // strongest ordering (ARM nGnRnE)
};

/** Target security state for formats that distinguish it (ARM Secure/NS tables). */
enum class security_state : std::uint8_t { inherit = 0, secure, non_secure };

/** SCTLR-style enforcement policy.
 * @tparam Wxn  any writable mapping (kernel or user) is never privileged-executable
 *              (AArch64 SCTLR.WXN: write-implies-XN, applied at all levels).
 * @tparam Uwxn user-writable mappings are never privileged-executable (SCTLR.UWXN). */
template <bool Wxn = false, bool Uwxn = false> struct mmu_policy {
  static constexpr bool wxn = Wxn;
  static constexpr bool uwxn = Uwxn;
};
using default_mmu_policy = mmu_policy<>;

namespace detail {
constexpr bool k_can(kprot p, kprot bit) noexcept {
  return (static_cast<std::uint8_t>(p) & static_cast<std::uint8_t>(bit)) != 0;
}
constexpr bool u_can(uprot p, uprot bit) noexcept {
  return (static_cast<std::uint8_t>(p) & static_cast<std::uint8_t>(bit)) != 0;
}
} // namespace detail

/** Value type combining all attributes of a mapping. */
class protection {
public:
  constexpr protection() noexcept = default;

  // --- getters ---
  [[nodiscard]] constexpr kprot kernel() const noexcept { return kernel_; }
  [[nodiscard]] constexpr uprot user() const noexcept { return user_; }
  [[nodiscard]] constexpr scope scope_of() const noexcept { return scope_; }
  [[nodiscard]] constexpr cache_mode cache() const noexcept { return cache_; }
  [[nodiscard]] constexpr security_state security() const noexcept { return security_; }

  // --- predicates ---
  [[nodiscard]] constexpr bool kernel_read() const noexcept { return kernel_ != kprot::none; }
  [[nodiscard]] constexpr bool kernel_write() const noexcept { return detail::k_can(kernel_, kprot::write); }
  [[nodiscard]] constexpr bool kernel_exec() const noexcept { return detail::k_can(kernel_, kprot::exec); }
  [[nodiscard]] constexpr bool user_read() const noexcept { return user_ != uprot::none; }
  [[nodiscard]] constexpr bool user_write() const noexcept { return detail::u_can(user_, uprot::write); }
  [[nodiscard]] constexpr bool user_exec() const noexcept { return detail::u_can(user_, uprot::exec); }
  [[nodiscard]] constexpr bool is_user_visible() const noexcept { return user_ != uprot::none; }
  [[nodiscard]] constexpr bool is_global() const noexcept { return scope_ == scope::global; }
  [[nodiscard]] constexpr bool is_device() const noexcept {
    return cache_ == cache_mode::device || cache_ == cache_mode::device_ordered;
  }
  [[nodiscard]] constexpr bool is_none() const noexcept { return kernel_ == kprot::none && user_ == uprot::none; }
  [[nodiscard]] constexpr bool violates_wx() const noexcept {
    return (kernel_write() && kernel_exec()) || (user_write() && user_exec()) || (kernel_write() && user_exec()) ||
           (user_write() && kernel_exec());
  }

  // --- builders ---
  [[nodiscard]] constexpr protection with_kernel(kprot p) const noexcept {
    protection r = *this;
    r.kernel_ = p;
    return r;
  }
  [[nodiscard]] constexpr protection without_kernel() const noexcept { return with_kernel(kprot::none); }
  [[nodiscard]] constexpr protection with_user(uprot p) const noexcept {
    protection r = *this;
    r.user_ = p;
    return r;
  }
  [[nodiscard]] constexpr protection without_user() const noexcept { return with_user(uprot::none); }
  [[nodiscard]] constexpr protection with_scope(scope s) const noexcept {
    protection r = *this;
    r.scope_ = s;
    return r;
  }
  [[nodiscard]] constexpr protection with_cache(cache_mode c) const noexcept {
    protection r = *this;
    r.cache_ = c;
    return r;
  }
  [[nodiscard]] constexpr protection with_security(security_state s) const noexcept {
    protection r = *this;
    r.security_ = s;
    return r;
  }

  /** Apply WXN / UWXN: execute permission is removed where the policy would
   * ignore it anyway, so a query reports what the hardware really enforces. */
  template <typename Policy = default_mmu_policy> [[nodiscard]] constexpr protection enforce_policy() const noexcept {
    protection r = *this;
    if constexpr (Policy::wxn) {
      if (kernel_write() || user_write()) {
        r.kernel_ = static_cast<kprot>(static_cast<std::uint8_t>(r.kernel_) & ~static_cast<std::uint8_t>(kprot::exec));
        r.user_ = static_cast<uprot>(static_cast<std::uint8_t>(r.user_) & ~static_cast<std::uint8_t>(uprot::exec));
      }
    }
    if constexpr (Policy::uwxn) {
      if (user_write())
        r.kernel_ = static_cast<kprot>(static_cast<std::uint8_t>(r.kernel_) & ~static_cast<std::uint8_t>(kprot::exec));
    }
    return r;
  }

  // --- presets ---
  [[nodiscard]] static constexpr protection kernel_text() noexcept {
    return protection{}.with_kernel(kprot::read_exec).with_scope(scope::global);
  }
  [[nodiscard]] static constexpr protection kernel_rodata() noexcept {
    return protection{}.with_kernel(kprot::read).with_scope(scope::global);
  }
  [[nodiscard]] static constexpr protection kernel_data() noexcept {
    return protection{}.with_kernel(kprot::write).with_scope(scope::global);
  }
  [[nodiscard]] static constexpr protection user_text() noexcept {
    return protection{}.with_user(uprot::read_exec).with_kernel(kprot::read);
  }
  [[nodiscard]] static constexpr protection user_rodata() noexcept {
    return protection{}.with_user(uprot::read).with_kernel(kprot::read);
  }
  [[nodiscard]] static constexpr protection user_data() noexcept {
    return protection{}.with_user(uprot::write).with_kernel(kprot::write);
  }
  [[nodiscard]] static constexpr protection kernel_rw_user_ro() noexcept {
    return protection{}.with_user(uprot::read).with_kernel(kprot::write);
  }
  [[nodiscard]] static constexpr protection device_mmio() noexcept {
    return kernel_data().with_cache(cache_mode::device);
  }
  [[nodiscard]] static constexpr protection device_ordered_mmio() noexcept {
    return kernel_data().with_cache(cache_mode::device_ordered);
  }
  [[nodiscard]] static constexpr protection dma_buffer() noexcept {
    return kernel_data().with_cache(cache_mode::uncached);
  }
  [[nodiscard]] static constexpr protection framebuffer() noexcept {
    return kernel_data().with_cache(cache_mode::write_combining);
  }
  /** Guest-physical mappings (stage 2 / EPT / G-stage): only the kernel bits are used. */
  [[nodiscard]] static constexpr protection guest_ram() noexcept { return protection{}.with_kernel(kprot::write_exec); }
  [[nodiscard]] static constexpr protection guest_rom() noexcept { return protection{}.with_kernel(kprot::read_exec); }
  [[nodiscard]] static constexpr protection guest_mmio() noexcept {
    return protection{}.with_kernel(kprot::write).with_cache(cache_mode::device);
  }

  friend constexpr bool operator==(const protection &a, const protection &b) noexcept {
    return a.kernel_ == b.kernel_ && a.user_ == b.user_ && a.scope_ == b.scope_ && a.cache_ == b.cache_ &&
           a.security_ == b.security_;
  }
  friend constexpr bool operator!=(const protection &a, const protection &b) noexcept { return !(a == b); }

private:
  kprot kernel_ = kprot::none;
  uprot user_ = uprot::none;
  scope scope_ = scope::per_address_space;
  cache_mode cache_ = cache_mode::write_back;
  security_state security_ = security_state::inherit;
};

// Cross-category combination. Same-category `|` is intentionally not provided.
constexpr protection operator|(kprot k, uprot u) noexcept { return protection{}.with_kernel(k).with_user(u); }
constexpr protection operator|(uprot u, kprot k) noexcept { return protection{}.with_kernel(k).with_user(u); }
constexpr protection operator|(kprot k, scope s) noexcept { return protection{}.with_kernel(k).with_scope(s); }
constexpr protection operator|(kprot k, cache_mode c) noexcept { return protection{}.with_kernel(k).with_cache(c); }
constexpr protection operator|(uprot u, scope s) noexcept { return protection{}.with_user(u).with_scope(s); }
constexpr protection operator|(uprot u, cache_mode c) noexcept { return protection{}.with_user(u).with_cache(c); }
constexpr protection operator|(protection p, kprot k) noexcept { return p.with_kernel(k); }
constexpr protection operator|(protection p, uprot u) noexcept { return p.with_user(u); }
constexpr protection operator|(protection p, scope s) noexcept { return p.with_scope(s); }
constexpr protection operator|(protection p, cache_mode c) noexcept { return p.with_cache(c); }
constexpr protection operator|(protection p, security_state s) noexcept { return p.with_security(s); }

} // namespace structo::arch
