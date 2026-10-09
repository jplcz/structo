// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file sbi.hpp
 * @brief RISC-V Supervisor Binary Interface (SBI) client: extension and function
 * ids, error codes, and typed wrappers over a pluggable `ecall` backend.
 *
 * SBI is how S-mode software (a kernel or hypervisor) asks M-mode firmware
 * (OpenSBI, RustSBI) for services: `a7` = extension id (EID), `a6` = function
 * id (FID), `a0..a5` = arguments; the firmware returns `a0` = error and
 * `a1` = value.
 *
 * The header performs no `ecall` itself, so it is testable on any host.
 * You supply a backend: any type with
 * `sbiret call(uint32_t eid, uint32_t fid, uint64_t a0, ..., uint64_t a5)`.
 *
 * @code
 * // Backend: why - only this type contains the architecture-specific ecall.
 * struct ecall_backend {
 *   // eid: extension id (a7), fid: function id (a6), a0..a5: call arguments.
 *   structo::riscv::sbi::sbiret call(uint32_t eid, uint32_t fid, uint64_t a0, uint64_t a1, uint64_t a2,
 *                                    uint64_t a3, uint64_t a4, uint64_t a5) noexcept {
 *     register uint64_t r0 asm("a0") = a0;
 *     register uint64_t r1 asm("a1") = a1;
 *     register uint64_t r2 asm("a2") = a2;
 *     register uint64_t r3 asm("a3") = a3;
 *     register uint64_t r4 asm("a4") = a4;
 *     register uint64_t r5 asm("a5") = a5;
 *     register uint64_t r6 asm("a6") = fid;
 *     register uint64_t r7 asm("a7") = eid;
 *     asm volatile("ecall" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3), "r"(r4), "r"(r5), "r"(r6), "r"(r7) : "memory");
 *     return {static_cast<int64_t>(r0), static_cast<int64_t>(r1)};
 *   }
 * };
 *
 * structo::riscv::sbi::client<ecall_backend> sbi{ecall_backend{}};
 * if (sbi.probe_extension(structo::riscv::sbi::eid_time).value_or(false))
 *   (void)sbi.set_timer(next_deadline_ticks);   // absolute mtime value of the next timer interrupt
 * (void)sbi.hart_start(1, entry_phys, opaque);  // hart id, physical entry address, value passed in a1
 * @endcode
 */

#include <cstdint>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>

