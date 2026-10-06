<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `structo::hw::vm_time_manager<Traits>`

`include/structo/hw/vm_time_manager.hpp`

A hypervisor-side helper that computes and applies the raw hardware
offset a guest vCPU's virtual free-running counter needs, relative to
the host's own counter (`time_source_ref`, see
[`time_manager.md`](time_manager.md)), so a vCPU's timer baseline can be
set at reset, preserved across a scheduling pause, or re-established
after live migration.

This is unrelated to (though it composes with) `structo::hw::time_manager`:
`time_manager` samples a free-running counter to maintain the *host's
own* monotonic/realtime "now". `vm_time_manager` instead programs the
architecture register that determines what a *guest* vCPU's own counter
reads -- it never reads or republishes a clock of its own, and it does
not run inside the guest.

## The core primitive: one offset, one convention, three architectures

Every architecture `structo` targets exposes a guest-visible virtual
counter whose value is some function of the host's physical counter and
one hypervisor-programmed per-vCPU offset register:

| Architecture | Guest-visible register | Offset register | Native relationship |
|---|---|---|---|
| ARMv8-A/ARMv9-A | `CNTVCT_EL0` | `CNTVOFF_EL2` | `guest = host - CNTVOFF_EL2` |
| x86-64 (Intel VMX) | `RDTSC`/`RDTSCP` | VMCS `TSC_OFFSET` | `guest = host + TSC_OFFSET` (ignoring the optional VMCS `TSC_MULTIPLIER` scaling field) |
| RISC-V (H-extension) | `time` CSR (VS-level) | `htimedelta`/`htimedeltah` | `guest = host + htimedelta` |

Two of the three add the offset; ARM subtracts it. `vm_time_manager`
fixes one canonical convention -- **`guest = host + offset`** -- for
every `cycles` value it exchanges with `vm_timer_traits`; a backend
whose native register uses the opposite convention (ARM) negates once,
at the point it touches the register, via `cycles::wrapping_sub`. All of
`vm_time_manager`'s own arithmetic is `cycles::wrapping_add`/
`wrapping_sub` -- the same modulo-2^64, never-fails arithmetic the
hardware register itself implements, with no intermediate
`reloco::duration` conversion and no possibility of a spurious
`error::integer_overflow`.

**Out of scope:** per-vCPU frequency *scaling* (VMX `TSC_MULTIPLIER`, or
an equivalent software-trapped scheme on ARM/RISC-V) -- `vm_timer_traits`
is offset-only. A hypervisor presenting a scaled guest frequency must
apply that scaling itself and keep `CNTFRQ_EL0`/`capabilities().clock_hz`/
equivalent consistent with whatever rate it actually presents.

## Customization point: `vm_timer_traits<Traits>`

`Traits` is supplied by the embedding hypervisor and must define:

```cpp
struct my_vm_timer_traits {
  // Opaque, cheap-to-copy handle identifying one vCPU's register context (a raw pointer, an index into a
  // table the hypervisor already owns, ...). Never dereferenced by vm_time_manager itself.
  using vcpu_handle = my_vcpu_handle;

  // Programs the offset register so the vCPU's virtual counter reads `host + offset` from this point on.
  // Writing an inactive vCPU's saved register context (rather than a live system register) is fine and
  // expected; vm_time_manager never assumes the vCPU is the one currently executing.
  static void apply_offset(vcpu_handle vcpu, structo::hw::cycles offset) noexcept;

  // Reads back whatever apply_offset most recently programmed, decoded into the same "guest = host +
  // offset" convention (an ARM backend must undo its own negation here).
  static reloco::result<structo::hw::cycles> try_read_offset(vcpu_handle vcpu) noexcept;
};
```

There is no undefined default -- unlike `time_source_traits`/
`hw_rng_traits`, `vm_time_manager<Traits>` simply will not compile
against an incomplete `Traits`, mirroring `sync::irq_guard<Traits>`/
`arch::lazy_context<Traits>`'s direct-duck-typing convention for a
single-architecture-at-a-time policy parameter: a given hypervisor build
targets exactly one architecture's register layout, so there is no
type-erased `vm_timer_ref`.

### Worked examples (ARM, x86 VMX, RISC-V H-extension)

