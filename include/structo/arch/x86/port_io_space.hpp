// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file port_io_space.hpp
 * @brief `structo::arch::x86::port_io_backend`: a concrete
 * `structo::io_space_traits` backend issuing real `IN`/`OUT`
 * instructions over x86/x86-64's separate 16-bit port-I/O address
 * space, bindable through `structo::io_space_ref`.
 *
 * `io_address.hpp` is pure address tagging; `io_space_ref.hpp` is the
 * type-erased access layer and its `io_space_traits<Backend>`
 * customization point. This header supplies one concrete `Backend` for
 * that point: a zero-sized `port_io_backend` tag wired up to the actual
 * `inb`/`outb`/`inw`/`outw`/`inl`/`outl` instructions (plus their
 * `rep insb`/`outsb`-style string-I/O counterparts where one exists):
 *
 * @code
 * using lsr_addr = structo::io_address<std::uint8_t, structo::port_io_space>;
 *
 * structo::arch::x86::port_io_backend backend{};
 * structo::io_space_ref<structo::port_io_space> com1(backend);
 *
 * auto lsr = com1.read(lsr_addr{0x3FD});  // Line Status Register, COM1
 * if (lsr && (*lsr & 0x20))               // THR empty
 *   (void)com1.write(lsr_addr{0x3F8}, static_cast<std::uint8_t>('!'));
 * @endcode
 *
 * ## No 64-bit port I/O
 *
 * x86 has no `inq`/`outq` instruction and no 64-bit `rep ins`/`rep outs`
 * form. `read64`/`write64` unconditionally report
 * `reloco::error::unsupported_operation` (the documented contract for a
 * backend that genuinely cannot support a given width); `read_rep64`/
 * `write_rep64` are simply not provided at all, so `io_space_ref`
 * synthesizes them generically on top of that same always-failing
 * `read64`/`write64`.
 *
 * ## `rep ins`/`outs` fast paths
 *
 * `read_rep{8,16,32}`/`write_rep{8,16,32}` use the corresponding
 * `rep insb`/`insw`/`insl`/`outsb`/`outsw`/`outsl` instruction to service
 * an entire same-port burst (e.g. draining a FIFO) in one trampoline
 * call instead of `io_space_ref`'s generic one-access-at-a-time
 * fallback loop.
 *
 * ## Port range
 *
 * x86's port space is 16 bits wide (`[0, 0xFFFF]`). Every access
 * bounds-checks the incoming "wire" `std::uint64_t` address first and
 * reports `reloco::error::out_of_range` if it doesn't fit, before
 * narrowing it to the `std::uint16_t` the `IN`/`OUT` encodings need.
 *
 * ## Validation
 *
 * `IN`/`OUT` (and `rep ins`/`outs`) fault from unprivileged userspace
 * without an explicit `ioperm(2)`/`iopl(2)` grant, so this backend is
 * not runtime-exercised by this repository's own test suite -- only
 * header-compiled, under its `__i386__`/`__x86_64__` guard, exactly like
 * `arch::x86::irq_traits` (see `docs/irq_guard.md`). On every other host
 * this header is an intentional no-op so it stays header-check-clean
 * cross-compiled from any machine.
 */

#include "../../io_space_ref.hpp"

#include <reloco/error.hpp>

#include <cstddef>
#include <cstdint>

