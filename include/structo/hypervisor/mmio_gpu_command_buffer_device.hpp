// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file mmio_gpu_command_buffer_device.hpp
 * @brief `structo::hypervisor::mmio_gpu_command_buffer_device`: a
 * pseudo hardware-accelerated 2D drawing device -- the guest fills a
 * directly-mapped command buffer with a batch of drawing primitives,
 * then triggers their synchronous execution with one trapped MMIO
 * write, exactly like a real GPU's command-buffer/ring submission
 * model, just executed inline rather than asynchronously by actual
 * silicon.
 *
 * ## Two address ranges, a third time
 *
 * - @ref command_buffer() returns a directly-mapped, allocator-owned,
 *   page-aligned block of fixed-size command slots (see @ref
 *   command_slot_size/@ref gpu_command): the guest driver writes
 *   `count()` commands into it with ordinary stores, no VM exit per
 *   command -- exactly the same "plain-memory data plane" every other
 *   header in this series (`mmio_text_console`,
 *   `mmio_framebuffer_device`) already establishes, just holding
 *   drawing commands instead of cells or pixels this time.
 * - The `mmio_device_traits<mmio_gpu_command_buffer_device>`
 *   specialization models a tiny trapped "virtual GPU control" register
 *   window: a read-only `capacity` (how many command slots the buffer
 *   holds), a read/write `count` (how many of those slots the guest has
 *   actually filled in since the last execute), and a doorbell
 *   `execute` register. Writing *any* value to `execute` synchronously
 *   decodes and runs `count()` commands from the buffer against a bound
 *   `hw::gpu_accel_ref` target -- the trapped write does not return to
 *   the guest (or, in this emulation, to the VM-exit handler's caller)
 *   until every command has been executed; there is no asynchronous
 *   completion/interrupt to model, matching how this emulation always
 *   executes rather than queues work for a separate pass. `count()` is
 *   reset to `0` immediately afterwards, ready for the next batch.
 *
 * ## Commands
 *
 * Each @ref command_slot_size-byte slot decodes (via @ref
 * decode_gpu_command) to a @ref gpu_command: a @ref gpu_command_opcode
 * plus four signed 32-bit operands (`x0`/`y0`/`x1`/`y1`, reused as
 * `x`/`y`/`w`/`h` for the rectangle opcodes) and one packed 0xRRGGBBAA
 * color. See @ref execute_gpu_command for exactly how each opcode's
 * operands map onto a `gpu_accel_ref` call. A negative operand where a
 * `std::size_t` coordinate/extent is required (e.g. a negative `x0` for
 * `fill_rect`) makes that single command a silent no-op (still counted
 * as executed) rather than wrapping around to a huge unsigned value --
 * a buggy or malicious guest driver cannot use this to draw outside the
 * bound target's own clipped bounds.
 *
 * ## Backend: `hw::gpu_accel_ref`
 *
 * This header only *decodes and dispatches* commands; the actual
 * drawing happens wherever the bound `hw::gpu_accel_ref` (see
 * `gpu_accel_ref.hpp`) forwards to -- a plain software
 * `hw::framebuffer<PixelFormat>` by default, or a real GPU-backed
 * renderer (e.g. an SDL `SDL_Renderer` wrapper specializing
 * `hw::gpu_accel_traits`) an embedding plugs in instead, with this
 * device needing no changes either way.
 *
 * @code
 * structo::hw::framebuffer<structo::hw::xrgb8888> screen = ...;
 * structo::hw::gpu_accel_ref target(screen); // or an SDL_Renderer-backed type instead
 *
 * auto maker = structo::hypervisor::mmio_gpu_command_buffer_device::try_create(target, 64);
 * auto accel = std::move(maker.value());
 * structo::hypervisor::mmio_device_ref control(accel); // the small trapped register window
 * // map accel.command_buffer() directly into guest physical memory for the command data itself.
 *
 * // Guest driver writes two commands (clear, then fill_rect) into slots 0 and 1 of command_buffer(),
 * // sets count = 2, then writes anything to the execute register -- synchronously, right here:
 * (void)control.try_write(mmio_gpu_command_buffer_device::control_off_count, ...); // count = 2
 * (void)control.try_write(mmio_gpu_command_buffer_device::control_off_execute, ...); // runs both commands now
 * @endcode
 */

#include "mmio_device_ref.hpp"

#include <structo/hw/gpu_accel_ref.hpp>

#include <cstddef>
#include <cstdint>
#include <reloco/allocator.hpp>
#include <reloco/default_allocator.hpp>
#include <reloco/error.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/expected.hpp>
#include <reloco/span.hpp>
#include <utility>

namespace structo::hypervisor {

/** @brief The small set of drawing primitives a @ref gpu_command may request -- one-to-one with
 * `hw::gpu_accel_ref`'s own operation set (minus `clear`/`width`/`height`, which take no rectangle/line
 * operands, and are modeled by @ref gpu_command_opcode::clear alone). */
enum class gpu_command_opcode : std::uint32_t {
  /** @brief Ignored: all operands and the color are unused. The buffer's zeroed padding past `count()`
   * naturally decodes to this, so trailing garbage/unfilled slots are always safe to "execute". */
  nop = 0,
  /** @brief `target.clear(color)`. All four operands ignored. */
  clear = 1,
  /** @brief `target.fill_rect(x0, y0, x1, y1, color)` (`x1`/`y1` reused as `w`/`h`). */
  fill_rect = 2,
  /** @brief `target.draw_rect(x0, y0, x1, y1, color)` (`x1`/`y1` reused as `w`/`h`). */
  draw_rect = 3,
  /** @brief `target.draw_line(x0, y0, x1, y1, color)`. The only opcode whose operands may legitimately be
   * negative/out-of-bounds in either direction -- a line may start/end offscreen. */
  draw_line = 4,
  /** @brief `target.put_pixel(x0, y0, color)`. `x1`/`y1` ignored. */
  put_pixel = 5,
};

/** @brief One decoded drawing command -- see the @file-level docs' "Commands" for the full operand-reuse
 * rules per @ref gpu_command_opcode. */
struct gpu_command {
  gpu_command_opcode opcode = gpu_command_opcode::nop;
  std::int32_t x0 = 0;
  std::int32_t y0 = 0;
  std::int32_t x1 = 0;
  std::int32_t y1 = 0;
  /** @brief Packed `0xRRGGBBAA` (red in the most significant byte); see @ref unpack_gpu_color. */
  std::uint32_t color = 0;
};

/** @brief Unpacks a @ref gpu_command::color value into an `hw::rgb_color`. */
[[nodiscard]] constexpr hw::rgb_color unpack_gpu_color(std::uint32_t packed) noexcept {
  return {static_cast<std::uint8_t>((packed >> 24) & 0xffu), static_cast<std::uint8_t>((packed >> 16) & 0xffu),
          static_cast<std::uint8_t>((packed >> 8) & 0xffu), static_cast<std::uint8_t>(packed & 0xffu)};
}

/** @brief Packs an `hw::rgb_color` into a @ref gpu_command::color value (the inverse of @ref
 * unpack_gpu_color), mainly useful for tests/a hypervisor-side helper constructing commands directly
 * rather than decoding them from guest-written bytes. */
[[nodiscard]] constexpr std::uint32_t pack_gpu_color(hw::rgb_color c) noexcept {
  return (static_cast<std::uint32_t>(c.r) << 24) | (static_cast<std::uint32_t>(c.g) << 16) |
         (static_cast<std::uint32_t>(c.b) << 8) | static_cast<std::uint32_t>(c.a);
}

/**
 * @brief Decodes one @ref gpu_command from a @ref mmio_gpu_command_buffer_device::command_slot_size-byte
 * slot, field-by-field (no reliance on `gpu_command`'s in-memory layout matching the wire format, matching
 * every other trapped register in this codebase -- see `mmio_device_ref.hpp`'s own docs on endianness: none
 * is performed).
 * @param slot Exactly @ref mmio_gpu_command_buffer_device::command_slot_size bytes.
 */
[[nodiscard]] inline gpu_command decode_gpu_command(span<const std::byte> slot) noexcept {
  gpu_command cmd;
  std::uint32_t opcode_raw = 0;
  // Every offset+4 is within the documented command_slot_size (24) bytes the caller must pass.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  __builtin_memcpy(&opcode_raw, slot.data() + 0, 4);
  cmd.opcode = static_cast<gpu_command_opcode>(opcode_raw);
  __builtin_memcpy(&cmd.x0, slot.data() + 4, 4);
  __builtin_memcpy(&cmd.y0, slot.data() + 8, 4);
  __builtin_memcpy(&cmd.x1, slot.data() + 12, 4);
  __builtin_memcpy(&cmd.y1, slot.data() + 16, 4);
  __builtin_memcpy(&cmd.color, slot.data() + 20, 4);
  RELOCO_END_UNSAFE_BUFFER_USAGE
  return cmd;
}

/** @brief Encodes @p cmd into a @ref mmio_gpu_command_buffer_device::command_slot_size-byte slot -- the
 * inverse of @ref decode_gpu_command, used by tests and by any hypervisor-side helper that wants to stage
 * commands into @ref mmio_gpu_command_buffer_device::command_buffer() itself rather than relying on the
 * guest driver to. */
inline void encode_gpu_command(const gpu_command &cmd, span<std::byte> slot) noexcept {
  auto opcode_raw = static_cast<std::uint32_t>(cmd.opcode);
  // Every offset+4 is within the documented command_slot_size (24) bytes the caller must pass.
  RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
  __builtin_memcpy(slot.data() + 0, &opcode_raw, 4);
  __builtin_memcpy(slot.data() + 4, &cmd.x0, 4);
  __builtin_memcpy(slot.data() + 8, &cmd.y0, 4);
  __builtin_memcpy(slot.data() + 12, &cmd.x1, 4);
  __builtin_memcpy(slot.data() + 16, &cmd.y1, 4);
  __builtin_memcpy(slot.data() + 20, &cmd.color, 4);
  RELOCO_END_UNSAFE_BUFFER_USAGE
}

/** @brief Dispatches one decoded @ref gpu_command to @p target, per the per-opcode operand-reuse rules
 * documented on @ref gpu_command_opcode. A negative operand where @p target needs an unsigned
 * coordinate/extent makes the whole command a no-op instead of wrapping around to a huge unsigned value. */
inline void execute_gpu_command(hw::gpu_accel_ref &target, const gpu_command &cmd) noexcept {
  hw::rgb_color color = unpack_gpu_color(cmd.color);
  switch (cmd.opcode) {
  case gpu_command_opcode::clear:
    target.clear(color);
    break;
  case gpu_command_opcode::fill_rect:
    if (cmd.x0 >= 0 && cmd.y0 >= 0 && cmd.x1 >= 0 && cmd.y1 >= 0) {
      target.fill_rect(static_cast<std::size_t>(cmd.x0), static_cast<std::size_t>(cmd.y0),
                       static_cast<std::size_t>(cmd.x1), static_cast<std::size_t>(cmd.y1), color);
    }
    break;
  case gpu_command_opcode::draw_rect:
    if (cmd.x0 >= 0 && cmd.y0 >= 0 && cmd.x1 >= 0 && cmd.y1 >= 0) {
      target.draw_rect(static_cast<std::size_t>(cmd.x0), static_cast<std::size_t>(cmd.y0),
                       static_cast<std::size_t>(cmd.x1), static_cast<std::size_t>(cmd.y1), color);
    }
    break;
  case gpu_command_opcode::draw_line:
    target.draw_line(cmd.x0, cmd.y0, cmd.x1, cmd.y1, color); // signed: offscreen endpoints are fine
    break;
  case gpu_command_opcode::put_pixel:
    if (cmd.x0 >= 0 && cmd.y0 >= 0) {
      (void)target.put_pixel(static_cast<std::size_t>(cmd.x0), static_cast<std::size_t>(cmd.y0), color);
    }
    break;
  case gpu_command_opcode::nop:
  default:
    break;
  }
}

/**
 * @brief A pseudo hardware-accelerated 2D drawing device: an allocator-owned, page-aligned, directly
 * guest-mappable command buffer plus a small trapped "virtual GPU control" register window that
 * synchronously executes a batch of queued commands against a bound `hw::gpu_accel_ref` target on a single
 * MMIO write. See the @file-level docs above. Move-only.
 */
class mmio_gpu_command_buffer_device {
public:
  /** @brief Bytes per command slot: six packed 4-byte fields (opcode, x0, y0, x1, y1, color); see @ref
   * gpu_command/@ref decode_gpu_command. */
  static constexpr std::size_t command_slot_size = 24;

  /** @brief Default VM page size used by @ref try_allocate/@ref try_create; see `mmio_text_console`'s docs
   * for why the owned allocation path rounds up to this. */
  static constexpr std::size_t default_page_size = 4096;

  /** @brief Virtual GPU control-register offsets, each a 4-byte register. */
  static constexpr std::uint64_t control_off_capacity = 0;
  static constexpr std::uint64_t control_off_count = 4;
  static constexpr std::uint64_t control_off_execute = 8;
  /** @brief Size in bytes of the "virtual GPU control" register window -- what `mmio_device_ref` should be
   * sized to, entirely separate from `command_buffer()`'s own (much larger) size. */
  static constexpr std::size_t control_window_size = 12;

  /** @brief Constructs an empty (`capacity() == 0`), storage-less, unbound device for a future @ref
   * try_allocate-style use. Never fails. */
  constexpr explicit mmio_gpu_command_buffer_device(allocator_ref alloc = default_allocator()) noexcept
      : alloc_(alloc) {}

  mmio_gpu_command_buffer_device(mmio_gpu_command_buffer_device &&other) noexcept
      : alloc_(other.alloc_), owned_base_(other.owned_base_), owned_size_(other.owned_size_),
        capacity_(other.capacity_), count_(other.count_), last_executed_(other.last_executed_),
        target_(other.target_) {
    other.owned_base_ = nullptr;
    other.owned_size_ = 0;
    other.capacity_ = 0;
    other.count_ = 0;
    other.last_executed_ = 0;
    other.target_ = hw::gpu_accel_ref();
  }

  mmio_gpu_command_buffer_device &operator=(mmio_gpu_command_buffer_device &&other) noexcept {
    if (this != &other) {
      release();
      alloc_ = other.alloc_;
      owned_base_ = other.owned_base_;
      owned_size_ = other.owned_size_;
      capacity_ = other.capacity_;
      count_ = other.count_;
      last_executed_ = other.last_executed_;
      target_ = other.target_;
      other.owned_base_ = nullptr;
      other.owned_size_ = 0;
      other.capacity_ = 0;
      other.count_ = 0;
      other.last_executed_ = 0;
      other.target_ = hw::gpu_accel_ref();
    }
    return *this;
  }

  // Owns at most one unique allocation; not copyable, same as mmio_text_console/mmio_framebuffer_device.
  mmio_gpu_command_buffer_device(const mmio_gpu_command_buffer_device &) = delete;
  mmio_gpu_command_buffer_device &operator=(const mmio_gpu_command_buffer_device &) = delete;

  ~mmio_gpu_command_buffer_device() noexcept { release(); }

  /**
   * @brief Fallible allocation factory: allocates and owns a page-aligned command buffer.
   * @param alloc Allocator backing the command buffer's storage.
   * @param target Drawing backend every executed command is forwarded to. May itself be unbound; see
   * @ref hw::gpu_accel_ref's own null-safety convention (every call silently no-ops).
   * @param capacity Number of @ref command_slot_size-byte command slots the buffer holds; the backing
   * block needs `capacity * command_slot_size` bytes, rounded up to a whole number of @p page_size bytes.
   * @param page_size VM page size the allocation's base address and byte size are aligned/rounded to; must
   * be a power of two. Defaults to @ref default_page_size.
   * @return The device (all slots zeroed, i.e. all decode as @ref gpu_command_opcode::nop), or
   * `error::invalid_argument` if `capacity`/`page_size` are zero or `page_size` is not a power of two, or
   * `error::allocation_failed` if @p alloc could not provide the backing storage.
   */
  [[nodiscard]] static result<mmio_gpu_command_buffer_device>
  try_allocate(allocator_ref alloc, hw::gpu_accel_ref target, std::size_t capacity,
              std::size_t page_size = default_page_size) noexcept {
    if (capacity == 0 || page_size == 0 || (page_size & (page_size - 1)) != 0) {
      return unexpected(error::invalid_argument);
    }

    mmio_gpu_command_buffer_device dev(alloc);
    const std::size_t bytes = capacity * command_slot_size;
    const std::size_t aligned_bytes = (bytes + page_size - 1) & ~(page_size - 1);

    // allocate() returns a block of at least aligned_bytes, which is only checked for failure here.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    auto res = alloc.allocate(aligned_bytes, page_size);
    RELOCO_END_UNSAFE_BUFFER_USAGE
    if (!res) {
      return unexpected(res.error());
    }

    // Sole owner of res->ptr/res->size, recorded directly here. See mmio_text_console.hpp's docs for why
    // ownership is never recovered indirectly through some other composed view later on.
    dev.owned_base_ = static_cast<std::byte *>(res->ptr);
    dev.owned_size_ = res->size; // the allocator's absorbed size -- what deallocate() must be called with
    for (std::byte &b : span<std::byte>(dev.owned_base_, dev.owned_size_)) {
      b = std::byte{0}; // every slot decodes as `nop` until the guest fills it in
    }
    dev.capacity_ = capacity;
    dev.target_ = target;
    return dev;
  }

  /** @brief `try_allocate()` using `reloco::default_allocator()`. */
  [[nodiscard]] static result<mmio_gpu_command_buffer_device>
  try_create(hw::gpu_accel_ref target, std::size_t capacity, std::size_t page_size = default_page_size) noexcept {
    return try_allocate(default_allocator(), target, capacity, page_size);
  }

  /** @brief Number of command slots @ref command_buffer() holds. */
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  /** @brief Number of command slots the guest has staged (and not yet executed) since the last @ref
   * execute_pending(). Always `<= capacity()`. */
  [[nodiscard]] std::size_t count() const noexcept { return count_; }

  /** @brief Number of commands actually executed by the most recent @ref execute_pending() call (`0`
   * before the first one). */
  [[nodiscard]] std::size_t last_executed() const noexcept { return last_executed_; }

  /** @brief Sets @ref count() -- how many of @ref command_buffer()'s leading slots the next @ref
   * execute_pending() should run.
   * @return `error::out_of_range` if @p n exceeds @ref capacity(). */
  result<void> set_count(std::size_t n) noexcept {
    if (n > capacity_) {
      return unexpected(error::out_of_range);
    }
    count_ = n;
    return {};
  }

  /** @brief The backend every executed command is forwarded to. */
  [[nodiscard]] hw::gpu_accel_ref target() const noexcept { return target_; }

  /** @brief The directly-mapped command buffer: the embedding hypervisor maps this span into guest
   * physical memory so the guest driver can write @ref gpu_command slots into it with ordinary stores, no
   * VM exit per command. Spans the full page-aligned owned block (which may be somewhat larger than
   * `capacity() * command_slot_size` after page rounding), mirroring `mmio_framebuffer_device::pixels()`'s
   * own "expose the whole owned block" convention. */
  [[nodiscard]] span<std::byte> command_buffer() noexcept { return span<std::byte>(owned_base_, owned_size_); }

  /**
   * @brief Synchronously decodes and executes @ref count() commands from the front of @ref
   * command_buffer() against @ref target(), then resets @ref count() to `0` (ready for the next batch).
   * Called automatically by a guest write to `control_off_execute`; also callable directly by the
   * hypervisor (e.g. to drain a batch the hypervisor itself staged, bypassing the MMIO window).
   * @return The number of commands executed (i.e. @ref count() as it was before this call).
   */
  std::size_t execute_pending() noexcept {
    std::size_t n = count_;
    span<std::byte> buf = command_buffer();
    for (std::size_t i = 0; i < n; ++i) {
      span<std::byte> slot = buf.subspan(i * command_slot_size, command_slot_size);
      gpu_command cmd = decode_gpu_command(slot);
      execute_gpu_command(target_, cmd);
    }
    last_executed_ = n;
    count_ = 0;
    return n;
  }

  /** @brief Resets @ref count() and @ref last_executed() to `0`, discarding any staged-but-not-yet-executed
   * commands without running them. Does not touch @ref command_buffer()'s own contents. */
  void reset_queue() noexcept {
    count_ = 0;
    last_executed_ = 0;
  }

private:
  void release() noexcept {
    if (owned_base_ != nullptr) {
      // owned_base_/owned_size_ are exactly the block returned by allocate().
      RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
      alloc_.deallocate(owned_base_, owned_size_);
      RELOCO_END_UNSAFE_BUFFER_USAGE
      owned_base_ = nullptr;
      owned_size_ = 0;
      capacity_ = 0;
      count_ = 0;
      last_executed_ = 0;
    }
  }

  allocator_ref alloc_;
  std::byte *owned_base_ = nullptr; // sole owner; see try_allocate's docs
  std::size_t owned_size_ = 0;      // the allocator's absorbed size -- what deallocate() must be called with
  std::size_t capacity_ = 0;
  std::size_t count_ = 0;
  std::size_t last_executed_ = 0;
  hw::gpu_accel_ref target_;
};

/** @brief The "virtual GPU control" register window: `capacity` read-only, `count` read/write, `execute` a
 * write-triggered doorbell (also readable, reporting @ref mmio_gpu_command_buffer_device::last_executed()).
 * See the @file-level docs' "Two address ranges, a third time". */
template <> struct mmio_device_traits<mmio_gpu_command_buffer_device> {
  using device = mmio_gpu_command_buffer_device;

  static std::size_t size(device &) noexcept { return device::control_window_size; }

  static result<void> try_read(device &d, std::uint64_t offset, span<std::byte> dst) noexcept {
    std::uint32_t value;
    switch (offset) {
    case device::control_off_capacity:
      value = static_cast<std::uint32_t>(d.capacity());
      break;
    case device::control_off_count:
      value = static_cast<std::uint32_t>(d.count());
      break;
    case device::control_off_execute:
      value = static_cast<std::uint32_t>(d.last_executed());
      break;
    default:
      return unexpected(error::invalid_argument);
    }
    if (dst.size() != sizeof(value)) {
      return unexpected(error::invalid_argument);
    }
    // dst.size() was checked equal to sizeof(value) above.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    __builtin_memcpy(dst.data(), &value, sizeof(value));
    RELOCO_END_UNSAFE_BUFFER_USAGE
    return {};
  }

  static result<void> try_write(device &d, std::uint64_t offset, span<const std::byte> src) noexcept {
    if (src.size() != sizeof(std::uint32_t)) {
      return unexpected(error::invalid_argument);
    }
    std::uint32_t value;
    // src.size() was checked equal to sizeof(value) above.
    RELOCO_BEGIN_UNSAFE_BUFFER_USAGE
    __builtin_memcpy(&value, src.data(), sizeof(value));
    RELOCO_END_UNSAFE_BUFFER_USAGE
    switch (offset) {
    case device::control_off_count:
      return d.set_count(value);
    case device::control_off_execute:
      (void)d.execute_pending(); // the written value itself is ignored -- any write is a doorbell ring
      return {};
    default:
      return unexpected(error::permission_denied); // capacity is read-only
    }
  }

  /** @brief Reflects whether the bound @ref hw::gpu_accel_ref target is itself actually bound. */
  static bool is_available(device &d) noexcept { return static_cast<bool>(d.target()); }

  /** @brief Discards any staged-but-not-yet-executed commands (via @ref
   * mmio_gpu_command_buffer_device::reset_queue()) without running them. */
  static result<void> try_reset(device &d) noexcept {
    d.reset_queue();
    return {};
  }
};

} // namespace structo::hypervisor
