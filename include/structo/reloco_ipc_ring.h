// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

/**
 * @file reloco_ipc_ring.h
 * @brief Lock-free, single-producer/single-consumer (SPSC) cross-process
 * byte ring buffer over a shared memory page. Plain C (C99+, also usable
 * from Linux and FreeBSD kernel space), with an optional C++-only fault
 * injection integration for attack-surface testing.
 *
 * @details
 * This header defines `struct reloco_ipc_spsc_page` -- the strictly
 * standard-layout header + payload region a producer and consumer
 * process (or kernel thread) place in a shared memory mapping (e.g. an
 * `mmap()`ed `shm_open()` file, or a kernel-managed DMA/shared page) --
 * plus a matching pair of `reloco_ipc_producer`/`reloco_ipc_consumer`
 * mount contexts (one per side, never shared between them) and the
 * `reloco_ipc_try_write()`/`reloco_ipc_try_read()` functions that move
 * bytes through it.
 *
 * @par Threat model and security boundary
 * The whole point of this header: `read_idx`/`write_idx` live in memory
 * the *other* process/kernel entity controls and may be malicious, buggy,
 * or simply racing this side without synchronization beyond the
 * acquire/release protocol below. Each side treats the field it does not
 * own as **untrusted input** -- every refresh of that field is funneled
 * through `RELOCO_IPC_LOAD_ACQUIRE_FAULT()` (a single-read-to-local
 * macro that forecloses double-fetch/TOCTOU bugs) and immediately
 * bounds-checked (`in_use > cap` / `available > cap`) before being used
 * to compute any pointer offset, so a spoofed index can only make a
 * transfer fail, never read/write out of bounds. See
 * [Fault injection](../../docs/fault-injection.md) and the
 * `RELOCO_ENABLE_FAULT_INJECTION` block below for how this boundary is
 * itself tested.
 *
 * @par Preconditions / invariants callers must uphold
 * - `capacity` (the payload region's size in bytes) must be a power of
 *   two and at least 2; both sides must independently know and pass the
 *   *same* value as `expected_capacity` -- `reloco_ipc_validate_mount()`
 *   rejects a page whose embedded `untrusted_capacity` disagrees.
 * - Exactly one producer and one consumer may mount a given page at a
 *   time (SPSC only -- no support for multiple concurrent writers or
 *   readers on the same side).
 * - `reloco_ipc_producer_init()`/`reloco_ipc_consumer_init()` must run
 *   only after the page's header fields (`magic`, `version`, `flags`,
 *   `untrusted_capacity`) have been fully written and are visible to
 *   this side (e.g. after the mapping/handshake that hands over the
 *   page); this header does not itself synchronize page *creation*.
 * - All reads/writes of `write_idx`/`read_idx` must go through the
 *   `RELOCO_IPC_LOAD_ACQUIRE`/`RELOCO_IPC_STORE_RELEASE`/
 *   `RELOCO_IPC_LOAD_RELAXED` macros (never a plain load/store) --
 *   these resolve to the correct atomic/barrier primitive for
 *   userspace, Linux kernel (`RELOCO_IPC_LINUX_KERNEL`), or FreeBSD
 *   kernel (`RELOCO_IPC_FREEBSD_KERNEL`) builds.
 * - `reloco_ipc_try_write()`/`reloco_ipc_try_read()` are strictly
 *   all-or-nothing: they transfer exactly the requested count or exactly
 *   `0`, never a partial amount. Their return type,
 *   `reloco_ipc_ssize_t`, is a signed, fixed-width (`int64_t`) counter
 *   distinguishing three outcomes: a positive/zero value is the number
 *   of bytes transferred (`count`/`max_count` on success, `0` if there
 *   simply is not enough room/data *right now* -- an ordinary, expected
 *   condition, not an error, exactly like `EAGAIN`); a **negative**
 *   value (`-EFAULT`) means this side's own `in_use > cap` / `available
 *   > cap` security boundary check tripped -- the peer's index was
 *   spoofed or corrupted. That is not a transient condition: a caller
 *   that sees a negative return should treat the whole shared page as
 *   compromised and stop using it entirely (unmount, tear down, and
 *   fail the whole session), rather than retry.
 *
 * @note This header is usable from plain C, C++, and (with the
 * appropriate `RELOCO_IPC_LINUX_KERNEL`/`RELOCO_IPC_FREEBSD_KERNEL`
 * macro defined before inclusion) kernel space. The optional fault
 * injection block below is the only part that requires C++ and opt-in
 * `RELOCO_ENABLE_FAULT_INJECTION`; it compiles to nothing otherwise. For
 * the C++ RAII/zero-copy wrapper (`reloco::ipc_producer`/
 * `reloco::ipc_consumer`), see `reloco_ipc_ring.hpp` -- there, the same
 * three outcomes are surfaced as `result<size_type>`/`result<write_tx>`/
 * `result<read_tx>` instead: `error::security_violation` for the
 * negative case above, `Ok` (holding `0`/an empty transaction) for the
 * benign "not enough room/data yet" case, and `Ok` (holding the
 * transferred count/a non-empty transaction) on success.
 */