namespace structo::arch::x86 {

/**
 * @brief Zero-sized `io_space_traits` backend issuing real `IN`/`OUT`
 * instructions over x86/x86-64's 16-bit port-I/O address space.
 *
 * Not to be confused with `structo::port_io_space`, the address-*space
 * tag* `io_address<T, SpaceTag, IoInt>` is parameterized on; this is the
 * *backend* that actually performs accesses into such a space.
 */
struct port_io_backend {};

#if defined(__i386__) || defined(__x86_64__)

namespace detail {

/** @brief Bounds-checks @p addr against x86's 16-bit port space. */
[[nodiscard]] inline reloco::result<std::uint16_t> checked_port(std::uint64_t addr) noexcept {
  if (addr > 0xFFFFu)
    return reloco::unexpected(reloco::error::out_of_range);
  return static_cast<std::uint16_t>(addr);
}

[[nodiscard]] inline std::uint8_t hw_inb(std::uint16_t port) noexcept {
  std::uint8_t value;
  asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

inline void hw_outb(std::uint16_t port, std::uint8_t value) noexcept {
  asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

[[nodiscard]] inline std::uint16_t hw_inw(std::uint16_t port) noexcept {
  std::uint16_t value;
  asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

inline void hw_outw(std::uint16_t port, std::uint16_t value) noexcept {
  asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

[[nodiscard]] inline std::uint32_t hw_inl(std::uint16_t port) noexcept {
  std::uint32_t value;
  asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
  return value;
}

inline void hw_outl(std::uint16_t port, std::uint32_t value) noexcept {
  asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

inline void hw_insb(std::uint16_t port, std::uint8_t *dst, std::size_t count) noexcept {
  asm volatile("rep insb" : "+D"(dst), "+c"(count) : "d"(port) : "memory");
}

inline void hw_outsb(std::uint16_t port, const std::uint8_t *src, std::size_t count) noexcept {
  asm volatile("rep outsb" : "+S"(src), "+c"(count) : "d"(port) : "memory");
}

inline void hw_insw(std::uint16_t port, std::uint16_t *dst, std::size_t count) noexcept {
  asm volatile("rep insw" : "+D"(dst), "+c"(count) : "d"(port) : "memory");
}

inline void hw_outsw(std::uint16_t port, const std::uint16_t *src, std::size_t count) noexcept {
  asm volatile("rep outsw" : "+S"(src), "+c"(count) : "d"(port) : "memory");
}

inline void hw_insl(std::uint16_t port, std::uint32_t *dst, std::size_t count) noexcept {
  asm volatile("rep insl" : "+D"(dst), "+c"(count) : "d"(port) : "memory");
}

inline void hw_outsl(std::uint16_t port, const std::uint32_t *src, std::size_t count) noexcept {
  asm volatile("rep outsl" : "+S"(src), "+c"(count) : "d"(port) : "memory");
}

} // namespace detail

#endif // defined(__i386__) || defined(__x86_64__)

} // namespace structo::arch::x86

namespace structo {

template <> struct io_space_traits<structo::arch::x86::port_io_backend> {
#if defined(__i386__) || defined(__x86_64__)

  [[nodiscard]] static reloco::result<std::uint8_t> read8(structo::arch::x86::port_io_backend &,
                                                          std::uint64_t addr) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    return structo::arch::x86::detail::hw_inb(*port);
  }

  [[nodiscard]] static reloco::result<std::uint16_t> read16(structo::arch::x86::port_io_backend &,
                                                            std::uint64_t addr) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    return structo::arch::x86::detail::hw_inw(*port);
  }

  [[nodiscard]] static reloco::result<std::uint32_t> read32(structo::arch::x86::port_io_backend &,
                                                            std::uint64_t addr) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    return structo::arch::x86::detail::hw_inl(*port);
  }

  /** @brief x86 has no 64-bit port I/O; always unsupported. */
  [[nodiscard]] static reloco::result<std::uint64_t> read64(structo::arch::x86::port_io_backend &,
                                                            std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

  [[nodiscard]] static reloco::result<void> write8(structo::arch::x86::port_io_backend &, std::uint64_t addr,
                                                   std::uint8_t value) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    structo::arch::x86::detail::hw_outb(*port, value);
    return {};
  }

  [[nodiscard]] static reloco::result<void> write16(structo::arch::x86::port_io_backend &, std::uint64_t addr,
                                                    std::uint16_t value) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    structo::arch::x86::detail::hw_outw(*port, value);
    return {};
  }

  [[nodiscard]] static reloco::result<void> write32(structo::arch::x86::port_io_backend &, std::uint64_t addr,
                                                    std::uint32_t value) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    structo::arch::x86::detail::hw_outl(*port, value);
    return {};
  }

