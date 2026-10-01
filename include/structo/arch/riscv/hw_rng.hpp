// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file hw_rng.hpp
 * @brief `hw_rng_traits` backend for RISC-V's Zkr entropy-source
 * extension: `seed_rng` (the `seed` CSR, address `0x015`).
 *
 * A zero-sized tag type -- the CSR carries no state of its own -- bound
 * through `structo::hw::hw_rng_ref` exactly like any other
 * `hw_rng_traits` backend:
 *
 * @code
 * using namespace structo::arch::riscv;
 * using namespace structo::hw;
 *
 * seed_rng seed{};
 * hw_rng_ref rng(seed);
 * auto word = rng.try_generate64();
 * // ...
 * @endcode
 *
 * ## The `seed` CSR
 *
 * Unlike x86's `RDRAND`/`RDSEED` or AArch64's `RNDR`/`RNDRRS`, Zkr
 * exposes only a single 16-bit-at-a-time entropy source through one CSR
 * (`CSR_SEED = 0x015`), accessed with a read-write instruction that
 * atomically reads the current value and clears it (`csrrw rd, seed,
 * x0`, i.e. "swap with zero") -- a plain read is explicitly disallowed
 * by the spec since every read must consume (clear) the entropy it
 * returns. The top two bits (`[31:30]`) are the poll status `OPST`:
 *
 * | `OPST` | Meaning                                              |
 * |--------|-------------------------------------------------------|
 * | `00` (`BIST`) | Built-in self-test still running; not ready, retry |
 * | `01` (`WAIT`) | Entropy not ready yet; retry                        |
 * | `10` (`ES16`) | Success -- bits `[15:0]` hold 16 bits of entropy    |
 * | `11` (`DEAD`) | Unrecoverable hardware failure; do not retry        |
 *
 * `try_generate64` accumulates four successful 16-bit `ES16` samples
 * into one 64-bit word (matching the Linux kernel's own
 * `arch/riscv/include/asm/archrandom.h` algorithm), retrying on
 * `BIST`/`WAIT` and failing immediately and permanently on `DEAD`.
 *
 * ## Availability
 *
 * There is no standard RISC-V feature-discovery register analogous to
 * AArch64's `ID_AA64ISAR0_EL1` that is reliably accessible from this
 * header without OS support (RISC-V exposes extension discovery via
 * `riscv_hwprobe()`/device-tree, both OS- or firmware-mediated).
 * `is_available()` is therefore not implemented for `seed_rng` --
 * `hw_rng_ref::is_available()` conservatively reports `false` when a
 * backend omits it (see `hw/rng.hpp`) -- callers on genuine Zkr
 * hardware should call `try_generate64()` directly rather than gating
 * on `is_available()`.
 */

#include <structo/hw/rng.hpp>

#include <cstdint>

namespace structo::arch::riscv {

/** @brief Zero-sized `hw_rng_traits` backend tag for the RISC-V Zkr `seed` CSR. */
struct seed_rng {};

} // namespace structo::arch::riscv

namespace structo::hw {

template <> struct hw_rng_traits<structo::arch::riscv::seed_rng> {
  /**
   * @brief Executes `csrrw` against the `seed` CSR (address `0x015`)
   * repeatedly, accumulating four successful 16-bit `ES16` samples into
   * one 64-bit word. Reports `error::try_again` if the per-sample retry
   * budget is exhausted while still seeing `BIST`/`WAIT` (transient,
   * safe to retry the whole draw), or `error::io_error` immediately if
   * any sample reports `DEAD` (unrecoverable). Compiled only for a
   * genuine RISC-V target.
   */
  [[nodiscard]] static result<std::uint64_t> try_generate64(structo::arch::riscv::seed_rng &) noexcept {
#if defined(__riscv)
    constexpr std::uint32_t k_opst_mask = 0xC0000000u;
    constexpr std::uint32_t k_opst_es16 = 0x80000000u;
    constexpr std::uint32_t k_opst_dead = 0xC0000000u;
    constexpr std::uint32_t k_entropy_mask = 0xFFFFu;
    constexpr unsigned k_needed_samples = 4; // 4 x 16 bits = 64 bits
    constexpr unsigned k_retry_loops_per_sample = 100; // matches Linux's SEED_RETRY_LOOPS

    std::uint64_t word = 0;
    for (unsigned sample = 0; sample < k_needed_samples; ++sample) {
      bool sampled = false;
      for (unsigned attempt = 0; attempt < k_retry_loops_per_sample; ++attempt) {
        std::uint32_t csr_value;
        asm volatile("csrrw %0, 0x15, x0" : "=r"(csr_value));
        std::uint32_t const opst = csr_value & k_opst_mask;
        if (opst == k_opst_dead)
          return unexpected(error::io_error);
        if (opst == k_opst_es16) {
          word = (word << 16) | (csr_value & k_entropy_mask);
          sampled = true;
          break;
        }
        // BIST or WAIT: entropy source not ready yet, spin and retry.
      }
      if (!sampled)
        return unexpected(error::try_again);
    }
    return word;
#else
    return unexpected(error::unsupported_operation);
#endif
  }
};

} // namespace structo::hw
