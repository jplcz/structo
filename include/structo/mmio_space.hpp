// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmio_space.hpp
 * @brief `structo::mmio_space_backend`: a portable, `volatile`-pointer
 * `io_space_traits` backend for a plain memory-mapped I/O window,
 * bindable through `structo::io_space_ref`. Works identically on
 * x86, x86-64, RISC-V, ARM, and ARM64 -- no inline assembly, no
 * per-architecture header.
 *
 * `io_address.hpp` is pure address tagging; `io_space_ref.hpp` is the
 * type-erased access layer and its `io_space_traits<Backend>`
 * customization point. `arch/x86/port_io_space.hpp` supplies a backend
 * for x86's separate port-I/O space; this header supplies the other
 * common case -- a conventional memory-mapped register window, reached
 * through ordinary loads/stores (through a `volatile` pointer, so the
 * compiler can't elide/reorder/coalesce them) rather than dedicated
 * `IN`/`OUT`-style instructions:
 *
 * @code
 * using lsr_addr = structo::io_address<std::uint8_t, structo::device_io_space>;
 *
 * // `window` is some non-owning volatile* already mapped by the caller
 * // (e.g. the result of mapping a PCI BAR or a device-tree `reg` range).
 * structo::mmio_space_backend backend(window, window_size);
 * structo::io_space_ref<structo::device_io_space> regs(backend);
 *
 * auto status = regs.read(lsr_addr{0x04});
 * if (status && (*status & 0x01))
 *   (void)regs.write(lsr_addr{0x00}, static_cast<std::uint8_t>(0xFF));
 * @endcode
 *
 * ## Addressing: `addr` is a byte offset from `base`, not an absolute pointer
 *
 * The incoming "wire" `std::uint64_t addr` (from `io_address::value`) is
 * added to the backend's own `base` pointer at every access -- mirroring
 * FreeBSD's `bus_space_tag_t`/`bus_space_handle_t` split (a handle plus
 * caller-chosen offsets) and the worked combined port/MMIO example in
 * `docs/io_space_ref.md`. A caller that already has an absolute pointer
 * can simply construct with `base == nullptr` conceptually shifted out,
 * i.e. pass the real base once at construction and only ever use small,
 * device-relative offsets as `io_address` values thereafter.
 *
 * ## Bounds checking
 *
 * If constructed with a nonzero `size`, every access is checked against
 * `[0, size)` and reports `reloco::error::out_of_range` on overflow --
 * mirroring `tests/test_io_space_ref.cpp`'s `fake_mmio` backend.
 * Constructing with `size == 0` (the default) disables the check (an
 * unknown/unbounded window); the caller is then fully responsible for
 * not walking off the end of the real mapping.
 *
 * ## No implicit memory barriers
 *
 * Each access is exactly one `volatile` load or store of the requested
 * width -- nothing more. Like FreeBSD's `bus_space_barrier()` and
 * Linux's `wmb()`/`rmb()`, hardware completion-ordering around a batch
 * of accesses (when the target device's semantics require it) is the
 * caller's responsibility, not something silently inserted after every
 * single register access: pair with the appropriate
 * `structo::arch::{x86,arm,arm64,riscv}::barrier_traits` call (see each
 * architecture's own `barrier.hpp`) where the device actually needs one -- usually
 * far less often than once per access.
 *
 * ## Alignment
 *
 * No alignment check is performed: a misaligned `base + addr` for a
 * given access width is passed straight to a `volatile` dereference of
 * that width. Behavior then follows the target architecture's own
 * unaligned-access rules for ordinary loads/stores (e.g. slower but
 * permitted on x86; may fault with `SIGBUS` or the kernel-mode
 * equivalent on stricter-alignment architectures/configurations) --
 * exactly as an equivalent raw `volatile` pointer cast would behave
 * without this library involved at all.
 */

#include "io_space_ref.hpp"

#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>

#include <cstddef>
#include <cstdint>

namespace structo {

/**
 * @brief Non-owning `io_space_traits` backend for a plain memory-mapped
 * I/O window: every access is one `volatile` load/store of the
 * requested width at `base + addr`. See the @file-level docs for the
 * addressing, bounds-checking, barrier, and alignment conventions.
 */
class mmio_space_backend {
public:
  constexpr mmio_space_backend() noexcept = default;

  /**
   * @brief Binds to a window of @p size bytes starting at @p base
   * (already mapped by the caller). `size == 0` (the default) disables
   * bounds checking -- an unknown/unbounded window.
   */
  constexpr explicit mmio_space_backend(volatile void *base, std::size_t size = 0) noexcept
      : base_(static_cast<volatile std::byte *>(base)), size_(size) {}

  /** @brief Whether this backend was bound to a non-null window. */
  [[nodiscard]] constexpr bool is_bound() const noexcept { return base_ != nullptr; }

  /** @brief The window size passed at construction (`0` means unbounded/unchecked). */
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }

private:
  template <typename T> [[nodiscard]] reloco::result<volatile T *> at(std::uint64_t addr) const noexcept {
    if (!base_)
      return reloco::unexpected(reloco::error::unsupported_operation);
    if (size_ != 0 && (addr > size_ || size_ - addr < sizeof(T)))
      return reloco::unexpected(reloco::error::out_of_range);
    // The window check above guarantees addr + sizeof(T) <= size_.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    return reinterpret_cast<volatile T *>(base_ + addr);
    RELOCO_END_UNSAFE_BUFFER_USAGE
  }

  volatile std::byte *base_ = nullptr;
  std::size_t size_ = 0;

  friend struct io_space_traits<mmio_space_backend>;
};

template <> struct io_space_traits<mmio_space_backend> {
  [[nodiscard]] static reloco::result<std::uint8_t> read8(mmio_space_backend &b, std::uint64_t addr) noexcept {
    auto p = b.at<std::uint8_t>(addr);
    if (!p)
      return reloco::unexpected(p.error());
    return **p;
  }

  [[nodiscard]] static reloco::result<std::uint16_t> read16(mmio_space_backend &b, std::uint64_t addr) noexcept {
    auto p = b.at<std::uint16_t>(addr);
    if (!p)
      return reloco::unexpected(p.error());
    return **p;
  }

  [[nodiscard]] static reloco::result<std::uint32_t> read32(mmio_space_backend &b, std::uint64_t addr) noexcept {
    auto p = b.at<std::uint32_t>(addr);
    if (!p)
      return reloco::unexpected(p.error());
    return **p;
  }

  [[nodiscard]] static reloco::result<std::uint64_t> read64(mmio_space_backend &b, std::uint64_t addr) noexcept {
    auto p = b.at<std::uint64_t>(addr);
    if (!p)
      return reloco::unexpected(p.error());
    return **p;
  }

  [[nodiscard]] static reloco::result<void> write8(mmio_space_backend &b, std::uint64_t addr,
                                                   std::uint8_t value) noexcept {
    auto p = b.at<std::uint8_t>(addr);
    if (!p)
      return reloco::unexpected(p.error());
    **p = value;
    return {};
  }

  [[nodiscard]] static reloco::result<void> write16(mmio_space_backend &b, std::uint64_t addr,
                                                    std::uint16_t value) noexcept {
    auto p = b.at<std::uint16_t>(addr);
    if (!p)
      return reloco::unexpected(p.error());
    **p = value;
    return {};
  }

  [[nodiscard]] static reloco::result<void> write32(mmio_space_backend &b, std::uint64_t addr,
                                                    std::uint32_t value) noexcept {
    auto p = b.at<std::uint32_t>(addr);
    if (!p)
      return reloco::unexpected(p.error());
    **p = value;
    return {};
  }

  [[nodiscard]] static reloco::result<void> write64(mmio_space_backend &b, std::uint64_t addr,
                                                    std::uint64_t value) noexcept {
    auto p = b.at<std::uint64_t>(addr);
    if (!p)
      return reloco::unexpected(p.error());
    **p = value;
    return {};
  }

  // No read_rep*/write_rep*: a volatile MMIO window has no hardware
  // "rep ins/outs"-style fast path; io_space_ref's generic single-access
  // loop (built on the 8 mandatory functions above) already does the
  // right thing -- the same fixed address, `count` times.
  //
  // This matters beyond mere correctness-on-paper: real registers at a
  // fixed MMIO address are very often read- and/or write-*sensitive*
  // (a FIFO that pops its next byte on every read, a status register
  // that clears-on-read, a strobe/doorbell register where every write
  // -- not just the last one -- triggers a side effect). The generic
  // loop issues one genuine `read8`/`write8` call per element, each of
  // which performs its own fresh `volatile` dereference through `at<T>`
  // above; `volatile` is precisely what stops the compiler from
  // eliding, reordering, or coalescing those `count` accesses into
  // fewer (or differently-ordered) ones. See
  // `tests/test_mmio_space.cpp` for tests exercising exactly this: a
  // read-sensitive register observing successive live values across
  // repeated reads, and a write-sensitive one observing every write in
  // the order issued.
};

} // namespace structo