#ifndef RELOCO_IPC_RING_H
#define RELOCO_IPC_RING_H

#ifdef RELOCO_IPC_LINUX_KERNEL
/* Linux Kernel Space */
#include <asm/barrier.h>
#include <linux/bug.h>   /* For BUG_ON */
#include <linux/errno.h> /* For EFAULT */
#include <linux/string.h>
#include <linux/types.h>

#define RELOCO_IPC_LOAD_ACQUIRE(ptr) smp_load_acquire(ptr)
#define RELOCO_IPC_STORE_RELEASE(ptr, val) smp_store_release((ptr), (val))
#define RELOCO_IPC_LOAD_RELAXED(ptr) READ_ONCE(*(ptr))
/* Every RELOCO_IPC_ASSERT below checks an invariant this header's own
 * arithmetic already establishes (see the comments at each call site) --
 * it exists as a debug-time guard against a future refactor breaking
 * that invariant, not as a check on untrusted input. `BUG_ON` is
 * trivially available in Linux kernel space; ports that want a softer
 * failure mode can redefine this (e.g. to `WARN_ON(!(cond))`) before
 * including this header. */
#ifndef RELOCO_IPC_ASSERT
#define RELOCO_IPC_ASSERT(cond) BUG_ON(!(cond))
#endif
#elif defined(RELOCO_IPC_FREEBSD_KERNEL)
/* ---- FreeBSD Kernel Space ---- */
#include <machine/atomic.h>
#include <sys/errno.h> /* For EFAULT */
#include <sys/systm.h> /* For memcpy, MPASS */
#include <sys/types.h>

/* FreeBSD atomic API natively supports acquire/release semantics */
#define RELOCO_IPC_LOAD_ACQUIRE(ptr) atomic_load_acq_64((volatile uint64_t *)(ptr))
#define RELOCO_IPC_STORE_RELEASE(ptr, val) atomic_store_rel_64((volatile uint64_t *)(ptr), (val))
#define RELOCO_IPC_LOAD_RELAXED(ptr) atomic_load_64((volatile uint64_t *)(ptr))
/* See the Linux kernel branch's comment above -- same rationale.
 * `MPASS` is trivially available via <sys/systm.h>, already included
 * above. Ports may still redefine this before including this header. */
#ifndef RELOCO_IPC_ASSERT
#define RELOCO_IPC_ASSERT(cond) MPASS(cond)
#endif
#else
/* User Space or Bare-Metal C/C++ */
#include <assert.h>
#include <errno.h> /* For EFAULT */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Debug-time-only (compiles to nothing under NDEBUG, like plain
 * `assert()`) verification that this header's own overflow-sensitive
 * arithmetic (masking/subtracting already-bounds-checked indices, then
 * narrowing a uint64_t to uint32_t) stays within the invariants each
 * call site's surrounding comment documents. */
#ifndef RELOCO_IPC_ASSERT
#define RELOCO_IPC_ASSERT(cond) assert(cond)
#endif

