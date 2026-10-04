// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file cpu_mask.hpp
 * @brief `structo::arch::cpu_mask<Tag, MaxCpus>`: a fixed-capacity,
 * compile-time-sized bitmask over logical CPU (or vCPU) indices, with
 * Rust `bitflags`-flavored set-algebra semantics and a lock-free,
 * compiler-intrinsic-backed subset of `std::atomic<T>`'s per-bit API.
 *
 * Kernels and hypervisors constantly need "which of these N things does
 * this apply to" bitmasks -- which cores to IPI during a TLB shootdown
 * (see `asid_allocator.hpp`'s SMP flush guide), which cores a task's
 * affinity allows it to run on, which vCPUs of a VM need a virtual
 * interrupt delivered, which cores are currently online. `cpu_mask<Tag,
 * MaxCpus>` is the allocation-free, fixed-size bitmask for exactly that:
 * `MaxCpus` is a compile-time template parameter (matching `hw_id_lut`'s
 * `MaxCpus` and `asid_allocator`'s `MaxActive`), so the backing storage
 * is a small, fixed-size array of `std::uint64_t` words embedded
 * directly in the object -- no allocator, no `reloco::vector`, safe to
 * use in interrupt/trap context or before an allocator even exists.
 *
 * ## Why tagged
 *
 * A physical-core bitmask and a virtual-CPU bitmask are both "a bitmask
 * of CPU-shaped things", but a vCPU index from one VM's `vcpu_mask` must
 * never be fed into a physical-core API expecting a `physical_cpu_mask`
 * (or vice versa) -- the two index spaces mean completely different
 * things even though they're both just small integers. `cpu_mask<Tag,
 * MaxCpus>` is parameterized on a phantom `Tag` for exactly the reason
 * `tagged_asid<Tag>` is (see `asid_allocator.hpp`): `physical_cpu_tag`
 * and `vcpu_tag` are bundled, and a `cpu_mask<physical_cpu_tag, 64>` and
 * a `cpu_mask<vcpu_tag, 64>` are simply unrelated types with no implicit
 * conversion between them, at zero runtime cost. Like `tagged_asid`,
 * there is deliberately no cross-tag cast -- a physical core index and a
 * vCPU index are never legitimately interchangeable.
 *
 * ## Rust `bitflags`-flavored set algebra
 *
 * A CPU mask is structurally a *set* of flags, which is exactly the
 * domain Rust's `bitflags` crate targets -- unlike `target_ptr`'s
 * arithmetic domain (where Rust's `checked_add`/`wrapping_add`/
 * `saturating_add` family was the natural fit), the natural fit here is
 * `bitflags`' set-algebra vocabulary: `contains()`, `intersects()`,
 * `union_with()`/`operator|`, `intersection()`/`operator&`,
 * `difference()`/`operator-`, `symmetric_difference()`/`operator^`, and
 * `complement()`/`operator~`, all UB-free (operating only on the
 * `MaxCpus` bits that are actually in range; unused trailing bits in the
 * last word are permanently masked to zero so `complement()`/`filled()`
 * can never spuriously produce an out-of-range bit). Per-CPU access
 * (`set`/`clear`/`test`/`toggle`) instead follows `structo`'s usual
 * checked/`try_`/`unsafe_` tri-tier convention, since those operate on a
 * runtime index that can be out of range.
 *
 * @code
 * using core_mask = structo::arch::cpu_mask<structo::arch::physical_cpu_tag, 128>;
 *
 * core_mask online; // starts empty
 * online.set(0);
 * online.set(1);
 * online.set(2);
 *
 * core_mask siblings = core_mask::single(1) | core_mask::single(2);
 * if (online.contains(siblings)) {
 *   // every core in `siblings` is also online
 * }
 *
 * for (std::size_t cpu : online) {
 *   arch_send_ipi(cpu); // iterates 0, 1, 2
 * }
 *
 * core_mask offline = online.complement(); // every core NOT in `online`
 * @endcode
 *
 * ## Atomic per-CPU operations (lock-free, GCC/Clang only)
 *
 * A single, shared `cpu_mask` instance (e.g. a kernel-wide "online CPUs"
 * mask each core sets its own bit in during bring-up) needs concurrent,
 * lock-free updates from multiple cores -- exactly the subset of
 * `std::atomic<T>`'s API (and Linux's atomic bitops: `set_bit`/
 * `clear_bit`/`test_and_set_bit`/...) that `atomic_test`/`atomic_set`/
 * `atomic_clear`/`atomic_toggle`/`atomic_test_and_set`/
 * `atomic_test_and_clear`/`atomic_test_and_toggle` provide, each a single
 * atomic load or read-modify-write of the one word containing the
 * requested bit, implemented directly with GCC/Clang's `__atomic_*`
 * builtins (this library targets only those two compilers, matching
 * `sync/early_rendezvous_barrier.h`'s own convention -- no `std::atomic<T>`
 * storage is used, so the non-atomic API above stays trivially copyable
 * and `constexpr`-usable). This gives per-bit atomicity, not whole-mask
 * atomicity: a mask spanning more than one word (`MaxCpus > 64`) cannot be
 * observed or updated as a single indivisible unit, the same inherent
 * limitation real kernels' `cpumask_t` has. Mixing an `atomic_*` call
 * with a plain, non-atomic accessor on the same instance from different
 * threads without external synchronization is a data race like any
 * other.
 *
 * @code
 * using core_mask = structo::arch::cpu_mask<structo::arch::physical_cpu_tag, 128>;
 *
 * core_mask online_cpus; // shared, e.g. a global/static instance
 *
 * // Called independently by each core during its own bring-up path --
 * // safe without a lock, even for cores whose bits land in the same word.
 * void mark_this_cpu_online(std::size_t cpu_id) {
 *   online_cpus.atomic_set(cpu_id, std::memory_order_release);
 * }
 *
 * bool is_cpu_online(std::size_t cpu_id) {
 *   return online_cpus.atomic_test(cpu_id, std::memory_order_acquire);
 * }
 *
 * // Lock-free "claim a free vCPU slot" bitmap allocator, racing against
 * // other cores calling the same function concurrently.
 * structo::result<std::size_t> allocate_vcpu_slot(core_mask &free_slots) {
 *   auto slot = free_slots.atomic_find_and_set();
 *   if (!slot.has_value()) {
 *     return structo::unexpected(structo::error::resource_exhausted);
 *   }
 *   return slot.value();
 * }
 * @endcode
 *
 * Bit-scanning operations (`count()`, `lowest_set[_from]()`,
 * `highest_set()`, and their atomic counterparts) are implemented with
 * GCC/Clang's `__builtin_popcountll`/`__builtin_ctzll`/`__builtin_clzll`
 * intrinsics rather than hand-rolled bit-by-bit loops -- these lower
 * directly to a single hardware instruction (e.g. `popcnt`/`bsf`/`bsr` or
 * `tzcnt`/`lzcnt` on x86, `rbit`+`clz` on AArch64) on every target this
 * library supports.
 *
 * @note Refactor candidate: `bitmap_utils.hpp`/`bitmap_ops.hpp` (added
 * later) now provide the same tri-tier/atomic bit API generically over
 * `unsigned long` words. `cpu_mask` predates them and still hand-rolls
 * its own `std::uint64_t`-word bit-twiddling so it can stay fully
 * `constexpr`-usable (`bitmap_ops` is not); unifying the two would mean
 * either making `bitmap_utils` constexpr-friendly or accepting the loss
 * of `cpu_mask`'s compile-time usability. Left as-is for now -- a future
 * pass could have `cpu_mask<Tag, MaxCpus>` derive from
 * `bitmap_ops<cpu_mask<Tag, MaxCpus>>` (on top of a `fixed_bitmap<MaxCpus>`-
 * shaped backing store) once that tradeoff is revisited.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <reloco/detail/assert.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>

#if !defined(__GNUC__) && !defined(__clang__)
#error "cpu_mask's atomic_* operations require GCC or Clang (__atomic_* builtins)"
#endif

namespace structo::arch {

using namespace reloco;

/** @brief Phantom `cpu_mask` tag for physical/logical host CPU indices. */
struct physical_cpu_tag {};

/** @brief Phantom `cpu_mask` tag for a VM's virtual CPU (vCPU) indices. */
struct vcpu_tag {};

/**
 * @brief Fixed-capacity bitmask over `[0, MaxCpus)` logical CPU/vCPU indices.
 * @tparam Tag Phantom tag distinguishing e.g. physical-core masks from
 * vCPU masks at the type level; see "Why tagged" above. Use
 * `physical_cpu_tag`/`vcpu_tag`, or define your own for further
 * subdivisions (e.g. a secure-world-only core mask).
 * @tparam MaxCpus Number of CPU/vCPU indices this mask can represent,
 * `[0, MaxCpus)`. A compile-time constant -- the backing storage is a
 * fixed-size array of `std::uint64_t` words, no allocation.
 */
template <typename Tag, std::size_t MaxCpus> class cpu_mask {
  static_assert(MaxCpus >= 1, "cpu_mask requires at least one CPU");

  static constexpr std::size_t bits_per_word = sizeof(std::uint64_t) * 8;

public:
  using tag_type = Tag;

  /** @brief Number of CPU/vCPU indices this mask type can represent. */
  static inline constexpr std::size_t max_cpus = MaxCpus;

  /** @brief Number of `std::uint64_t` words backing this mask. */
  static inline constexpr std::size_t word_count = (MaxCpus + bits_per_word - 1) / bits_per_word;

  /** @brief Constructs an empty mask (no CPUs set). */
  constexpr cpu_mask() noexcept = default;

  // ---------------------------------------------------------------------------
  // Factories
  // ---------------------------------------------------------------------------

  /** @brief An empty mask (no CPUs set); equivalent to the default constructor. */
  [[nodiscard]] static constexpr cpu_mask empty() noexcept { return cpu_mask{}; }

  /** @brief A full mask with every CPU in `[0, MaxCpus)` set. */
  [[nodiscard]] static constexpr cpu_mask filled() noexcept {
    cpu_mask m;
    for (std::size_t i = 0; i < word_count; ++i) {
      m.words_[i] = ~std::uint64_t{0};
    }
    m.mask_tail_padding();
    return m;
  }

  /**
   * @brief A mask with only `cpu` set.
   *
   * Traps (`RELOCO_ASSERT`) if `cpu >= MaxCpus`; use `try_single()` to
   * handle an out-of-range index as data instead.
   */
  [[nodiscard]] RELOCO_CONSTEXPR20 static cpu_mask single(std::size_t cpu) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: single() cpu out of range");
    cpu_mask m;
    m.unsafe_set(cpu);
    return m;
  }

  /** @brief Fallible variant of `single()`. */
  [[nodiscard]] static constexpr result<cpu_mask> try_single(std::size_t cpu) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    cpu_mask m;
    m.unsafe_set(cpu);
    return m;
  }

  // ---------------------------------------------------------------------------
  // Per-CPU Tri-Tier Accessors
  // ---------------------------------------------------------------------------

  /** @brief Sets `cpu`. Traps (`RELOCO_ASSERT`) if `cpu >= MaxCpus`. */
  RELOCO_CONSTEXPR20 void set(std::size_t cpu) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: set() cpu out of range");
    unsafe_set(cpu);
  }

  /** @brief Fallible variant of `set()`. */
  constexpr result<void> try_set(std::size_t cpu) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    unsafe_set(cpu);
    return {};
  }

  /** @brief Sets `cpu` without range-checking; UB if `cpu >= MaxCpus`. */
  RELOCO_UNSAFE_BUFFER_USAGE constexpr void unsafe_set(std::size_t cpu) noexcept {
    words_[cpu / bits_per_word] |= (std::uint64_t{1} << (cpu % bits_per_word));
  }

  /** @brief Clears `cpu`. Traps (`RELOCO_ASSERT`) if `cpu >= MaxCpus`. */
  RELOCO_CONSTEXPR20 void clear(std::size_t cpu) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: clear() cpu out of range");
    unsafe_clear(cpu);
  }

  /** @brief Fallible variant of `clear()`. */
  constexpr result<void> try_clear(std::size_t cpu) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    unsafe_clear(cpu);
    return {};
  }

  /** @brief Clears `cpu` without range-checking; UB if `cpu >= MaxCpus`. */
  RELOCO_UNSAFE_BUFFER_USAGE constexpr void unsafe_clear(std::size_t cpu) noexcept {
    words_[cpu / bits_per_word] &= ~(std::uint64_t{1} << (cpu % bits_per_word));
  }

  /** @brief Returns whether `cpu` is set. Traps (`RELOCO_ASSERT`) if `cpu >= MaxCpus`. */
  [[nodiscard]] RELOCO_CONSTEXPR20 bool test(std::size_t cpu) const noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: test() cpu out of range");
    return unsafe_test(cpu);
  }

  /** @brief Fallible variant of `test()`. */
  [[nodiscard]] constexpr result<bool> try_test(std::size_t cpu) const noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    return unsafe_test(cpu);
  }

  /** @brief Returns whether `cpu` is set, without range-checking; UB if `cpu >= MaxCpus`. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE constexpr bool unsafe_test(std::size_t cpu) const noexcept {
    return (words_[cpu / bits_per_word] & (std::uint64_t{1} << (cpu % bits_per_word))) != 0;
  }

  /** @brief Flips `cpu`. Traps (`RELOCO_ASSERT`) if `cpu >= MaxCpus`. */
  RELOCO_CONSTEXPR20 void toggle(std::size_t cpu) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: toggle() cpu out of range");
    unsafe_toggle(cpu);
  }

  /** @brief Fallible variant of `toggle()`. */
  constexpr result<void> try_toggle(std::size_t cpu) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    unsafe_toggle(cpu);
    return {};
  }

  /** @brief Flips `cpu` without range-checking; UB if `cpu >= MaxCpus`. */
  RELOCO_UNSAFE_BUFFER_USAGE constexpr void unsafe_toggle(std::size_t cpu) noexcept {
    words_[cpu / bits_per_word] ^= (std::uint64_t{1} << (cpu % bits_per_word));
  }

  /** @brief Returns the raw backing word at `index`. Traps if `index >= word_count`. */
  [[nodiscard]] RELOCO_CONSTEXPR20 std::uint64_t word(std::size_t index) const noexcept {
    RELOCO_ASSERT(index < word_count, "cpu_mask: word() index out of range");
    return words_[index];
  }

  /** @brief Fallible variant of `word()`. */
  [[nodiscard]] constexpr result<std::uint64_t> try_word(std::size_t index) const noexcept {
    if (index >= word_count) {
      return unexpected(error::out_of_range);
    }
    return words_[index];
  }

  /**
   * @brief A non-owning, `word_count`-long view over this mask's raw
   * backing words (word `i` holds CPUs `[i * 64, i * 64 + 64)`),
   * letting any `MaxCpus`-templated `cpu_mask` "decay" to a
   * type-erased word sequence for an API (e.g. `irqc_ref::send_ipi`)
   * that does not want to be templated on `Tag`/`MaxCpus` itself.
   * `&`-qualified (and `RELOCO_LIFETIMEBOUND`) since the returned span
   * borrows this mask's own storage: it must not outlive it, and may
   * not be taken from a temporary.
   */
  [[nodiscard]] constexpr span<const std::uint64_t> words() const & noexcept RELOCO_LIFETIMEBOUND {
    return span<const std::uint64_t>(words_, word_count);
  }

  // ---------------------------------------------------------------------------
  // Atomic Per-CPU Operations (lock-free, GCC/Clang `__atomic_*` builtins)
  // ---------------------------------------------------------------------------
  //
  // Each `atomic_*` method below is a single, genuinely atomic read-modify-
  // write (or load) of the ONE word containing `cpu`'s bit, implemented
  // directly with GCC/Clang's `__atomic_*` builtins (this library targets
  // only those two compilers -- see `sync/early_rendezvous_barrier.h` for
  // the same convention). This is enough to let multiple cores concurrently
  // set/clear/test/toggle bits of a single, shared `cpu_mask` instance --
  // even bits that happen to land in the same word -- with no external
  // lock, exactly like Linux's `set_bit`/`clear_bit`/`test_and_set_bit`
  // atomic bitops or Rust's `AtomicUsize::fetch_or`/`fetch_and`/
  // `compare_exchange`.
  //
  // What this does NOT provide is whole-mask atomicity: a mask wider than
  // one word (`MaxCpus > 64`) cannot be read or updated as a single
  // indivisible unit -- a concurrent reader can observe one word already
  // updated and another not yet, the same inherent limitation `cpumask_t`
  // has on real kernels. And mixing an `atomic_*` call with a plain
  // (non-atomic) accessor (`set`/`clear`/`operator|=`/...) on the same
  // instance from different threads without external synchronization is
  // a data race like any other, exactly as it would be mixing
  // `std::atomic<T>` operations with a plain, unsynchronized read/write of
  // the same memory.
  //
  // Also included here: `atomic_lowest_set[_from]()` (atomic find-first-set,
  // mirroring Linux's `find_next_bit()`) and `atomic_find_and_set[_from]()`
  // (atomic find-first-CLEAR-and-set, a lock-free "claim a free slot"
  // bitmap allocator primitive built on `__atomic_compare_exchange_n()`
  // retry loops). Both scan word-by-word using `__builtin_ctzll()` rather
  // than a bit-by-bit loop.

  /** @brief Atomically returns whether `cpu` is set. Traps if `cpu >= MaxCpus`. */
  [[nodiscard]] bool atomic_test(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: atomic_test() cpu out of range");
    return unsafe_atomic_test(cpu, order);
  }

  /** @brief Fallible variant of `atomic_test()`. */
  [[nodiscard]] result<bool> atomic_try_test(std::size_t cpu,
                                             std::memory_order order = std::memory_order_seq_cst) const noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    return unsafe_atomic_test(cpu, order);
  }

  /** @brief `atomic_test()` without range-checking; UB if `cpu >= MaxCpus`. */
  [[nodiscard]] RELOCO_UNSAFE_BUFFER_USAGE bool
  unsafe_atomic_test(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    std::uint64_t w = __atomic_load_n(&words_[cpu / bits_per_word], to_atomic_order(order));
    return (w & (std::uint64_t{1} << (cpu % bits_per_word))) != 0;
  }

  /** @brief Atomically sets `cpu`. Traps if `cpu >= MaxCpus`. */
  void atomic_set(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: atomic_set() cpu out of range");
    unsafe_atomic_set(cpu, order);
  }

  /** @brief Fallible variant of `atomic_set()`. */
  result<void> atomic_try_set(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    unsafe_atomic_set(cpu, order);
    return {};
  }

  /** @brief `atomic_set()` without range-checking; UB if `cpu >= MaxCpus`. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_atomic_set(std::size_t cpu,
                                                    std::memory_order order = std::memory_order_seq_cst) noexcept {
    __atomic_fetch_or(&words_[cpu / bits_per_word], std::uint64_t{1} << (cpu % bits_per_word), to_atomic_order(order));
  }

  /** @brief Atomically clears `cpu`. Traps if `cpu >= MaxCpus`. */
  void atomic_clear(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: atomic_clear() cpu out of range");
    unsafe_atomic_clear(cpu, order);
  }

  /** @brief Fallible variant of `atomic_clear()`. */
  result<void> atomic_try_clear(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    unsafe_atomic_clear(cpu, order);
    return {};
  }

  /** @brief `atomic_clear()` without range-checking; UB if `cpu >= MaxCpus`. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_atomic_clear(std::size_t cpu,
                                                      std::memory_order order = std::memory_order_seq_cst) noexcept {
    __atomic_fetch_and(&words_[cpu / bits_per_word], ~(std::uint64_t{1} << (cpu % bits_per_word)),
                       to_atomic_order(order));
  }

  /** @brief Atomically flips `cpu`. Traps if `cpu >= MaxCpus`. */
  void atomic_toggle(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: atomic_toggle() cpu out of range");
    unsafe_atomic_toggle(cpu, order);
  }

  /** @brief Fallible variant of `atomic_toggle()`. */
  result<void> atomic_try_toggle(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    unsafe_atomic_toggle(cpu, order);
    return {};
  }

  /** @brief `atomic_toggle()` without range-checking; UB if `cpu >= MaxCpus`. */
  RELOCO_UNSAFE_BUFFER_USAGE void unsafe_atomic_toggle(std::size_t cpu,
                                                       std::memory_order order = std::memory_order_seq_cst) noexcept {
    __atomic_fetch_xor(&words_[cpu / bits_per_word], std::uint64_t{1} << (cpu % bits_per_word), to_atomic_order(order));
  }

  /**
   * @brief Atomically sets `cpu`, returning its previous state.
   *
   * Mirrors Linux's `test_and_set_bit()` / Rust's `AtomicBool`-style
   * fetch-and-modify. Traps if `cpu >= MaxCpus`.
   */
  bool atomic_test_and_set(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: atomic_test_and_set() cpu out of range");
    return unsafe_atomic_test_and_set(cpu, order);
  }

  /** @brief Fallible variant of `atomic_test_and_set()`. */
  result<bool> atomic_try_test_and_set(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    return unsafe_atomic_test_and_set(cpu, order);
  }

  /** @brief `atomic_test_and_set()` without range-checking; UB if `cpu >= MaxCpus`. */
  RELOCO_UNSAFE_BUFFER_USAGE bool
  unsafe_atomic_test_and_set(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    std::uint64_t bit = std::uint64_t{1} << (cpu % bits_per_word);
    std::uint64_t prev = __atomic_fetch_or(&words_[cpu / bits_per_word], bit, to_atomic_order(order));
    return (prev & bit) != 0;
  }

  /**
   * @brief Atomically clears `cpu`, returning its previous state.
   *
   * Mirrors Linux's `test_and_clear_bit()`. Traps if `cpu >= MaxCpus`.
   */
  bool atomic_test_and_clear(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: atomic_test_and_clear() cpu out of range");
    return unsafe_atomic_test_and_clear(cpu, order);
  }

  /** @brief Fallible variant of `atomic_test_and_clear()`. */
  result<bool> atomic_try_test_and_clear(std::size_t cpu,
                                         std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    return unsafe_atomic_test_and_clear(cpu, order);
  }

  /** @brief `atomic_test_and_clear()` without range-checking; UB if `cpu >= MaxCpus`. */
  RELOCO_UNSAFE_BUFFER_USAGE bool
  unsafe_atomic_test_and_clear(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    std::uint64_t bit = std::uint64_t{1} << (cpu % bits_per_word);
    std::uint64_t prev = __atomic_fetch_and(&words_[cpu / bits_per_word], ~bit, to_atomic_order(order));
    return (prev & bit) != 0;
  }

  /**
   * @brief Atomically flips `cpu`, returning its previous state.
   *
   * Mirrors Linux's `test_and_change_bit()`. Traps if `cpu >= MaxCpus`.
   */
  bool atomic_test_and_toggle(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    RELOCO_ASSERT(cpu < MaxCpus, "cpu_mask: atomic_test_and_toggle() cpu out of range");
    return unsafe_atomic_test_and_toggle(cpu, order);
  }

  /** @brief Fallible variant of `atomic_test_and_toggle()`. */
  result<bool> atomic_try_test_and_toggle(std::size_t cpu,
                                          std::memory_order order = std::memory_order_seq_cst) noexcept {
    if (cpu >= MaxCpus) {
      return unexpected(error::out_of_range);
    }
    return unsafe_atomic_test_and_toggle(cpu, order);
  }

  /** @brief `atomic_test_and_toggle()` without range-checking; UB if `cpu >= MaxCpus`. */
  RELOCO_UNSAFE_BUFFER_USAGE bool
  unsafe_atomic_test_and_toggle(std::size_t cpu, std::memory_order order = std::memory_order_seq_cst) noexcept {
    std::uint64_t bit = std::uint64_t{1} << (cpu % bits_per_word);
    std::uint64_t prev = __atomic_fetch_xor(&words_[cpu / bits_per_word], bit, to_atomic_order(order));
    return (prev & bit) != 0;
  }

  /** @brief Atomically loads the raw backing word at `index`. Traps if `index >= word_count`. */
  [[nodiscard]] std::uint64_t atomic_word(std::size_t index,
                                          std::memory_order order = std::memory_order_seq_cst) const noexcept {
    RELOCO_ASSERT(index < word_count, "cpu_mask: atomic_word() index out of range");
    return __atomic_load_n(&words_[index], to_atomic_order(order));
  }

  /** @brief Fallible variant of `atomic_word()`. */
  [[nodiscard]] result<std::uint64_t>
  atomic_try_word(std::size_t index, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    if (index >= word_count) {
      return unexpected(error::out_of_range);
    }
    return __atomic_load_n(&words_[index], to_atomic_order(order));
  }

  /**
   * @brief Atomically finds the lowest set CPU index at or after `start`.
   *
   * Mirrors Linux's `find_next_bit()`. Each backing word is loaded with
   * a single atomic `__atomic_load_n()`, and `__builtin_ctzll()` locates
   * the lowest set bit within it -- no hand-rolled bit-by-bit scan. As
   * with the rest of the atomic API, this is only atomic per-word: a
   * concurrent writer can set a bit in an already-scanned word after this
   * call observed it as zero, so the result is a snapshot, not a
   * linearization point across the whole mask.
   */
  [[nodiscard]] reloco::optional<std::size_t>
  atomic_lowest_set_from(std::size_t start, std::memory_order order = std::memory_order_seq_cst) const noexcept {
    if (start >= MaxCpus) {
      return reloco::nullopt;
    }
    std::size_t word_idx = start / bits_per_word;
    std::uint64_t remaining = atomic_word(word_idx, order) >> (start % bits_per_word);
    if (remaining != 0) {
      std::size_t bit = start + static_cast<std::size_t>(__builtin_ctzll(remaining));
      return bit < MaxCpus ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
    }
    for (std::size_t i = word_idx + 1; i < word_count; ++i) {
      std::uint64_t w = atomic_word(i, order);
      if (w != 0) {
        std::size_t bit = i * bits_per_word + static_cast<std::size_t>(__builtin_ctzll(w));
        return bit < MaxCpus ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
      }
    }
    return reloco::nullopt;
  }

  /** @brief Atomically finds the lowest set CPU index, if any. */
  [[nodiscard]] reloco::optional<std::size_t>
  atomic_lowest_set(std::memory_order order = std::memory_order_seq_cst) const noexcept {
    return atomic_lowest_set_from(0, order);
  }

  /**
   * @brief Atomically finds the lowest clear CPU index at or after `start`
   * and sets it, returning that index -- a lock-free "allocate one CPU
   * slot" primitive (mirrors Rust's `AtomicUsize`-based bitset allocators
   * and `asid_allocator.hpp`'s bitmap scan-and-claim pattern).
   *
   * Retries internally via compare-and-swap on the owning word if another
   * core concurrently claims/frees bits in the same word; never blocks or
   * traps. Returns `nullopt` if every CPU at or after `start` is already
   * set.
   */
  [[nodiscard]] reloco::optional<std::size_t>
  atomic_find_and_set_from(std::size_t start, std::memory_order order = std::memory_order_seq_cst) noexcept {
    std::size_t start_word = start / bits_per_word;
    for (std::size_t word_idx = start_word; word_idx < word_count; ++word_idx) {
      std::size_t word_base = word_idx * bits_per_word;
      std::size_t low_bound = word_idx == start_word ? start % bits_per_word : 0;
      std::uint64_t expected = __atomic_load_n(&words_[word_idx], to_atomic_order(order));
      for (;;) {
        std::uint64_t available = ~expected >> low_bound;
        if (available == 0) {
          break; // this word is exhausted (from low_bound onward); move to the next word
        }
        std::size_t bit_in_word = low_bound + static_cast<std::size_t>(__builtin_ctzll(available));
        std::size_t cpu = word_base + bit_in_word;
        if (cpu >= MaxCpus) {
          break;
        }
        std::uint64_t desired = expected | (std::uint64_t{1} << bit_in_word);
        if (__atomic_compare_exchange_n(&words_[word_idx], &expected, desired, /*weak=*/true, to_atomic_order(order),
                                        to_atomic_order(order))) {
          return cpu;
        }
        // `expected` was refreshed with the current value by the failed CAS; retry.
      }
    }
    return reloco::nullopt;
  }

  /** @brief Atomically finds and sets the lowest clear CPU index, if any. */
  [[nodiscard]] reloco::optional<std::size_t>
  atomic_find_and_set(std::memory_order order = std::memory_order_seq_cst) noexcept {
    return atomic_find_and_set_from(0, order);
  }

  /**
   * @brief Takes a plain, non-atomic snapshot of the whole mask, loading
   * each backing word with a single atomic `__atomic_load_n()`.
   *
   * Like every other `atomic_*` operation here, this is only atomic
   * per-word, not whole-mask-atomic: for `MaxCpus > 64`, a concurrent
   * writer can update one word in between this call reading two
   * different words, so the result can be a mix of before- and
   * after-update state across words. Useful for computing a TLB-shootdown
   * target set (see `asid_allocator.hpp`'s SMP flush guide and
   * `mm_asid_context.hpp`) from a concurrently-updated "which cores is
   * this address space resident on" mask, where a slightly-stale
   * (superset) snapshot is always safe -- flushing one CPU too many never
   * causes incorrect behavior, only a wasted invalidation.
   */
  [[nodiscard]] cpu_mask atomic_snapshot(std::memory_order order = std::memory_order_seq_cst) const noexcept {
    cpu_mask snapshot;
    for (std::size_t i = 0; i < word_count; ++i) {
      snapshot.words_[i] = atomic_word(i, order);
    }
    return snapshot;
  }

  // ---------------------------------------------------------------------------
  // Whole-Mask Queries
  // ---------------------------------------------------------------------------

  /** @brief Clears every bit. */
  constexpr void clear_all() noexcept {
    for (std::size_t i = 0; i < word_count; ++i) {
      words_[i] = 0;
    }
  }

  /** @brief Number of set bits. */
  [[nodiscard]] constexpr std::size_t count() const noexcept {
    std::size_t total = 0;
    for (std::size_t i = 0; i < word_count; ++i) {
      total += static_cast<std::size_t>(__builtin_popcountll(words_[i]));
    }
    return total;
  }

  /** @brief `true` if at least one CPU is set. */
  [[nodiscard]] constexpr bool any() const noexcept {
    for (std::size_t i = 0; i < word_count; ++i) {
      if (words_[i] != 0) {
        return true;
      }
    }
    return false;
  }

  /** @brief `true` if no CPU is set. */
  [[nodiscard]] constexpr bool none() const noexcept { return !any(); }

  /** @brief `true` if every CPU in `[0, MaxCpus)` is set. */
  [[nodiscard]] constexpr bool all() const noexcept { return *this == filled(); }

  /** @brief Lowest set CPU index at or after `start`, if any. */
  [[nodiscard]] constexpr reloco::optional<std::size_t> lowest_set_from(std::size_t start) const noexcept {
    if (start >= MaxCpus) {
      return reloco::nullopt;
    }
    std::size_t word_idx = start / bits_per_word;
    std::uint64_t remaining = words_[word_idx] >> (start % bits_per_word);
    if (remaining != 0) {
      std::size_t bit = start + static_cast<std::size_t>(__builtin_ctzll(remaining));
      return bit < MaxCpus ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
    }
    for (std::size_t i = word_idx + 1; i < word_count; ++i) {
      if (words_[i] != 0) {
        std::size_t bit = i * bits_per_word + static_cast<std::size_t>(__builtin_ctzll(words_[i]));
        return bit < MaxCpus ? reloco::optional<std::size_t>(bit) : reloco::nullopt;
      }
    }
    return reloco::nullopt;
  }

  /** @brief Lowest set CPU index, if any. */
  [[nodiscard]] constexpr reloco::optional<std::size_t> lowest_set() const noexcept { return lowest_set_from(0); }

  /** @brief Highest set CPU index, if any. */
  [[nodiscard]] constexpr reloco::optional<std::size_t> highest_set() const noexcept {
    for (std::size_t word_idx = word_count; word_idx-- > 0;) {
      std::uint64_t w = words_[word_idx];
      if (w == 0) {
        continue;
      }
      std::size_t bit = (bits_per_word - 1) - static_cast<std::size_t>(__builtin_clzll(w));
      return word_idx * bits_per_word + bit;
    }
    return reloco::nullopt;
  }

  // ---------------------------------------------------------------------------
  // Rust `bitflags`-Flavored Set Algebra
  // ---------------------------------------------------------------------------

  /** @brief `true` if every CPU set in `other` is also set in `*this` (superset test). */
  [[nodiscard]] constexpr bool contains(const cpu_mask &other) const noexcept { return (*this & other) == other; }

  /** @brief `true` if `*this` and `other` have at least one CPU in common. */
  [[nodiscard]] constexpr bool intersects(const cpu_mask &other) const noexcept { return (*this & other).any(); }

  /** @brief CPUs set in `*this` or `other` (or both). Matches `operator|`. */
  [[nodiscard]] constexpr cpu_mask union_with(const cpu_mask &other) const noexcept { return *this | other; }

  /** @brief CPUs set in both `*this` and `other`. Matches `operator&`. */
  [[nodiscard]] constexpr cpu_mask intersection(const cpu_mask &other) const noexcept { return *this & other; }

  /** @brief CPUs set in `*this` but not in `other`. Matches `operator-`. */
  [[nodiscard]] constexpr cpu_mask difference(const cpu_mask &other) const noexcept { return *this & ~other; }

  /** @brief CPUs set in exactly one of `*this`/`other`. Matches `operator^`. */
  [[nodiscard]] constexpr cpu_mask symmetric_difference(const cpu_mask &other) const noexcept { return *this ^ other; }

  /** @brief Every CPU in `[0, MaxCpus)` NOT set in `*this`. Matches `operator~`. */
  [[nodiscard]] constexpr cpu_mask complement() const noexcept { return ~*this; }

  [[nodiscard]] friend constexpr cpu_mask operator|(cpu_mask a, const cpu_mask &b) noexcept {
    a |= b;
    return a;
  }

  [[nodiscard]] friend constexpr cpu_mask operator&(cpu_mask a, const cpu_mask &b) noexcept {
    a &= b;
    return a;
  }

  [[nodiscard]] friend constexpr cpu_mask operator^(cpu_mask a, const cpu_mask &b) noexcept {
    a ^= b;
    return a;
  }

  [[nodiscard]] friend constexpr cpu_mask operator-(cpu_mask a, const cpu_mask &b) noexcept {
    a -= b;
    return a;
  }

  [[nodiscard]] constexpr cpu_mask operator~() const noexcept {
    cpu_mask r;
    for (std::size_t i = 0; i < word_count; ++i) {
      r.words_[i] = ~words_[i];
    }
    r.mask_tail_padding();
    return r;
  }

  constexpr cpu_mask &operator|=(const cpu_mask &other) noexcept {
    for (std::size_t i = 0; i < word_count; ++i) {
      words_[i] |= other.words_[i];
    }
    return *this;
  }

  constexpr cpu_mask &operator&=(const cpu_mask &other) noexcept {
    for (std::size_t i = 0; i < word_count; ++i) {
      words_[i] &= other.words_[i];
    }
    return *this;
  }

  constexpr cpu_mask &operator^=(const cpu_mask &other) noexcept {
    for (std::size_t i = 0; i < word_count; ++i) {
      words_[i] ^= other.words_[i];
    }
    return *this;
  }

  /** @brief Clears every bit in `other` from `*this` (set difference, in place). */
  constexpr cpu_mask &operator-=(const cpu_mask &other) noexcept {
    for (std::size_t i = 0; i < word_count; ++i) {
      words_[i] &= ~other.words_[i];
    }
    return *this;
  }

  [[nodiscard]] friend constexpr bool operator==(const cpu_mask &a, const cpu_mask &b) noexcept {
    for (std::size_t i = 0; i < word_count; ++i) {
      if (a.words_[i] != b.words_[i]) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] friend constexpr bool operator!=(const cpu_mask &a, const cpu_mask &b) noexcept { return !(a == b); }

  // ---------------------------------------------------------------------------
  // Iteration (yields set CPU indices, ascending)
  // ---------------------------------------------------------------------------

  /** @brief Forward iterator yielding each set CPU index, ascending. */
  class const_iterator {
  public:
    using value_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    constexpr const_iterator() noexcept = default;

    [[nodiscard]] constexpr std::size_t operator*() const noexcept { return pos_; }

    constexpr const_iterator &operator++() noexcept {
      auto next = mask_->lowest_set_from(pos_ + 1);
      pos_ = next.has_value() ? next.value() : MaxCpus;
      return *this;
    }

    constexpr const_iterator operator++(int) noexcept {
      const_iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    [[nodiscard]] friend constexpr bool operator==(const const_iterator &a, const const_iterator &b) noexcept {
      return a.pos_ == b.pos_;
    }

    [[nodiscard]] friend constexpr bool operator!=(const const_iterator &a, const const_iterator &b) noexcept {
      return !(a == b);
    }

  private:
    friend class cpu_mask;
    constexpr const_iterator(const cpu_mask *mask, std::size_t pos) noexcept : mask_(mask), pos_(pos) {}

    const cpu_mask *mask_{nullptr};
    std::size_t pos_{MaxCpus};
  };

  using iterator = const_iterator;

  [[nodiscard]] constexpr const_iterator begin() const noexcept {
    auto first = lowest_set_from(0);
    return const_iterator(this, first.has_value() ? first.value() : MaxCpus);
  }

  [[nodiscard]] constexpr const_iterator end() const noexcept { return const_iterator(this, MaxCpus); }

private:
  // Clears any unused high bits in the last word (when MaxCpus isn't a
  // multiple of bits_per_word) so filled()/complement()/all()/== never
  // observe spurious out-of-range bits.
  constexpr void mask_tail_padding() noexcept {
    constexpr std::size_t valid_bits_in_last_word = MaxCpus - (word_count - 1) * bits_per_word;
    if constexpr (valid_bits_in_last_word < bits_per_word) {
      constexpr std::uint64_t tail_mask = (std::uint64_t{1} << valid_bits_in_last_word) - 1;
      words_[word_count - 1] &= tail_mask;
    }
  }

  // GCC/Clang's `__atomic_*` builtins take a plain `int` memory-order
  // constant (`__ATOMIC_RELAXED`, ...); both compilers' `std::memory_order`
  // enumerators are defined with exactly these same underlying values (it
  // is how their own `<atomic>` implementation passes `std::memory_order`
  // through to these very builtins internally), so this cast is safe given
  // this library only targets GCC/Clang.
  [[nodiscard]] static constexpr int to_atomic_order(std::memory_order order) noexcept {
    return static_cast<int>(order);
  }

  std::uint64_t words_[word_count]{};
};

} // namespace structo::arch
