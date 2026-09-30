#pragma once

/**
 * @file reloco_ipc_ring.hpp
 * @brief C++ RAII / zero-copy wrapper around `reloco_ipc_ring.h`'s
 * lock-free, single-producer/single-consumer (SPSC) cross-process byte
 * ring buffer.
 *
 * @details
 * `ipc_producer`/`ipc_consumer` mount the same `reloco_ipc_spsc_page`
 * shared memory page `reloco_ipc_ring.h` defines (`create()` wraps
 * `reloco_ipc_producer_init()`/`reloco_ipc_consumer_init()`, returning a
 * `result<T>`/`expected<T, error>` instead of an `int` status code), and
 * offer two ways to move bytes:
 * - `try_write()`/`try_read()`: a thin, copying wrapper over
 *   `reloco_ipc_try_write()`/`reloco_ipc_try_read()`.
 * - `begin_write()`/`begin_read()`: a zero-copy transaction
 *   (`write_tx`/`read_tx`) exposing up to two contiguous `span`s (a
 *   wrap-around write/read straddles the end of the payload region as
 *   two chunks) to fill/consume in place, followed by an explicit
 *   `commit()`/`consume(bytes)` call that publishes exactly how many of
 *   those bytes were actually used. Both transaction types carry
 *   `RELOCO_CONSUMABLE`/typestate annotations so a compiler-enforced
 *   warning fires if a transaction is destroyed without being committed
 *   or explicitly discarded.
 *
 * See `reloco_ipc_ring.h`'s own file-level documentation for the full
 * threat model, preconditions, and invariants (power-of-two capacity,
 * SPSC-only, acquire/release discipline, all-or-nothing transfers) --
 * they apply identically here, since this wrapper is a thin layer over
 * the same underlying page and functions.
 *
 * @warning **ODR (One Definition Rule):** `RELOCO_ENABLE_FAULT_INJECTION`
 * changes the *body* of `ipc_producer::write_slices()`/
 * `ipc_consumer::read_slices()` below (adding fault-point calls); as
 * implicitly-inline member functions they have external/vague linkage,
 * so every translation unit in the same program that includes this
 * header must define `RELOCO_ENABLE_FAULT_INJECTION` identically (all or
 * nothing). Mixing translation units that do and don't define it is an
 * ODR violation: the linker silently keeps only one of the two differing
 * bodies and uses it for every TU, so a fault-injection-instrumented TU
 * can end up silently running the *other* TU's non-instrumented code
 * instead of failing to build. Keep any fault-injection-enabled
 * translation unit in its own, separate binary from ordinary
 * (non-instrumented) ones -- see `tests/test_ipc_ring_fault_injection.cpp`'s
 * own dedicated CMake target for a worked example. (By contrast, the
 * plain C API in `reloco_ipc_ring.h` is `static inline`, hence internal
 * linkage per TU, and is safe to mix.)
 */
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>
#include <cstdlib>
#include <type_traits>

// Pulled in here, before RELOCO_BEGIN_UNSAFE_BUFFER_USAGE opens below,
// so its own transitive dependencies (e.g. value_ref.hpp) can safely
// open/close their own unsafe-buffer-usage pragma region without
// nesting inside this header's: reloco_ipc_ring.h's own conditional
// `#include "fault_injection.hpp"` becomes a no-op `#pragma once` re-hit.
//
// IMPORTANT (ODR): RELOCO_ENABLE_FAULT_INJECTION changes the *body* of
// ipc_producer::write_slices()/ipc_consumer::read_slices() below (adding
// the fault-point calls); as implicitly-inline member functions they
// have external/vague linkage, so every translation unit in the same
// program that includes this header must define
// RELOCO_ENABLE_FAULT_INJECTION identically (all or nothing) -- mixing
// TUs that do and don't define it is a One Definition Rule violation:
// the linker keeps only one of the two differing bodies and silently
// uses it for every TU, so a fault-injection-instrumented TU can end up
// silently running the *other* TU's non-instrumented code instead of
// failing to build. Keep any fault-injection-enabled translation unit
// in its own, separate binary from ordinary (non-instrumented) ones
// (see tests/test_ipc_ring_fault_injection.cpp's own dedicated CMake
// target for a worked example).
#if defined(RELOCO_ENABLE_FAULT_INJECTION)
#include <reloco/fault_injection.hpp>
#endif