#define RELOCO_IPC_LOAD_ACQUIRE(ptr) __atomic_load_n((ptr), __ATOMIC_ACQUIRE)
#define RELOCO_IPC_STORE_RELEASE(ptr, val) __atomic_store_n((ptr), (val), __ATOMIC_RELEASE)
#define RELOCO_IPC_LOAD_RELAXED(ptr) __atomic_load_n((ptr), __ATOMIC_RELAXED)
#endif

/**
 * @brief Signed, fixed-width return type for `reloco_ipc_try_write()`/
 * `reloco_ipc_try_read()`. Deliberately `int64_t`, not the platform's own
 * (width-varying, and unavailable on stock MSVC) `ssize_t`, so a
 * transferred-byte count -- itself always representable in a `uint32_t`
 * -- and the small negative `-EFAULT` sentinel both always fit, on every
 * target this header supports (userspace, bare-metal, Linux kernel,
 * FreeBSD kernel).
 */
typedef int64_t reloco_ipc_ssize_t;

/* =========================================================================
 * OPTIONAL FAULT INJECTION HOOKS (C++ only, opt-in)
 * =========================================================================
 * Real-world "attack surface" testing for this ring buffer's two
 * lock-free security boundary checks (`reloco_ipc_try_write`'s consumer
 * index spoofing guard, `reloco_ipc_try_read`'s producer index spoofing
 * guard): a test process shares this same page with an adversarial/buggy
 * peer, so those two checks are the only thing standing between a
 * corrupted `read_idx`/`write_idx` and a miscomputed, possibly
 * out-of-bounds `memcpy`. `fault_injection.hpp` is C++-only, so this
 * whole block -- and every `RELOCO_IPC_FAULT_POINT_ARGS` call site below
 * -- compiles to nothing at all unless both `__cplusplus` and
 * `RELOCO_ENABLE_FAULT_INJECTION` are defined; a plain C build (including
 * every Linux/FreeBSD kernel build) never sees any of it. See [Fault
 * injection](../../docs/fault-injection.md) for the general framework.
 *
 * The C API below (`reloco_ipc_try_write`/`reloco_ipc_try_read`, both
 * `static inline`, hence internal linkage per translation unit) is safe
 * to build with a different `RELOCO_ENABLE_FAULT_INJECTION` setting in
 * each TU. `reloco_ipc_ring.hpp`'s C++ wrapper is not: see its own
 * ODR note at the top of that file before enabling fault injection for
 * any translation unit that uses `ipc_producer`/`ipc_consumer`.
 */
#if defined(__cplusplus) && defined(RELOCO_ENABLE_FAULT_INJECTION)
namespace reloco::ipc_fault {

/**
 * @brief Fires right after the producer refreshes its cached view of the
 * consumer's (shared-memory, untrusted -- the consumer may be a
 * malicious or buggy separate process) `read_idx`, exposing the freshly
 * loaded value by reference. Arm this to simulate the consumer having
 * written an arbitrary value there concurrently, and assert that
 * `reloco_ipc_try_write`/`ipc_producer::begin_write` safely reject it
 * (return `0`/an empty transaction) instead of miscomputing available
 * space.
 */
RELOCO_FAULT_TAG(producer_read_idx_refresh);

/**
 * @brief The consumer-side mirror of `producer_read_idx_refresh`: fires
 * right after the consumer refreshes its cached view of the producer's
 * (shared-memory, untrusted) `write_idx`.
 */
RELOCO_FAULT_TAG(consumer_write_idx_refresh);

} // namespace reloco::ipc_fault

#define RELOCO_IPC_FAULT_POINT_ARGS(Tag, ...) RELOCO_FAULT_POINT_ARGS(::reloco::ipc_fault::Tag, __VA_ARGS__)
#else
#define RELOCO_IPC_FAULT_POINT_ARGS(Tag, ...) ((void)0)
#endif