See `vm_time_manager.hpp`'s own `@file`-level docs for complete,
compilable `Traits` implementations for all three architectures,
including the ARM sign-flip at the `apply_offset`/`try_read_offset`
boundary.

## The API: `rebind`/`reset`/`try_guest_now`

```cpp
template <typename Traits> class vm_time_manager {
public:
  using vcpu_handle = typename Traits::vcpu_handle;

  constexpr explicit vm_time_manager(time_source_ref host_counter) noexcept;

  result<void> rebind(vcpu_handle vcpu, cycles guest_target) const noexcept;
  result<void> reset(vcpu_handle vcpu) const noexcept; // rebind(vcpu, cycles{0})
  result<cycles> try_guest_now(vcpu_handle vcpu) const noexcept;
  constexpr time_source_ref host_counter() const noexcept;

  template <typename VCPUs>
  result<void> pause_all(VCPUs &&vcpus, reloco::span<cycles> out_snapshots) const noexcept;
  template <typename VCPUs>
  result<void> resume_all(VCPUs &&vcpus, reloco::span<const cycles> snapshots) const noexcept;
};
```

Every scenario reduces to the one `rebind` primitive -- program `vcpu`'s
offset register so its virtual counter reads `guest_target` *now* (at
whatever host instant the call samples):

- **vCPU reset**: `guest_target = cycles{0}` (or use `reset()`).
- **Pause/resume**: capture `try_guest_now(vcpu)` immediately before
  descheduling the vCPU, then `rebind(vcpu, <that captured value>)` on
  resume -- the host counter may have advanced arbitrarily far during
  the pause (a long scheduler quantum, a suspend/resume cycle), but the
  guest never observes that gap.