// reloco_ipc_ring.h defaults RELOCO_IPC_ASSERT to plain libc assert()
// for userspace/bare-metal builds (see that header's own comment). Since
// this wrapper already depends on error.hpp/expected.hpp -- which pulls
// in detail/assert.hpp -- route the C header's own overflow-verification
// asserts (in reloco_ipc_try_write()/reloco_ipc_try_read()) through the
// same RELOCO_ASSERT machinery this wrapper uses for its own equivalent
// checks in write_slices()/read_slices(), for one consistent
// failure-reporting/handler story across both files. Must be defined
// before reloco_ipc_ring.h is first included (below).
#ifndef RELOCO_IPC_ASSERT
#define RELOCO_IPC_ASSERT(cond) RELOCO_ASSERT(cond, "reloco_ipc_ring: invariant violated")
#endif

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
#include "reloco_ipc_ring.h"

// This wrapper is C++-only (userspace), unlike reloco_ipc_ring.h which also
// supports plain C and kernel builds -- so, unlike that header's separate
// LOAD_RELAXED-then-STORE_RELEASE pair (kept there for portability to
// backends without a generic RMW builtin), the index-commit path here can
// use a single atomic read-modify-write. This removes the (currently
// harmless, single-writer-only) window between reading the old index and
// storing the new one, and better documents that this line is *the*
// linearization point for this side of the SPSC handoff.
#define RELOCO_IPC_FETCH_ADD_RELEASE(ptr, val) __atomic_fetch_add((ptr), (val), __ATOMIC_RELEASE)

namespace reloco {

// =========================================================================
// IPC PRODUCER (Writer Domain)
// =========================================================================

/** @brief C++ producer interface for the IPC single-producer ring. */
class RELOCO_POINTER ipc_producer {
  reloco_ipc_producer ctx_{};

  constexpr ipc_producer() noexcept = default;

public:
  using size_type = uint32_t;

  /**
   * @brief Mounts a shared memory page for writing.
   * @param mapped_page Raw pointer to the shared memory region.
   * @param expected_capacity_bytes The capacity verified by the OS mapping size.
   * @return expected containing the mounted producer, or an error on ABI/tampering mismatch.
   */
  [[nodiscard]] static result<ipc_producer> create(void *mapped_page, uint32_t expected_capacity_bytes) noexcept {
    ipc_producer p;
    if (reloco_ipc_producer_init(&p.ctx_, static_cast<reloco_ipc_spsc_page *>(mapped_page), expected_capacity_bytes) !=
        0) {
      return unexpected(error::invalid_argument);
    }
    return p;
  }

  // Disable Copy
  ipc_producer(const ipc_producer &) = delete;
  ipc_producer &operator=(const ipc_producer &) = delete;

  // Enable Move (Required to return via reloco::result)
  ipc_producer(ipc_producer &&) noexcept = default;
  ipc_producer &operator=(ipc_producer &&) noexcept = default;

  // ---- Basic API ----

  /**
   * @brief Attempts an all-or-nothing write of `data.size()` bytes.
   * @return `Ok` holding the number of bytes written -- `data.size()` on
   * success, or `0` if there is not currently enough free space (benign,
   * retryable). `Err(error::security_violation)` if the consumer's
   * index was found to be spoofed/corrupted -- the ring is compromised
   * and must not be used again.
   */
  [[nodiscard]] result<size_type> try_write(span<const uint8_t> data) & noexcept {
    reloco_ipc_ssize_t rc = reloco_ipc_try_write(&ctx_, data.data(), static_cast<uint32_t>(data.size()));
    if (rc < 0) {
      return unexpected(error::security_violation);
    }
    return static_cast<size_type>(rc);
  }

  // ---- RAII Zero-Copy Write Transaction ----

  /** @brief RAII zero-copy transaction for writing one IPC message. */
  class RELOCO_POINTER RELOCO_CONSUMABLE(unconsumed) write_tx {
    ipc_producer *p_;
    std::pair<span<uint8_t>, span<uint8_t>> spans_;

    friend class ipc_producer;
    write_tx(ipc_producer *p, std::pair<span<uint8_t>, span<uint8_t>> spans) noexcept
        RELOCO_RETURN_TYPESTATE(unconsumed)
        : p_(p), spans_(spans) {}

  public:
    write_tx(const write_tx &) = delete;
    write_tx &operator=(const write_tx &) = delete;
    write_tx(write_tx &&other) noexcept RELOCO_RETURN_TYPESTATE(unconsumed) : p_(other.p_), spans_(other.spans_) {
      other.p_ = nullptr;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept RELOCO_TEST_TYPESTATE(unconsumed) {
      return p_ != nullptr && (!spans_.first.empty());
    }

    [[nodiscard]] span<uint8_t> chunk1() const noexcept RELOCO_LIFETIMEBOUND RELOCO_CALLABLE_WHEN(unconsumed) {
      return spans_.first;
    }
    [[nodiscard]] span<uint8_t> chunk2() const noexcept RELOCO_LIFETIMEBOUND RELOCO_CALLABLE_WHEN(unconsumed) {
      return spans_.second;
    }

    void commit(size_type bytes) noexcept RELOCO_SET_TYPESTATE(consumed) {
      if (p_) {
        p_->commit(bytes);
        p_ = nullptr;
      }
    }
  };