/**
 * @def RELOCO_IPC_LOAD_ACQUIRE_FAULT(ptr, FaultTag, local)
 * @brief The **only** sanctioned way to read a field out of the shared
 * page that the *other* side of the ring owns (`read_idx` from the
 * producer, `write_idx` from the consumer): reads @p ptr exactly once
 * into @p local (an already-declared local variable), then immediately
 * runs @p FaultTag's fault point on that single, already-captured local
 * -- never re-dereferencing @p ptr a second time to "check" it
 * afterwards. This shared memory is inherently adversarial (a
 * concurrent, possibly malicious or buggy peer process can rewrite it
 * at any instant), so re-reading the same field more than once for what
 * is logically a single decision is a double-fetch/TOCTOU bug: the two
 * reads could each observe a different, individually "valid" value
 * while the pair together is nonsensical. Capturing to a local exactly
 * once, right here, makes that class of bug structurally impossible
 * *and* makes every such read fault-injectable for attack-surface
 * testing (see the two `RELOCO_FAULT_TAG`s above). Any new field read
 * of untrusted, peer-owned shared state added to this header should go
 * through this macro rather than a bare `RELOCO_IPC_LOAD_ACQUIRE(...)`.
 */
#define RELOCO_IPC_LOAD_ACQUIRE_FAULT(ptr, FaultTag, local)                                                            \
  do {                                                                                                                 \
    (local) = RELOCO_IPC_LOAD_ACQUIRE(ptr);                                                                            \
    RELOCO_IPC_FAULT_POINT_ARGS(FaultTag, local);                                                                      \
  } while (0)