- **Live migration**: identical to pause/resume, except the `rebind`
  call happens through a *different* `vm_time_manager` instance (bound
  to the new host's counter) than the `try_guest_now` snapshot was taken
  through (bound to the old host's counter).

Both `rebind`/`try_guest_now` only fail if sampling the host counter (or,
for `try_guest_now`, reading back the offset register) itself fails;
`vm_timer_traits::apply_offset` is unconditional (`void`, not
`result<void>`), matching the "a register write is a programming
precondition, not a runtime condition" convention
`sync::irq_guard<Traits>::hw_restore_irqs` already uses.

## Whole-VM pause/resume: `pause_all`/`resume_all`

A single vCPU's ordinary scheduling gap -- the host descheduling one
vCPU's thread while the rest of the VM (and the host itself) keeps
running -- must **never** be hidden from the guest: its virtual counter
has to keep tracking real wall-clock time even while that vCPU isn't
scheduled, exactly like real hardware's architectural counters never
stop just because a core's thread lost the CPU. (Accounting for CPU
time a vCPU *wanted* but didn't get is a separate "steal time"
paravirtualization feature, out of scope here, for the same reason VMX
`TSC_MULTIPLIER` frequency scaling is.)

A **true whole-VM pause** -- a checkpoint, a suspend-to-host, or the
stop-the-world gap during live migration, where literally zero vCPUs of
the VM are executing anywhere -- is the opposite case: that gap *should*
be invisible to the guest, via the existing `try_guest_now`-before-pause
+ `rebind`-after-resume primitive applied to every vCPU at once.
`pause_all`/`resume_all` are exactly that loop:

```cpp
template <typename Traits> class vm_time_manager {
  // ...
  template <typename VCPUs>
  result<void> pause_all(VCPUs &&vcpus, reloco::span<cycles> out_snapshots) const noexcept;
  template <typename VCPUs>
  result<void> resume_all(VCPUs &&vcpus, reloco::span<const cycles> snapshots) const noexcept;
};
```

- `pause_all(vcpus, out_snapshots)` calls `try_guest_now` on every entry
  of `vcpus` (in order) and stores each result into the matching slot of
  `out_snapshots` -- call this immediately before the whole-VM pause
  takes effect.
- `resume_all(vcpus, snapshots)` calls `rebind` on every entry of
  `vcpus` with its matching `snapshots` value -- call this immediately
  after the VM resumes, so collectively no vCPU observes any elapsed
  time. For live migration, call it through a *different*
  `vm_time_manager` (bound to the destination host's counter) than the
  one `pause_all` was called through.
- `VCPUs` is any range usable in a range-`for` that also supports
  `std::size` (e.g. `reloco::span<const vcpu_handle>`, a plain C array,
  or a hypervisor-owned intrusive/linked vCPU list that tracks its own
  count) -- iteration itself never needs random access, only
  `begin()`/`end()`/`size()`, so the embedding hypervisor's own vCPU
  bookkeeping doesn't have to be array-based.
- `out_snapshots`/`snapshots` are caller-owned (`reloco::span`, never
  allocated internally), one `cycles` slot per entry of `vcpus`, in the
  same order. A length mismatch returns `error::invalid_argument`
  without touching any vCPU; otherwise, the first failing
  `try_guest_now`/`rebind` call stops the loop and its error is returned
  (any vCPUs already processed before the failure keep their new state).

## Multi-vCPU VMs: one `vm_time_manager`, one host time source, many `vcpu_handle`s

A `vm_time_manager<Traits>` instance is bound to exactly one host counter
(its constructor argument), but every method takes a `vcpu_handle`
identifying *which* vCPU's offset register to touch -- so the right
granularity is **one `vm_time_manager` per VM** (constructed once, e.g.
alongside the VM itself), reused across calls for every vCPU that VM
owns, **never** one instance per vCPU.

This mirrors `time_manager`'s own `is_per_cpu` restriction (see
[`time_manager.md`](time_manager.md)'s "Handling an imperfect hardware
counter" section) for exactly the same reason: a guest SMP kernel
assumes its vCPUs' virtual counters are mutually consistent -- a read on
one vCPU and a read on another, taken "at the same time", must agree
(modulo normal cross-core skew), exactly like native SMP hardware's
`CNTVCT_EL0`/invariant TSC/`mtime` already guarantee across physical
cores. That guarantee only holds if every vCPU's offset was computed
from the *same* globally-consistent host counter
(`time_source_capabilities::is_per_cpu == false`) -- binding two
different `vm_time_manager` instances to two different,
not-mutually-synchronized counters (e.g. two per-core, non-invariant
TSCs) for two vCPUs of the *same* VM would silently reintroduce the
exact cross-core skew problem a single, shared host counter exists to
avoid.

This does **not** mean every vCPU must share one offset register, or
even be bound to the same baseline at the same wall-clock moment -- the
`vcpu_handle` parameter is per-vCPU precisely so each vCPU's own register
can be programmed independently (they are, after all, physically
separate `CNTVOFF_EL2`/VMCS/`htimedelta` instances, one per vCPU). What
must be shared is only the *host counter being read*, so that "vCPU A's
offset, computed from host reading X" and "vCPU B's offset, computed
from host reading Y" both describe offsets from the same underlying
timeline. In practice this also makes "align every vCPU to the same
guest-visible baseline at VM boot" straightforward -- call `reset` (or
`rebind`) once per vCPU, in a tight loop, all through the one shared
`vm_time_manager`:

```cpp
structo::hw::vm_time_manager<my_vm_timer_traits> vm_clock(host_counter); // once per VM
for (my_vcpu_handle vcpu : vm.vcpus())
  (void)vm_clock.reset(vcpu); // every vCPU reads ~cycles{0} at (approximately) the same host instant
```

(The loop body's own execution time means later vCPUs in the loop are
bound a handful of host cycles later than earlier ones -- exactly the
same bounded skew native multi-core boot/reset sequencing already has to
tolerate, not a gap this header introduces.)

## Testing

`tests/test_vm_time_manager.cpp` exercises: `rebind`/`reset` programming
the expected offset (including the wraparound case where `guest_target`
is numerically below the current host reading); `try_guest_now` tracking
the host counter advancing after a `rebind`; both host-counter-read and
offset-read-back failures propagating; the pause/resume round trip
preserving apparent guest time across an arbitrarily large host jump;
two vCPUs sharing one `vm_time_manager` staying mutually consistent;
`pause_all`/`resume_all` round-tripping across multiple vCPUs supplied
through a minimal intrusive linked list (proving no random access is
required), plus their length-mismatch and mid-loop failure-propagation
cases; and a second, ARM-shaped fake backend confirming the
subtract-convention sign flip round-trips correctly through the common
`guest = host + offset` convention.

## See also

- [`time_manager.md`](time_manager.md) -- the host-side tickless
  timekeeping facility `vm_time_manager`'s bound `time_source_ref`
  typically also backs.
- [`reference.md`](reference.md) -- the full per-header API map.
