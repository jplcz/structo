<!--
SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>

SPDX-License-Identifier: BSD-2-Clause
-->

# `reloco_ipc_producer` / `reloco_ipc_consumer` / `ipc_producer` / `ipc_consumer`

`include/structo/reloco_ipc_ring.h`, `include/structo/reloco_ipc_ring.hpp`

> Moved from `jplcz_reloco` into `jplcz_structo`'s own `include/structo/`,
> file names and the C/C++ API kept as `reloco_ipc_*`/`reloco::` (not
> re-homed into the `structo` namespace, per explicit instruction to
> leave this component's naming untouched). Documented here as extracted
> from reloco's own reference.

A cross-process, allocation-free single-producer/single-consumer
byte-stream ring buffer over a shared-memory page -- the same demand-
driven-cache-refresh design as `spsc_ring_buffer` in `jplcz_reloco`, but
across a process (or process/kernel) boundary via a raw memory mapping
instead of across threads via a `std::atomic` member.

`reloco_ipc_ring.h` is a standalone, dependency-light C header (no C++
required) defining the wire layout and the raw mount/read/write API:

- `struct reloco_ipc_spsc_page`: the strictly standard-layout page header
  (magic/version/flags/capacity, producer's `write_idx`, consumer's
  `read_idx`, each on its own cache line, followed by the flexible
  `payload[]` array) that both sides map at the same shared-memory
  address.
- `reloco_ipc_validate_mount()`: verifies magic/version/flags/capacity
  (capacity must be a power of two) before either side trusts the page --
  the "IPC security boundary" against a misconfigured or malicious peer.
- `reloco_ipc_producer_init()`/`reloco_ipc_consumer_init()`: mount a
  validated page for writing/reading.
- `reloco_ipc_try_write()`/`reloco_ipc_try_read()`: all-or-nothing raw
  byte transfers, returning a signed `reloco_ipc_ssize_t` (a self-defined
  `int64_t`, not platform `ssize_t`): the requested count on success;
  `0` if there is not currently enough free space/data (benign,
  retryable, like `EAGAIN`); a **negative** `-EFAULT` if the *other*
  side's index was found spoofed/corrupted -- the caller must treat the
  whole page as compromised and stop using it, not retry.

Three build configurations select the atomic-load/store backend via
`RELOCO_IPC_LOAD_ACQUIRE`/`RELOCO_IPC_STORE_RELEASE`/
`RELOCO_IPC_LOAD_RELAXED`: plain userspace/bare-metal (GCC/Clang
`__atomic_*` builtins), `RELOCO_IPC_LINUX_KERNEL` (`smp_load_acquire`/
`smp_store_release`/`READ_ONCE`), and `RELOCO_IPC_FREEBSD_KERNEL`
(`atomic_load_acq_64`/`atomic_store_rel_64`/`atomic_load_64`) -- the same
page format and index protocol is readable from a kernel module on either
side and from ordinary userspace processes on the other.

`reloco_ipc_ring.hpp` wraps the C API in a fallible, RAII, zero-copy C++
layer:

```cpp
void *mapped_page = mmap(...); // shared between the two processes/domains
auto producer = reloco::ipc_producer::create(mapped_page, capacity_bytes);
if (!producer) { /* ABI mismatch or tampered page: producer.error() */ }
```

`ipc_producer::create()`/`ipc_consumer::create()` are the fallible
mounting tier, returning `result<ipc_producer>`/`expected<ipc_consumer,
error>` and failing with `error::invalid_argument` on any
`reloco_ipc_validate_mount()` rejection. `try_write(span<const
uint8_t>)`/`try_read(span<uint8_t>)` mirror the C API's all-or-nothing
byte transfer directly over a `span`, returning `result<size_type>`: `Ok`
holding the transferred count (`data.size()`/`dest.size()` on success, or
`0` for the benign "not enough room/data yet" case), `Err
(error::security_violation)` if the peer's index was found
spoofed/corrupted.

`begin_write(min_bytes)`/`begin_read(min_bytes)` open a zero-copy
transaction instead, also returning a `result<write_tx>`/`result<read_tx>`
for the same reason (a peer-index security-boundary check happens on the
same demand-driven refresh here too): `Err(error::security_violation)` on
detected corruption, `Ok` otherwise holding a `-Wconsumed`
typestate-tracked, `[[nodiscard]]` transaction exposing up to two `span`s
(`chunk1()`/`chunk2()`, `RELOCO_CALLABLE_WHEN(unconsumed)`) directly into
the shared page -- `chunk2()` is non-empty only when the available range
wraps past the end of the ring, exactly like `spsc_ring_buffer`'s own
scatter-gather API. Note the exposed span pair is bounded only by the
ring's total capacity, not clamped to `min_bytes` -- a transaction may
legitimately expose anywhere from `min_bytes` up to the full capacity.
`explicit operator bool()` reports whether at least `min_bytes` were
actually available (an empty transaction is the benign "not enough
room/data yet" case, distinct from the `Err` corruption case above). The
caller writes/reads directly into/from those spans (no intermediate
copy), then calls `commit(bytes)`/`consume(bytes)`
(`RELOCO_SET_TYPESTATE(consumed)`) to publish exactly how many bytes were
produced/consumed -- which may be less than what was reserved.

```cpp
if (auto tx_res = producer.begin_write(5)) {
  auto &tx = tx_res.value();
  if (tx) {
    auto chunk1 = tx.chunk1();
    std::memcpy(chunk1.data(), "HELLO", 5); // zero-copy write directly into shared memory
    tx.commit(5);
  }
}
```

**Optional, opt-in attack-surface fault injection** (C++ only): both the
producer's and consumer's "demand-driven cache refresh" of the *other*
side's shared-memory index -- the exact moment a malicious or buggy peer
process's tampered `read_idx`/`write_idx` gets trusted -- are wired up as
`fault_injection.hpp` fault points, guarded by `#ifdef __cplusplus` in
`reloco_ipc_ring.h` so a plain C/kernel build never sees any of it:
`reloco::ipc_fault::producer_read_idx_refresh`/`consumer_write_idx_refresh`
(only defined when `RELOCO_ENABLE_FAULT_INJECTION` is also defined). Every
such untrusted-field read goes through the header's own
`RELOCO_IPC_LOAD_ACQUIRE_FAULT(ptr, FaultTag, local)` macro, which reads
@p ptr exactly once into @p local and immediately fires @p FaultTag on
that single captured value -- the sanctioned way to add any further
peer-owned-field read without introducing a double-fetch/TOCTOU bug. See
[`tests/test_ipc_ring_fault_injection.cpp`](../tests/test_ipc_ring_fault_injection.cpp)
for worked index-spoofing and single-bit-flip attack simulations against
both the C and C++ APIs. **ODR warning**: enabling this changes the body
of `ipc_producer::write_slices()`/`ipc_consumer::read_slices()`
(implicitly-inline, external linkage), so every translation unit linked
into the same program must define `RELOCO_ENABLE_FAULT_INJECTION`
identically -- keep fault-injection-enabled translation units in their
own separate binary (`jplcz_structo_ipc_ring_fault_injection_tests` in
this repo's `CMakeLists.txt`).

Both `ipc_producer`/`ipc_consumer` are `RELOCO_POINTER`-tagged (they are
thin, non-owning handles over caller-mapped memory -- unmapping the page
while a handle is still alive is the caller's responsibility, same as any
other `mmap` lifetime), move-only, and never allocate.