#ifdef __cplusplus
extern "C" {
#endif

#define RELOCO_IPC_MAGIC 0x524C434F /* 'RLCO' */
#define RELOCO_IPC_VERSION 1
#define RELOCO_IPC_CACHE_LINE 64
/* Cross-compiler alignment macro */
#if defined(__cplusplus)
#define RELOCO_IPC_ALIGN(x) alignas(x)
#elif defined(__GNUC__) || defined(__clang__)
#define RELOCO_IPC_ALIGN(x) __attribute__((aligned(x)))
#else
#define RELOCO_IPC_ALIGN(x)
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc99-extensions"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

/**
 * @brief Strictly standard-layout shared memory page.
 * @note All synchronization fields are raw integers. The caller MUST use
 * proper atomic built-ins or std::atomic_ref to read/write them.
 */
struct reloco_ipc_spsc_page {
  /* ---- CACHE LINE 0: VALIDATION HEADER ---- */
  uint32_t magic;
  uint32_t version;
  uint32_t flags; /* Reserved, must be 0 (Maintains 16-byte header alignment) */
  uint32_t untrusted_capacity;

  /* ---- CACHE LINE 1: PRODUCER STATE ---- */
  RELOCO_IPC_ALIGN(RELOCO_IPC_CACHE_LINE)
  uint64_t write_idx;

  /* ---- CACHE LINE 2: CONSUMER STATE ---- */
  RELOCO_IPC_ALIGN(RELOCO_IPC_CACHE_LINE)
  uint64_t read_idx;

  /* ---- CACHE LINE 3+: PAYLOAD ---- */
  RELOCO_IPC_ALIGN(RELOCO_IPC_CACHE_LINE)
  uint8_t payload[];
};

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/**
 * @brief Validates the memory page.
 * @return 0 on success, -1 on fatal mismatch.
 */
static inline int reloco_ipc_validate_mount(const struct reloco_ipc_spsc_page *page, uint32_t expected_capacity) {
  if (!page)
    return -1;
  if (page->magic != RELOCO_IPC_MAGIC)
    return -1;
  if (page->version != RELOCO_IPC_VERSION)
    return -1;
  if (page->flags != 0)
    return -1;
  if (page->untrusted_capacity != expected_capacity)
    return -1;
  /* Capacity must be at least 2 bytes and strictly a power of 2 */
  if (expected_capacity < 2 || (expected_capacity & (expected_capacity - 1)) != 0)
    return -1;
  return 0;
}

/* =========================================================================
 * FORMALLY VERIFIED INDEX ARITHMETIC
 *
 * The three helpers below are the only overflow/buffer-safety-sensitive
 * computations `reloco_ipc_try_write()`/`reloco_ipc_try_read()` (and the
 * mirrored `write_slices()`/`read_slices()` in `reloco_ipc_ring.hpp`)
 * perform. They are deliberately pure, side-effect-free, plain-integer
 * functions -- callers never see them; they exist as a proof unit -- so
 * that Frama-C's WP plugin can verify their ACSL contracts directly
 * against *this* shipped code, not merely against a look-alike model.
 * (ACSL annotations, written as specially-marked comments starting with
 * `@`, are ordinary C comments: no compiler other than Frama-C ever
 * looks inside them.)
 *
 * Verify with (from the repository root):
 *   eval $(opam env)   # if Frama-C was installed via opam
 *   frama-c -wp -wp-rte -wp-fct \
 *     reloco_ipc_offset_in_page,reloco_ipc_first_chunk_len,\
 *     reloco_ipc_available_from_in_use \
 *     proofs/ipc_ring_offset_header_driver.c
 *
 * See `proofs/README.md` for the full writeup, including the one
 * deliberately-trusted fact (`mask_is_mod_ax` below) and the one
 * residual SMT-timeout goal.
 * ========================================================================= */

/*@
  // `cap` is a validated power of two, `>= 2` (rejected otherwise by
  // `reloco_ipc_validate_mount()` above).
  predicate reloco_ipc_is_pow2(uint32_t cap) = cap >= 2 && (cap & (cap - 1)) == 0;

  // TRUSTED AXIOM, not a WP proof obligation: "AND-ing with (cap - 1) is
  // exactly modulo `cap`, for any power of two `cap`" is a true, standard
  // bitwise-arithmetic fact -- but it sits outside the decidable
  // fragment this project's SMT backend (Alt-Ergo) handles (confirmed
  // experimentally: even the single *concrete* case `w & 15 == w % 16`
  // times out un-axiomatized). A bit-vector-native solver (e.g. Z3/CVC5
  // via why3) or an inductive Coq proof (over the power's exponent)
  // could derive this from first principles instead of asserting it;
  // neither is wired up in this project. Every other contract below is a
  // genuine, from-scratch WP-discharged proof built *on top* of this one
  // trusted fact -- exactly mirroring this file's own runtime posture,
  // which never trusts the bitwise identity blindly either: see the
  // `RELOCO_IPC_ASSERT(physical < cap)` immediately below, which
  // re-verifies this axiom's consequence on every call, in every build.
  axiomatic RelocoIpcPowerOfTwoMask {
    axiom reloco_ipc_mask_is_mod_ax:
      \forall uint64_t w, uint32_t cap;
        reloco_ipc_is_pow2(cap) ==> (uint32_t)(w & (cap - 1)) == w % cap;
  }
*/

/**
 * @brief Converts a monotonically increasing 64-bit stream index into a
 * physical byte offset into `payload[cap]`. Pure bitwise masking
 * (`idx & (cap - 1)`), formally equivalent to `idx % cap` for the
 * power-of-two `cap` this header requires -- see `reloco_ipc_mask_is_mod_ax`
 * above.
 */
/*@
  requires reloco_ipc_is_pow2(cap);
  assigns \nothing;
  ensures \result == idx % cap;
  ensures 0 <= \result < cap;
*/
static inline uint32_t reloco_ipc_offset_in_page(uint64_t idx, uint32_t cap) {
  uint32_t mask = cap - 1;
  uint32_t physical = (uint32_t)(idx & mask);
  RELOCO_IPC_ASSERT(physical < cap);
  return physical;
}

/**
 * @brief Length of the first (possibly only) contiguous chunk of a
 * transfer of `count` bytes starting at physical offset `physical`,
 * clamped so it never runs past `payload[cap]`. The remainder, if any
 * (`count - result`), wraps and starts over at `payload[0]`.
 */
/*@
  requires reloco_ipc_is_pow2(cap);
  requires physical < cap;
  requires count <= cap;
  assigns \nothing;
  // Buffer-safety: the two memcpy() calls this feeds -- one of
  // `\result` bytes starting at `payload[physical]`, one of
  // `count - \result` bytes starting at `payload[0]` -- together touch
  // exactly `count` bytes and never read/write past `payload[cap]`.
  ensures \result <= count;
  ensures physical + \result <= cap;
  ensures (count - \result) <= cap;
*/
static inline uint32_t reloco_ipc_first_chunk_len(uint32_t physical, uint32_t cap, uint32_t count) {
  uint32_t first_chunk = cap - physical;
  if (first_chunk > count)
    first_chunk = count;
  return first_chunk;
}

/**
 * @brief Narrows the already security-boundary-checked `in_use`/`cap`
 * difference (`cap - in_use`, both known `<= cap`) to `uint32_t` without
 * loss -- the "available space/data" value both `reloco_ipc_try_write()`
 * and `reloco_ipc_try_read()` (and their `.hpp` `*_slices()` mirrors)
 * compare against the caller's requested count.
 */
/*@
  requires reloco_ipc_is_pow2(cap);
  requires in_use <= cap;
  assigns \nothing;
  ensures \result <= cap;
  ensures in_use + \result == cap;
*/
static inline uint32_t reloco_ipc_available_from_in_use(uint64_t in_use, uint32_t cap) {
  uint32_t available = (uint32_t)(cap - in_use);
  RELOCO_IPC_ASSERT(available <= cap);
  return available;
}

/* =========================================================================
 * PRODUCER API (Writer Context)
 * ========================================================================= */

struct reloco_ipc_producer {
  struct reloco_ipc_spsc_page *page;
  uint64_t cached_read_idx;
  uint32_t capacity; /* Trusted, local copy */
};

/**
 * @brief Mounts the page for writing.
 */
static inline int reloco_ipc_producer_init(struct reloco_ipc_producer *p, struct reloco_ipc_spsc_page *page,
                                           const uint32_t expected_capacity) {
  if (reloco_ipc_validate_mount(page, expected_capacity) != 0)
    return -1;
  p->page = page;
  RELOCO_IPC_LOAD_ACQUIRE_FAULT(&page->read_idx, producer_read_idx_refresh, p->cached_read_idx);
  /* Lock the verified constants into local trusted memory */
  p->capacity = expected_capacity;
  return 0;
}

/**
 * @brief Attempts to write `count` elements. Strictly all-or-nothing.
 * @return `count` if the whole write succeeded; `0` if there is not
 * currently enough free space (benign -- retry later); a **negative**
 * value (`-EFAULT`) if the consumer's `read_idx` was found to be
 * spoofed/corrupted (the security boundary tripped) -- treat the ring as
 * compromised and stop using it, do not retry.
 */
static inline reloco_ipc_ssize_t reloco_ipc_try_write(struct reloco_ipc_producer *p, const void *data, uint32_t count) {
  struct reloco_ipc_spsc_page *page = p->page;
  uint64_t w = RELOCO_IPC_LOAD_RELAXED(&page->write_idx);
  uint64_t r = p->cached_read_idx;
  uint32_t cap = p->capacity;
  uint32_t available;
  uint32_t physical_w;
  uint32_t first_chunk;
  const uint8_t *src;

  uint64_t in_use = w - r;

  /* Demand-driven cache refresh */
  if (in_use >= cap || cap - in_use < count) {
    RELOCO_IPC_LOAD_ACQUIRE_FAULT(&page->read_idx, producer_read_idx_refresh, r);
    p->cached_read_idx = r;
    in_use = w - r;
  }

  /* IPC SECURITY BOUNDARY: Detect malicious consumer index spoofing */
  if (in_use > cap) {
    return -EFAULT; /* Ring compromised: Consumer advanced read_idx beyond write_idx */
  }

  /* Formally verified (see `reloco_ipc_available_from_in_use()` above,
   * and proofs/README.md): `in_use <= cap` was just checked, so this
   * narrowing to uint32_t is lossless. */
  available = reloco_ipc_available_from_in_use(in_use, cap);
  if (available < count) {
    return 0; /* Not enough space for all-or-nothing write (benign) */
  }

  /* Formally verified (see `reloco_ipc_offset_in_page()`/
   * `reloco_ipc_first_chunk_len()` above, and proofs/README.md):
   * `count <= available <= cap` (both checks above), and `cap` is a
   * validated power of two (>= 2), so `physical_w` is strictly less
   * than `cap`, meaning `first_chunk` cannot underflow and always fits
   * in uint32_t (bounded by `cap`, itself a uint32_t). */
  physical_w = reloco_ipc_offset_in_page(w, cap);
  first_chunk = reloco_ipc_first_chunk_len(physical_w, cap, count);

  src = (const uint8_t *)data;

  memcpy(page->payload + physical_w, src, first_chunk);
  if (first_chunk < count) {
    memcpy(page->payload, src + first_chunk, (count - first_chunk));
  }

  /* Commit write (Release ensures memory writes are visible before idx update) */
  RELOCO_IPC_STORE_RELEASE(&page->write_idx, w + count);
  return (reloco_ipc_ssize_t)count;
}

/* =========================================================================
 * CONSUMER API (Reader Context)
 * ========================================================================= */

struct reloco_ipc_consumer {
  struct reloco_ipc_spsc_page *page;
  uint64_t cached_write_idx;
  uint32_t capacity; /* Trusted, local copy */
};

/**
 * @brief Mounts the page for reading.
 */
static inline int reloco_ipc_consumer_init(struct reloco_ipc_consumer *c, struct reloco_ipc_spsc_page *page,
                                           uint32_t expected_capacity) {
  if (reloco_ipc_validate_mount(page, expected_capacity) != 0)
    return -1;
  c->page = page;
  RELOCO_IPC_LOAD_ACQUIRE_FAULT(&page->write_idx, consumer_write_idx_refresh, c->cached_write_idx);
  /* Lock the verified constants into local trusted memory */
  c->capacity = expected_capacity;
  return 0;
}

/**
 * @brief Reads exactly `max_count` elements into `dest`. Strictly
 * all-or-nothing (like `reloco_ipc_try_write()` -- a bare, no-copy
 * partial read is not exposed here).
 * @return `max_count` if that many bytes were genuinely available and
 * were read; `0` if there is not currently enough data (benign -- retry
 * later); a **negative** value (`-EFAULT`) if the producer's `write_idx`
 * was found to be spoofed/corrupted (the security boundary tripped) --
 * treat the ring as compromised and stop using it, do not retry.
 */
static inline reloco_ipc_ssize_t reloco_ipc_try_read(struct reloco_ipc_consumer *c, void *dest, uint32_t max_count) {
  struct reloco_ipc_spsc_page *page = c->page;
  uint64_t r = RELOCO_IPC_LOAD_RELAXED(&page->read_idx);
  uint64_t w = c->cached_write_idx;
  uint32_t cap;
  uint32_t physical_r;
  uint32_t first_chunk;
  uint8_t *dst;
  uint64_t available = w - r;

  cap = c->capacity;

  /* Demand-driven cache refresh */
  if (available < max_count) {
    RELOCO_IPC_LOAD_ACQUIRE_FAULT(&page->write_idx, consumer_write_idx_refresh, w);
    c->cached_write_idx = w;
    available = w - r;
  }

  /* IPC SECURITY BOUNDARY: Detect malicious producer index spoofing */
  if (available > cap) {
    return -EFAULT; /* Ring compromised: Producer advanced write_idx out of bounds */
  }

  if (available < max_count) {
    return 0; /* Not enough data for all-or-nothing read (benign) */
  }

  /* Formally verified (see `reloco_ipc_offset_in_page()`/
   * `reloco_ipc_first_chunk_len()` above, and proofs/README.md):
   * `max_count <= available <= cap` (both checks above), and `cap` is a
   * validated power of two (>= 2), so `physical_r` is strictly less
   * than `cap`, meaning `first_chunk` cannot underflow. */
  physical_r = reloco_ipc_offset_in_page(r, cap);
  first_chunk = reloco_ipc_first_chunk_len(physical_r, cap, max_count);

  dst = (uint8_t *)dest;

  memcpy(dst, page->payload + physical_r, first_chunk);
  if (first_chunk < max_count) {
    memcpy(dst + first_chunk, page->payload, (max_count - first_chunk));
  }

  /* Commit read (Release ensures we are done reading memory before recycling slot) */
  RELOCO_IPC_STORE_RELEASE(&page->read_idx, r + max_count);
  return (reloco_ipc_ssize_t)max_count;
}

#ifdef __cplusplus
}
#endif

#endif /* RELOCO_IPC_RING_H */