  [[nodiscard]] result<write_tx> begin_write(size_type min_bytes = 1) & noexcept RELOCO_LIFETIMEBOUND {
    auto slices = write_slices(min_bytes);
    if (!slices) {
      return unexpected(slices.error());
    }
    return write_tx(this, *slices);
  }

private:
  /**
   * @brief Computes the up-to-two contiguous spans currently available
   * to write (bounded by `cap`, not clamped to `min_bytes`).
   * @return `Ok` holding the (possibly empty, if there is not currently
   * enough free space -- benign) span pair. `Err(error::security_violation)`
   * if the consumer's index was found to be spoofed/corrupted.
   */
  [[nodiscard]] result<std::pair<span<uint8_t>, span<uint8_t>>>
  write_slices(size_type min_bytes) & noexcept RELOCO_LIFETIMEBOUND {
    uint64_t w = RELOCO_IPC_LOAD_RELAXED(&ctx_.page->write_idx);
    uint64_t r = ctx_.cached_read_idx;
    uint32_t cap = ctx_.capacity;
    uint64_t in_use = w - r;

    if (in_use >= cap || cap - in_use < min_bytes) {
      RELOCO_IPC_LOAD_ACQUIRE_FAULT(&ctx_.page->read_idx, producer_read_idx_refresh, r);
      ctx_.cached_read_idx = r;
      in_use = w - r;
    }

    // IPC SECURITY BOUNDARY: Detect malicious consumer index spoofing.
    if (in_use > cap) {
      return unexpected(error::security_violation);
    }

    if (cap - in_use < min_bytes) {
      return std::pair<span<uint8_t>, span<uint8_t>>{}; // Benign: not enough free space yet.
    }

    // Formally verified (see `reloco_ipc_available_from_in_use()`/
    // `reloco_ipc_offset_in_page()`/`reloco_ipc_first_chunk_len()` in
    // reloco_ipc_ring.h, and proofs/README.md): `in_use <= cap` was just
    // checked, so the narrowing to uint32_t is lossless, and
    // `physical_w` is strictly less than the validated power-of-two
    // `cap`, so `cap - physical_w` cannot underflow.
    uint32_t available = reloco_ipc_available_from_in_use(in_use, cap);
    uint32_t physical_w = reloco_ipc_offset_in_page(w, cap);
    uint32_t first_chunk = reloco_ipc_first_chunk_len(physical_w, cap, available);

    uint8_t *payload = ctx_.page->payload;
    span<uint8_t> s1(payload + physical_w, first_chunk);
    span<uint8_t> s2;
    if (first_chunk < available) {
      s2 = span<uint8_t>(payload, available - first_chunk);
    }
    return std::pair<span<uint8_t>, span<uint8_t>>{s1, s2};
  }

  void commit(size_type bytes) & noexcept { RELOCO_IPC_FETCH_ADD_RELEASE(&ctx_.page->write_idx, bytes); }
};

// =========================================================================
// IPC CONSUMER (Reader Domain)
// =========================================================================

/** @brief C++ consumer interface for the IPC single-consumer ring. */
class RELOCO_POINTER ipc_consumer {
  reloco_ipc_consumer ctx_{};

  // Private constructor
  ipc_consumer() noexcept = default;

public:
  using size_type = uint32_t;

  [[nodiscard]] static expected<ipc_consumer, error> create(void *mapped_page,
                                                            uint32_t expected_capacity_bytes) noexcept {
    ipc_consumer c;
    if (reloco_ipc_consumer_init(&c.ctx_, static_cast<reloco_ipc_spsc_page *>(mapped_page), expected_capacity_bytes) !=
        0) {
      return unexpected(error::invalid_argument);
    }
    return c;
  }

  // Disable Copy
  ipc_consumer(const ipc_consumer &) = delete;
  ipc_consumer &operator=(const ipc_consumer &) = delete;

  // Enable Move
  ipc_consumer(ipc_consumer &&) noexcept = default;
  ipc_consumer &operator=(ipc_consumer &&) noexcept = default;

  // ---- Basic API ----

  /**
   * @brief Attempts an all-or-nothing read of `dest.size()` bytes.
   * @return `Ok` holding the number of bytes read -- `dest.size()` on
   * success, or `0` if there is not currently enough data (benign,
   * retryable). `Err(error::security_violation)` if the producer's
   * index was found to be spoofed/corrupted -- the ring is compromised
   * and must not be used again.
   */
  [[nodiscard]] result<size_type> try_read(span<uint8_t> dest) & noexcept {
    reloco_ipc_ssize_t rc = reloco_ipc_try_read(&ctx_, dest.data(), static_cast<uint32_t>(dest.size()));
    if (rc < 0) {
      return unexpected(error::security_violation);
    }
    return static_cast<size_type>(rc);
  }

