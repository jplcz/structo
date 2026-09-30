/* SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* Frama-C/WP model of the overflow-sensitive index arithmetic in
 * reloco_ipc_ring.h's reloco_ipc_try_write()/reloco_ipc_try_read()
 * (the `physical_w`/`physical_r` masking and `first_chunk` subtraction
 * sites, each currently guarded at runtime by RELOCO_IPC_ASSERT).
 *
 * This is NOT the header itself (WP cannot reason through the atomic
 * macros/memcpy/struct layout there) -- it is a standalone, pure model
 * of just the index math, extracted so an SMT-backed proof checker can
 * verify the safety claims the header's comments currently only assert
 * in prose.
 *
 * Verify with:
 *   frama-c -wp -wp-rte proofs/ipc_ring_offset.c
 */

#include <stdint.h>

/*@
  // `cap` is a validated power of two, `>= 2` (reloco_ipc_ring.h rejects
  // capacity 0/1 and any non-power-of-two value at mount time -- see
  // reloco_ipc_validate_mount()).
  predicate is_pow2(uint32_t cap) = cap >= 2 && (cap & (cap - 1)) == 0;

  // TRUSTED AXIOM, not a WP proof obligation: "AND-ing with (cap - 1) is
  // exactly modulo `cap`, for any power of two `cap`" is a true, standard
  // bitwise-arithmetic fact -- but it sits outside the decidable
  // fragment Alt-Ergo's linear-arithmetic/uninterpreted-function core
  // handles (confirmed experimentally: even the single *concrete* case
  // `w & 15 == w % 16` times out un-axiomatized). A bit-vector-native
  // solver (e.g. Z3/CVC5 via why3) or an inductive Coq proof (over the
  // power's exponent) could derive this from first principles instead of
  // asserting it; neither is wired up here. Every other lemma in this
  // file is a genuine, from-scratch WP-discharged proof built *on top*
  // of this one trusted fact -- exactly mirroring reloco_ipc_ring.h's
  // own design, which never trusts the bitwise identity blindly either:
  // it re-verifies its consequence (`physical_w < cap`) with a runtime
  // RELOCO_IPC_ASSERT right after computing the mask.
  axiomatic PowerOfTwoMask {
    axiom mask_is_mod_ax:
      \forall uint64_t w, uint32_t cap;
        is_pow2(cap) ==> (uint32_t)(w & (cap - 1)) == w % cap;
  }
*/

/*@
  requires is_pow2(cap);
  assigns \nothing;
  // The central bitwise-masking lemma this module exists to check:
  // AND-ing with (cap - 1) is exactly modulo `cap`, for any power of two
  // `cap` and any 64-bit index `w` -- this is what lets the ring buffer
  // convert a monotonically increasing 64-bit stream index into a
  // physical byte offset with a single instruction and no branch.
  // (Machine-checked corollary of the `mask_is_mod_ax` axiom above --
  // WP discharges this one automatically once that fact is in scope.)
  ensures \result == w % cap;
  ensures 0 <= \result < cap;
*/
uint32_t mask_is_mod(uint64_t w, uint32_t cap) {
  uint32_t mask = cap - 1;
  //@ assert mask_is_mod_ax: (uint32_t)(w & mask) == w % cap;
  return (uint32_t)(w & mask);
}

/*@
  requires is_pow2(cap);
  requires physical_w < cap;
  assigns \nothing;
  // The `first_chunk = cap - physical_w` subtraction at each call site:
  // given the masking lemma above (physical_w is always < cap), this
  // can never underflow, and the "first contiguous chunk" it computes
  // never runs past the end of the `payload[cap]` array.
  ensures 0 < \result <= cap;
  ensures physical_w + \result == cap;
*/
uint32_t first_chunk_len(uint32_t physical_w, uint32_t cap) { return cap - physical_w; }

/*@
  requires is_pow2(cap);
  requires physical_w < cap;
  requires 0 < count <= cap;
  // Mirrors the `if (first_chunk > count) first_chunk = count;` clamp
  // applied at both reloco_ipc_try_write()'s and reloco_ipc_try_read()'s
  // call sites.
  assigns \nothing;
  // Buffer-safety: the two memcpy() calls this feeds -- one of
  // `\result` bytes starting at `payload[physical_w]`, one of
  // `count - \result` bytes starting at `payload[0]` -- together touch
  // exactly `count` bytes and never write/read past `payload[cap]`.
  ensures \result <= count;
  ensures physical_w + \result <= cap;
  ensures (count - \result) <= cap;
*/
uint32_t clamped_first_chunk(uint32_t physical_w, uint32_t cap, uint32_t count) {
  uint32_t first_chunk = cap - physical_w;
  if (first_chunk > count) {
    first_chunk = count;
  }
  return first_chunk;
}

/*@
  requires is_pow2(cap);
  requires in_use <= cap;
  assigns \nothing;
  // The `available = cap - in_use` step in both functions (after the
  // "IPC SECURITY BOUNDARY" check `in_use > cap` has already rejected
  // the corrupted case) -- proves the uint64_t-to-uint32_t narrowing
  // immediately after it (`static_cast<uint32_t>(available)` in the
  // C++ wrapper, implicit in the C API) is lossless.
  ensures \result <= cap;
  ensures in_use + \result == cap;
*/
uint32_t available_from_in_use(uint64_t in_use, uint32_t cap) { return (uint32_t)(cap - in_use); }