namespace structo::riscv::sbi {

using namespace reloco;

/** @brief Extension ids. */
inline constexpr uint32_t eid_base = 0x10;
inline constexpr uint32_t eid_time = 0x54494D45;
inline constexpr uint32_t eid_ipi = 0x735049;
inline constexpr uint32_t eid_rfence = 0x52464E43;
inline constexpr uint32_t eid_hsm = 0x48534D;
inline constexpr uint32_t eid_srst = 0x53525354;
inline constexpr uint32_t eid_pmu = 0x504D55;
inline constexpr uint32_t eid_dbcn = 0x4442434E;
inline constexpr uint32_t eid_susp = 0x53555350;
inline constexpr uint32_t eid_cppc = 0x43505043;
inline constexpr uint32_t eid_nacl = 0x4E41434C;
inline constexpr uint32_t eid_sta = 0x535441;
/** @brief Legacy (v0.1) console putchar, deprecated; present on very old firmware. */
inline constexpr uint32_t eid_legacy_console_putchar = 0x01;
inline constexpr uint32_t eid_legacy_console_getchar = 0x02;

/** @brief Standard error codes returned in `sbiret::error`. */
enum class status : int64_t {
  success = 0,
  failed = -1,
  not_supported = -2,
  invalid_param = -3,
  denied = -4,
  invalid_address = -5,
  already_available = -6,
  already_started = -7,
  already_stopped = -8,
  no_shmem = -9,
  invalid_state = -10,
  bad_range = -11,
  timeout = -12,
  io = -13,
};

/** @brief The `a0`/`a1` pair every SBI call returns. */
struct sbiret {
  int64_t error = 0;
  int64_t value = 0;
};

/** @brief Maps a non-zero SBI error code to the nearest `reloco::error` (`success` has no error and maps to `io_error`
 * defensively; callers only map failures). */
[[nodiscard]] constexpr error status_to_error(int64_t code) noexcept {
  switch (static_cast<status>(code)) {
  case status::not_supported:
    return error::unsupported_operation;
  case status::invalid_param:
  case status::invalid_address:
    return error::invalid_argument;
  case status::invalid_state:
    return error::invalid_state;
  case status::bad_range:
    return error::out_of_range;
  case status::denied:
    return error::permission_denied;
  case status::already_available:
  case status::already_started:
  case status::already_stopped:
    return error::already_exists;
  case status::no_shmem:
    return error::resource_exhausted;
  case status::timeout:
    return error::timed_out;
  default:
    return error::io_error;
  }
}

/** @brief Base extension function ids. */
namespace base_fid {
inline constexpr uint32_t get_spec_version = 0;
inline constexpr uint32_t get_impl_id = 1;
inline constexpr uint32_t get_impl_version = 2;
inline constexpr uint32_t probe_extension = 3;
inline constexpr uint32_t get_mvendorid = 4;
inline constexpr uint32_t get_marchid = 5;
inline constexpr uint32_t get_mimpid = 6;
} // namespace base_fid

/** @brief Hart State Management states returned by `hart_get_status`. */
enum class hart_state : uint64_t {
  started = 0,
  stopped = 1,
  start_pending = 2,
  stop_pending = 3,
  suspended = 4,
  suspend_pending = 5,
  resume_pending = 6,
};

/** @brief System reset types and reasons (SRST). */
enum class reset_type : uint32_t { shutdown = 0, cold_reboot = 1, warm_reboot = 2 };
enum class reset_reason : uint32_t { none = 0, system_failure = 1 };

/** @brief Decoded SBI spec version (`major.minor`). */
struct spec_version {
  uint32_t major = 0;
  uint32_t minor = 0;
};

/** @brief Typed SBI calls over @p Backend (see the file comment for the contract). */
template <typename Backend> class client {
public:
  explicit client(Backend backend) noexcept : backend_(static_cast<Backend &&>(backend)) {}

  /** @brief Raw call: returns the error and value as the firmware gave them. */
  [[nodiscard]] sbiret raw(uint32_t eid, uint32_t fid, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0,
                           uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) noexcept {
    return backend_.call(eid, fid, a0, a1, a2, a3, a4, a5);
  }

  /** @brief Raw call, converting a non-zero error to `unexpected` and yielding the value. */
  [[nodiscard]] result<uint64_t> call(uint32_t eid, uint32_t fid, uint64_t a0 = 0, uint64_t a1 = 0, uint64_t a2 = 0,
                                      uint64_t a3 = 0, uint64_t a4 = 0, uint64_t a5 = 0) noexcept {
    const sbiret r = raw(eid, fid, a0, a1, a2, a3, a4, a5);
    if (r.error != 0)
      return unexpected(status_to_error(r.error));
    return static_cast<uint64_t>(r.value);
  }

  // ---- Base ----

  [[nodiscard]] result<spec_version> get_spec_version() noexcept {
    auto v = call(eid_base, base_fid::get_spec_version);
    if (!v)
      return unexpected(v.error());
    return spec_version{static_cast<uint32_t>((*v >> 24) & 0x7F), static_cast<uint32_t>(*v & 0xFFFFFF)};
  }
  [[nodiscard]] result<uint64_t> get_impl_id() noexcept { return call(eid_base, base_fid::get_impl_id); }
  [[nodiscard]] result<uint64_t> get_impl_version() noexcept { return call(eid_base, base_fid::get_impl_version); }
  [[nodiscard]] result<bool> probe_extension(uint32_t eid) noexcept {
    auto v = call(eid_base, base_fid::probe_extension, eid);
    if (!v)
      return unexpected(v.error());
    return *v != 0;
  }
  [[nodiscard]] result<uint64_t> get_mvendorid() noexcept { return call(eid_base, base_fid::get_mvendorid); }
  [[nodiscard]] result<uint64_t> get_marchid() noexcept { return call(eid_base, base_fid::get_marchid); }
  [[nodiscard]] result<uint64_t> get_mimpid() noexcept { return call(eid_base, base_fid::get_mimpid); }

  // ---- TIME ----

  /** @brief Programs the next timer interrupt at absolute `mtime` value @p deadline. */
  [[nodiscard]] result<void> set_timer(uint64_t deadline) noexcept { return discard(call(eid_time, 0, deadline)); }

  // ---- IPI ----

  /** @brief Sends an S-mode software interrupt to harts `hart_mask_base + bit` for each set bit of @p hart_mask. */
  [[nodiscard]] result<void> send_ipi(uint64_t hart_mask, uint64_t hart_mask_base) noexcept {
    return discard(call(eid_ipi, 0, hart_mask, hart_mask_base));
  }

  // ---- RFENCE ----

  [[nodiscard]] result<void> remote_fence_i(uint64_t hart_mask, uint64_t hart_mask_base) noexcept {
    return discard(call(eid_rfence, 0, hart_mask, hart_mask_base));
  }
  /** @brief Remote `sfence.vma` over `[start, start + size)` (size 0 or all-ones = whole address space). */
  [[nodiscard]] result<void> remote_sfence_vma(uint64_t hart_mask, uint64_t hart_mask_base, uint64_t start,
                                               uint64_t size) noexcept {
    return discard(call(eid_rfence, 1, hart_mask, hart_mask_base, start, size));
  }
  [[nodiscard]] result<void> remote_sfence_vma_asid(uint64_t hart_mask, uint64_t hart_mask_base, uint64_t start,
                                                    uint64_t size, uint64_t asid) noexcept {
    return discard(call(eid_rfence, 2, hart_mask, hart_mask_base, start, size, asid));
  }

  // ---- HSM ----

  /** @brief Starts @p hartid at physical @p start_addr in S-mode with `a0 = hartid`, `a1 = opaque`. */
  [[nodiscard]] result<void> hart_start(uint64_t hartid, uint64_t start_addr, uint64_t opaque) noexcept {
    return discard(call(eid_hsm, 0, hartid, start_addr, opaque));
  }
  /** @brief Stops the calling hart; does not return on success. */
  [[nodiscard]] result<void> hart_stop() noexcept { return discard(call(eid_hsm, 1)); }
  [[nodiscard]] result<hart_state> hart_get_status(uint64_t hartid) noexcept {
    auto v = call(eid_hsm, 2, hartid);
    if (!v)
      return unexpected(v.error());
    return static_cast<hart_state>(*v);
  }

  // ---- SRST ----

  /** @brief Requests a system reset; returns only on failure. */
  [[nodiscard]] result<void> system_reset(reset_type type, reset_reason reason = reset_reason::none) noexcept {
    return discard(call(eid_srst, 0, static_cast<uint64_t>(type), static_cast<uint64_t>(reason)));
  }

  // ---- DBCN (debug console) ----

  /** @brief Writes @p num_bytes bytes at physical address (@p base_lo, @p base_hi); yields the count written. */
  [[nodiscard]] result<uint64_t> debug_console_write(uint64_t num_bytes, uint64_t base_lo, uint64_t base_hi) noexcept {
    return call(eid_dbcn, 0, num_bytes, base_lo, base_hi);
  }
  [[nodiscard]] result<uint64_t> debug_console_read(uint64_t num_bytes, uint64_t base_lo, uint64_t base_hi) noexcept {
    return call(eid_dbcn, 1, num_bytes, base_lo, base_hi);
  }
  /** @brief Writes one byte; the only DBCN call that needs no memory address. */
  [[nodiscard]] result<void> debug_console_write_byte(uint8_t byte) noexcept {
    return discard(call(eid_dbcn, 2, byte));
  }

  [[nodiscard]] Backend &backend() noexcept { return backend_; }

private:
  static result<void> discard(result<uint64_t> r) noexcept {
    if (!r)
      return unexpected(r.error());
    return {};
  }
  Backend backend_;
};

} // namespace structo::riscv::sbi