  // ---- RAII Zero-Copy Read Transaction ----

  /** @brief RAII zero-copy transaction for reading one IPC message. */
  class RELOCO_POINTER RELOCO_CONSUMABLE(unconsumed) read_tx {
    ipc_consumer *c_;
    std::pair<span<const uint8_t>, span<const uint8_t>> spans_;

    friend class ipc_consumer;
    read_tx(ipc_consumer *c, std::pair<span<const uint8_t>, span<const uint8_t>> spans) noexcept
        RELOCO_RETURN_TYPESTATE(unconsumed)
        : c_(c), spans_(spans) {}

  public:
    read_tx(const read_tx &) = delete;
    read_tx &operator=(const read_tx &) = delete;
    read_tx(read_tx &&other) noexcept RELOCO_RETURN_TYPESTATE(unconsumed) : c_(other.c_), spans_(other.spans_) {
      other.c_ = nullptr;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept RELOCO_TEST_TYPESTATE(unconsumed) {
      return c_ != nullptr && (!spans_.first.empty());
    }

    [[nodiscard]] span<const uint8_t> chunk1() const noexcept RELOCO_LIFETIMEBOUND RELOCO_CALLABLE_WHEN(unconsumed) {
      return spans_.first;
    }
    [[nodiscard]] span<const uint8_t> chunk2() const noexcept RELOCO_LIFETIMEBOUND RELOCO_CALLABLE_WHEN(unconsumed) {
      return spans_.second;
    }

    void consume(size_type bytes) noexcept RELOCO_SET_TYPESTATE(consumed) {
      if (c_) {
        c_->consume(bytes);
        c_ = nullptr;
      }
    }
  };

  [[nodiscard]] result<read_tx> begin_read(size_type min_bytes = 1) & noexcept RELOCO_LIFETIMEBOUND {
    auto slices = read_slices(min_bytes);
    if (!slices) {
      return unexpected(slices.error());
    }
    return read_tx(this, *slices);
  }

private:
  /**
   * @brief Computes the up-to-two contiguous spans currently available
   * to read (bounded by `cap`, not clamped to `min_bytes`).
   * @return `Ok` holding the (possibly empty, if there is not currently
   * enough data -- benign) span pair. `Err(error::security_violation)`
   * if the producer's index was found to be spoofed/corrupted.
   */
  [[nodiscard]] result<std::pair<span<const uint8_t>, span<const uint8_t>>>
  read_slices(size_type min_bytes) & noexcept RELOCO_LIFETIMEBOUND {
    uint64_t r = RELOCO_IPC_LOAD_RELAXED(&ctx_.page->read_idx);
    uint64_t w = ctx_.cached_write_idx;
    uint32_t cap = ctx_.capacity;
    uint64_t available = w - r;

    if (available < min_bytes) {
      RELOCO_IPC_LOAD_ACQUIRE_FAULT(&ctx_.page->write_idx, consumer_write_idx_refresh, w);
      ctx_.cached_write_idx = w;
      available = w - r;
    }

    // IPC SECURITY BOUNDARY: Detect malicious producer index spoofing.
    if (available > cap) {
      return unexpected(error::security_violation);
    }

    if (available < min_bytes) {
      return std::pair<span<const uint8_t>, span<const uint8_t>>{}; // Benign: not enough data yet.
    }

    // Formally verified (see `reloco_ipc_offset_in_page()`/
    // `reloco_ipc_first_chunk_len()` in reloco_ipc_ring.h, and
    // proofs/README.md): `available <= cap` (checked above) fits
    // losslessly in uint32_t; `physical_r` is strictly less than the
    // validated power-of-two `cap`, so `cap - physical_r` cannot
    // underflow.
    RELOCO_ASSERT(available <= cap, "reloco_ipc_ring: available must not exceed capacity");
    uint32_t available32 = static_cast<uint32_t>(available);
    uint32_t physical_r = reloco_ipc_offset_in_page(r, cap);
    uint32_t first_chunk = reloco_ipc_first_chunk_len(physical_r, cap, available32);

    const uint8_t *payload = ctx_.page->payload;
    span<const uint8_t> s1(payload + physical_r, first_chunk);
    span<const uint8_t> s2;
    if (first_chunk < available32) {
      s2 = span<const uint8_t>(payload, available32 - first_chunk);
    }
    return std::pair<span<const uint8_t>, span<const uint8_t>>{s1, s2};
  }

  void consume(size_type bytes) & noexcept { RELOCO_IPC_FETCH_ADD_RELEASE(&ctx_.page->read_idx, bytes); }
};

} // namespace reloco
RELOCO_END_UNSAFE_BUFFER_USAGE