  /** @brief x86 has no 64-bit port I/O; always unsupported. */
  [[nodiscard]] static reloco::result<void> write64(structo::arch::x86::port_io_backend &, std::uint64_t,
                                                    std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

  [[nodiscard]] static reloco::result<void> read_rep8(structo::arch::x86::port_io_backend &, std::uint64_t addr,
                                                      std::uint8_t *dst, std::size_t count) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    structo::arch::x86::detail::hw_insb(*port, dst, count);
    return {};
  }

  [[nodiscard]] static reloco::result<void> write_rep8(structo::arch::x86::port_io_backend &, std::uint64_t addr,
                                                       const std::uint8_t *src, std::size_t count) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    structo::arch::x86::detail::hw_outsb(*port, src, count);
    return {};
  }

  [[nodiscard]] static reloco::result<void> read_rep16(structo::arch::x86::port_io_backend &, std::uint64_t addr,
                                                       std::uint16_t *dst, std::size_t count) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    structo::arch::x86::detail::hw_insw(*port, dst, count);
    return {};
  }

  [[nodiscard]] static reloco::result<void> write_rep16(structo::arch::x86::port_io_backend &, std::uint64_t addr,
                                                        const std::uint16_t *src, std::size_t count) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    structo::arch::x86::detail::hw_outsw(*port, src, count);
    return {};
  }

  [[nodiscard]] static reloco::result<void> read_rep32(structo::arch::x86::port_io_backend &, std::uint64_t addr,
                                                       std::uint32_t *dst, std::size_t count) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    structo::arch::x86::detail::hw_insl(*port, dst, count);
    return {};
  }

  [[nodiscard]] static reloco::result<void> write_rep32(structo::arch::x86::port_io_backend &, std::uint64_t addr,
                                                        const std::uint32_t *src, std::size_t count) noexcept {
    auto port = structo::arch::x86::detail::checked_port(addr);
    if (!port)
      return reloco::unexpected(port.error());
    structo::arch::x86::detail::hw_outsl(*port, src, count);
    return {};
  }

  // No read_rep64/write_rep64: no 64-bit rep ins/outs form exists on x86;
  // io_space_ref synthesizes them generically on top of the always-failing
  // read64/write64 above.

#else // !(defined(__i386__) || defined(__x86_64__))

  [[nodiscard]] static reloco::result<std::uint8_t> read8(structo::arch::x86::port_io_backend &,
                                                          std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

  [[nodiscard]] static reloco::result<std::uint16_t> read16(structo::arch::x86::port_io_backend &,
                                                            std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

  [[nodiscard]] static reloco::result<std::uint32_t> read32(structo::arch::x86::port_io_backend &,
                                                            std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

  [[nodiscard]] static reloco::result<std::uint64_t> read64(structo::arch::x86::port_io_backend &,
                                                            std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

  [[nodiscard]] static reloco::result<void> write8(structo::arch::x86::port_io_backend &, std::uint64_t,
                                                   std::uint8_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

  [[nodiscard]] static reloco::result<void> write16(structo::arch::x86::port_io_backend &, std::uint64_t,
                                                    std::uint16_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

  [[nodiscard]] static reloco::result<void> write32(structo::arch::x86::port_io_backend &, std::uint64_t,
                                                    std::uint32_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

  [[nodiscard]] static reloco::result<void> write64(structo::arch::x86::port_io_backend &, std::uint64_t,
                                                    std::uint64_t) noexcept {
    return reloco::unexpected(reloco::error::unsupported_operation);
  }

#endif // defined(__i386__) || defined(__x86_64__)
};

} // namespace structo
