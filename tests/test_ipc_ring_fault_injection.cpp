#if !defined(_MSC_VER)

// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

// Exercises reloco_ipc_ring.h/.hpp's built-in fault points --
// reloco::ipc_fault::producer_read_idx_refresh/consumer_write_idx_refresh
// -- for real "attack surface" testing of the two lock-free security
// boundary checks that guard against a malicious/buggy peer process
// spoofing the *other* side's shared-memory index. See
// test_fault_injection.cpp's own file-level comment for why
// RELOCO_ENABLE_FAULT_INJECTION is defined locally here.
#define RELOCO_ENABLE_FAULT_INJECTION

#include <cerrno>
#include <cstring>
#include <gtest/gtest.h>
#include <reloco/fault_injection_patterns.hpp>
#include <reloco/lifetime.hpp>
#include <structo/reloco_ipc_ring.hpp>

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

void format_shared_page(void *memory, uint32_t capacity_bytes) {
  auto *page = static_cast<reloco_ipc_spsc_page *>(memory);
  page->magic = RELOCO_IPC_MAGIC;
  page->version = RELOCO_IPC_VERSION;
  page->flags = 0;
  page->untrusted_capacity = capacity_bytes;
  page->write_idx = 0;
  page->read_idx = 0;
}

struct SharedMemorySim {
  alignas(RELOCO_IPC_CACHE_LINE) reloco::array<uint8_t, 4096> memory{};
  void *data() { return memory.data(); }
  reloco_ipc_spsc_page *get_page() { return std::launder(static_cast<reloco_ipc_spsc_page *>(data())); }
};

} // namespace

// =========================================================================
// C API: without any fault injected, a legitimate concurrent peer having
// really caught up (freeing real room) lets a request that would
// otherwise not fit go through once the demand-driven cache refresh
// re-reads the peer's index. The fault point stands in for a malicious
// peer corrupting that exact value during the very same refresh window,
// and must be rejected instead -- even though, from the caller's own
// point of view, the request looked perfectly legitimate.
// =========================================================================

TEST(IpcRingFaultInjectionTest, CApiProducerAcceptsLegitimateCatchUp) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  reloco_ipc_producer p;
  reloco_ipc_consumer c;
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16));

  uint8_t fill[15] = {0};
  ASSERT_EQ(15, reloco_ipc_try_write(&p, fill, 15)); // write_idx=15, producer's cache still read_idx=0
  uint8_t consumed[10] = {0};
  ASSERT_EQ(10, reloco_ipc_try_read(&c, consumed, 10)); // real read_idx=10; producer hasn't refreshed yet

  uint8_t dummy[10] = {0};
  // Without any fault injected: cached in_use (15) says only 1 byte
  // free, forcing a refresh; the refresh legitimately observes
  // read_idx=10, so 10 bytes really are free and the write succeeds.
  EXPECT_EQ(10, reloco_ipc_try_write(&p, dummy, 10));
}

TEST(IpcRingFaultInjectionTest, CApiProducerRejectsSpoofedReadIdxDuringLegitimateCatchUp) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  reloco_ipc_producer p;
  reloco_ipc_consumer c;
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16));

  uint8_t fill[15] = {0};
  ASSERT_EQ(15, reloco_ipc_try_write(&p, fill, 15)); // write_idx=15, producer's cache still read_idx=0
  uint8_t consumed[10] = {0};
  ASSERT_EQ(10, reloco_ipc_try_read(&c, consumed, 10)); // real read_idx=10: 10 bytes are genuinely free now

  // Arm the fault *after* the legitimate state above is set up, so it
  // only intercepts the producer's upcoming refresh, not the consumer's
  // own read.
  RELOCO_FAULT_SET(fi, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, 0xFFFFFFFFFFFFFFFFull);

  // The exact same request that would legitimately succeed (10 bytes
  // really are free) is instead rejected, because the refresh "observed"
  // a spoofed, wildly out-of-range read_idx.
  uint8_t dummy[10] = {0};
  EXPECT_EQ(0, reloco_ipc_try_write(&p, dummy, 10));
}

TEST(IpcRingFaultInjectionTest, CApiConsumerRejectsSpoofedWriteIdxDuringLegitimateCatchUp) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  reloco_ipc_producer p;
  reloco_ipc_consumer c;
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16)); // consumer's cache: write_idx=0

  uint8_t fill[10] = {0};
  ASSERT_EQ(10, reloco_ipc_try_write(&p, fill, 10)); // real write_idx=10: 10 bytes are genuinely available

  RELOCO_FAULT_SET(fi, reloco::ipc_fault::consumer_write_idx_refresh, std::uint64_t, 0xFFFFFFFFFFFFFFFFull);

  uint8_t dummy[10] = {0};
  // Would legitimately return 10 (all 10 written bytes are really
  // there); the spoofed write_idx (0xFFFF...FFFF, with real read_idx==0)
  // makes `available = w - r` wrap to far beyond `cap` -- a genuine
  // security-boundary trip, not a benign "not enough data" -- so the
  // refresh must make it reject with -EFAULT.
  EXPECT_EQ(-EFAULT, reloco_ipc_try_read(&c, dummy, 10));
}

// =========================================================================
// C++ zero-copy API: the same two fault points, exercised through
// ipc_producer::begin_write()/ipc_consumer::begin_read() instead of the
// all-or-nothing try_write()/try_read() entry points.
// =========================================================================

TEST(IpcRingFaultInjectionTest, CppZeroCopyProducerRejectsSpoofedReadIdxDuringLegitimateCatchUp) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  auto p = reloco::ipc_producer::create(sim.data(), 16).value();
  auto c = reloco::ipc_consumer::create(sim.data(), 16).value();

  uint8_t fill[15] = {0};
  ASSERT_EQ(15, p.try_write(reloco::span<const uint8_t>(fill, 15)).value());
  uint8_t consumed[10] = {0};
  ASSERT_EQ(10, c.try_read(reloco::span<uint8_t>(consumed, 10)).value()); // real read_idx=10

  RELOCO_FAULT_SET(fi, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, 0xFFFFFFFFFFFFFFFFull);

  // Spoofing read_idx to 0xFFFF...FFFF wraps in_use to exactly `cap`
  // (15 - (2^64-1) mod 2^64 == 16) -- not itself an out-of-range value,
  // so this is the benign "not enough room" outcome (Ok, empty
  // transaction), not a detected security violation.
  auto tx_res = p.begin_write(10); // would legitimately fit (10 bytes really free)
  ASSERT_TRUE(tx_res.has_value());
  EXPECT_FALSE(static_cast<bool>(tx_res.value()));
}

TEST(IpcRingFaultInjectionTest, CppZeroCopyConsumerRejectsSpoofedWriteIdxDuringLegitimateCatchUp) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  auto p = reloco::ipc_producer::create(sim.data(), 16).value();
  auto c = reloco::ipc_consumer::create(sim.data(), 16).value();

  uint8_t fill[10] = {0};
  ASSERT_EQ(10, p.try_write(reloco::span<const uint8_t>(fill, 10)).value()); // real write_idx=10

  RELOCO_FAULT_SET(fi, reloco::ipc_fault::consumer_write_idx_refresh, std::uint64_t, 0xFFFFFFFFFFFFFFFFull);

  // Here the spoofed write_idx (0xFFFF...FFFF) makes `available = w - r`
  // (with real r == 0) wrap to a value far beyond `cap` -- a genuine
  // security-boundary trip, surfaced as `Err(error::security_violation)`.
  auto tx_res = c.begin_read(10); // would legitimately fit (10 bytes really written)
  ASSERT_FALSE(tx_res.has_value());
  EXPECT_EQ(reloco::error::security_violation, tx_res.error());
}

// =========================================================================
// Bit-flip corruption ("single event upset"-style attack): XOR a single
// bit into the refreshed index instead of an unconditional overwrite,
// and confirm `reloco_ipc_try_write()`/`reloco_ipc_try_read()` never
// produce a partial transfer -- only ever exactly the requested count
// (success), exactly `0` (benign, not enough room/data), or exactly
// `-EFAULT` (security boundary tripped) -- no matter which bit gets
// corrupted.
// =========================================================================

TEST(IpcRingFaultInjectionTest, ProducerAllOrNothingInvariantSurvivesSingleBitFlipOnRefresh) {
  for (int bit = 0; bit < 64; ++bit) {
    SharedMemorySim sim;
    format_shared_page(sim.data(), 16);
    reloco_ipc_producer p;
    reloco_ipc_consumer c;
    ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
    ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16));

    uint8_t fill[15] = {0};
    ASSERT_EQ(15, reloco_ipc_try_write(&p, fill, 15));
    uint8_t consumed[10] = {0};
    ASSERT_EQ(10, reloco_ipc_try_read(&c, consumed, 10)); // real read_idx=10

    RELOCO_FAULT_XOR(fi, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, std::uint64_t{1} << bit);

    uint8_t dummy[10] = {0};
    reloco_ipc_ssize_t written = reloco_ipc_try_write(&p, dummy, 10);
    EXPECT_TRUE(written == 0 || written == 10 || written == -EFAULT)
        << "bit " << bit << " produced partial count " << written;
  }
}

TEST(IpcRingFaultInjectionTest, ConsumerAllOrNothingInvariantSurvivesSingleBitFlipOnRefresh) {
  // Mirror of the producer sweep above, on reloco_ipc_try_read()'s own
  // demand-driven write_idx refresh.
  for (int bit = 0; bit < 64; ++bit) {
    SharedMemorySim sim;
    format_shared_page(sim.data(), 16);
    reloco_ipc_producer p;
    reloco_ipc_consumer c;
    ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
    ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16)); // consumer's cache: write_idx=0

    uint8_t fill[10] = {0};
    ASSERT_EQ(10, reloco_ipc_try_write(&p, fill, 10)); // real write_idx=10; consumer hasn't refreshed yet

    RELOCO_FAULT_XOR(fi, reloco::ipc_fault::consumer_write_idx_refresh, std::uint64_t, std::uint64_t{1} << bit);

    uint8_t dummy[10] = {0};
    reloco_ipc_ssize_t got = reloco_ipc_try_read(&c, dummy, 10);
    EXPECT_TRUE(got == 0 || got == 10 || got == -EFAULT) << "bit " << bit << " produced partial count " << got;
  }
}

// =========================================================================
// C++ zero-copy API: the same two bit-flip sweeps, through
// begin_write()/begin_read() instead of try_write()/try_read() -- these
// exercise write_slices()/read_slices()'s own independent overflow
// verification (RELOCO_ASSERT(available <= cap, ...) /
// RELOCO_ASSERT(physical_w/physical_r < cap, ...)) under every possible
// single-bit corruption, confirming those assertions never spuriously
// fire. Unlike try_write()/try_read(), a zero-copy transaction is *not*
// clamped to the requested `min_bytes` -- begin_write()/begin_read()
// expose the entire currently-available region (bounded only by `cap`)
// -- so the invariant checked here is: `Err` is always exactly
// `error::security_violation`; `Ok` is always either an empty
// transaction (benign) or one whose total size falls in
// `[min_bytes, cap]`, never outside that range.
// =========================================================================

TEST(IpcRingFaultInjectionTest, CppZeroCopyProducerSurvivesSingleBitFlipOnRefresh) {
  constexpr reloco::ipc_producer::size_type kMinBytes = 10;
  constexpr reloco::ipc_producer::size_type kCap = 16;
  for (int bit = 0; bit < 64; ++bit) {
    SharedMemorySim sim;
    format_shared_page(sim.data(), kCap);
    auto p = reloco::ipc_producer::create(sim.data(), kCap).value();
    auto c = reloco::ipc_consumer::create(sim.data(), kCap).value();

    uint8_t fill[15] = {0};
    ASSERT_EQ(15, p.try_write(reloco::span<const uint8_t>(fill, 15)).value());
    uint8_t consumed[10] = {0};
    ASSERT_EQ(10, c.try_read(reloco::span<uint8_t>(consumed, 10)).value()); // real read_idx=10

    RELOCO_FAULT_XOR(fi, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, std::uint64_t{1} << bit);

    auto tx_res = p.begin_write(kMinBytes);
    if (!tx_res) {
      EXPECT_EQ(reloco::error::security_violation, tx_res.error()) << "bit " << bit;
      continue;
    }
    auto &tx = tx_res.value();
    if (tx) {
      auto total = tx.chunk1().size() + tx.chunk2().size();
      EXPECT_GE(total, kMinBytes) << "bit " << bit;
      EXPECT_LE(total, kCap) << "bit " << bit;
      tx.commit(static_cast<reloco::ipc_producer::size_type>(total));
    }
  }
}

TEST(IpcRingFaultInjectionTest, CppZeroCopyConsumerSurvivesSingleBitFlipOnRefresh) {
  constexpr reloco::ipc_consumer::size_type kMinBytes = 10;
  constexpr reloco::ipc_consumer::size_type kCap = 16;
  for (int bit = 0; bit < 64; ++bit) {
    SharedMemorySim sim;
    format_shared_page(sim.data(), kCap);
    auto p = reloco::ipc_producer::create(sim.data(), kCap).value();
    auto c = reloco::ipc_consumer::create(sim.data(), kCap).value();

    uint8_t fill[10] = {0};
    ASSERT_EQ(10, p.try_write(reloco::span<const uint8_t>(fill, 10)).value()); // real write_idx=10

    RELOCO_FAULT_XOR(fi, reloco::ipc_fault::consumer_write_idx_refresh, std::uint64_t, std::uint64_t{1} << bit);

    auto tx_res = c.begin_read(kMinBytes);
    if (!tx_res) {
      EXPECT_EQ(reloco::error::security_violation, tx_res.error()) << "bit " << bit;
      continue;
    }
    auto &tx = tx_res.value();
    if (tx) {
      auto total = tx.chunk1().size() + tx.chunk2().size();
      EXPECT_GE(total, kMinBytes) << "bit " << bit;
      EXPECT_LE(total, kCap) << "bit " << bit;
      tx.consume(static_cast<reloco::ipc_consumer::size_type>(total));
    }
  }
}

// =========================================================================
// Tag isolation: arming the *other* side's fault point must have zero
// effect on this side's refresh -- the shared, program-wide TLS stack
// (see fault_injection.hpp's "Storage" section) is walked looking for a
// matching (Tag, Args...) signature, so an armed-but-mismatched entry
// higher up the stack must be skipped over rather than misfiring. A
// copy-pasted wrong `Tag` at a call site, or a dispatch bug in the
// shared-stack walk, would show up here as the "wrong side" mutating a
// value it has no business touching.
// =========================================================================

TEST(IpcRingFaultInjectionTest, CApiProducerIgnoresConsumerTagFaultDuringRealCatchUp) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  reloco_ipc_producer p;
  reloco_ipc_consumer c;
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16));

  uint8_t fill[15] = {0};
  ASSERT_EQ(15, reloco_ipc_try_write(&p, fill, 15));
  uint8_t consumed[10] = {0};
  ASSERT_EQ(10, reloco_ipc_try_read(&c, consumed, 10)); // real read_idx=10

  // Armed for the *wrong* tag: must not intercept the producer's own
  // read_idx refresh below.
  RELOCO_FAULT_SET(fi, reloco::ipc_fault::consumer_write_idx_refresh, std::uint64_t, 0xFFFFFFFFFFFFFFFFull);

  uint8_t dummy[10] = {0};
  // Same legitimate catch-up as CApiProducerAcceptsLegitimateCatchUp:
  // must still succeed, completely unaffected by the mismatched-tag hook.
  EXPECT_EQ(10, reloco_ipc_try_write(&p, dummy, 10));
}

TEST(IpcRingFaultInjectionTest, CApiConsumerIgnoresProducerTagFaultDuringRealCatchUp) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  reloco_ipc_producer p;
  reloco_ipc_consumer c;
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16));

  uint8_t fill[10] = {0};
  ASSERT_EQ(10, reloco_ipc_try_write(&p, fill, 10)); // real write_idx=10

  RELOCO_FAULT_SET(fi, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, 0xFFFFFFFFFFFFFFFFull);

  uint8_t dummy[10] = {0};
  EXPECT_EQ(10, reloco_ipc_try_read(&c, dummy, 10));
}

// =========================================================================
// Stacked/nested fault injectors (LIFO): the outer instance's effect
// must resume, unchanged, the instant the inner one is destroyed --
// exactly the "stackable, caller-owned" contract fault_injection.hpp
// documents, exercised here against real ipc_ring call sites instead of
// a synthetic fault point.
// =========================================================================

TEST(IpcRingFaultInjectionTest, NestedFaultInjectorsRestoreOuterEffectAfterInnerDestructs) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  reloco_ipc_producer p;
  reloco_ipc_consumer c;
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16));

  uint8_t fill[15] = {0};
  ASSERT_EQ(15, reloco_ipc_try_write(&p, fill, 15));
  uint8_t consumed[10] = {0};
  ASSERT_EQ(10, reloco_ipc_try_read(&c, consumed, 10)); // real read_idx=10

  // Outer: spoofs read_idx to a nonsensical value -> every refresh while
  // only this is armed must be rejected.
  RELOCO_FAULT_SET(outer, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, 0xFFFFFFFFFFFFFFFFull);

  uint8_t dummy[10] = {0};
  ASSERT_EQ(0, reloco_ipc_try_write(&p, dummy, 10)); // outer's corruption rejected, as expected

  {
    // Inner: shadows the outer hook with the *real* value, so nesting
    // this on top must let the legitimate catch-up through again.
    RELOCO_FAULT_SET(inner, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, 10ull);
    EXPECT_EQ(10, reloco_ipc_try_write(&p, dummy, 10));
  } // inner destroyed here: LIFO restore of `outer`

  // `outer` (still spoofing read_idx to the same nonsensical
  // 0xFFFF...FFFF) is active again -- but real state has moved on since
  // the first assertion above (the inner scope's legitimate write
  // advanced write_idx by 10), so the very same spoofed value now makes
  // `in_use = w - r` wrap to a value *beyond* `cap` instead of landing
  // exactly on it: what was a benign "no room" rejection before is now
  // a detected security violation. This still demonstrates the LIFO
  // restore -- `outer`'s hook, not `inner`'s, is once again the one
  // intercepting the refresh -- just against a different real `w`.
  EXPECT_EQ(-EFAULT, reloco_ipc_try_write(&p, dummy, 10));
}

// =========================================================================
// Mount-time (init) fault injection: reloco_ipc_producer_init()/
// reloco_ipc_consumer_init() run the very same LOAD_ACQUIRE_FAULT as the
// on-demand refresh inside try_write()/try_read(), so a peer that has
// already made real, legitimate progress before this side ever mounts
// can still have that mount-time read corrupted. Confirm the same
// "fail safe" property holds here too: a spoofed *initial* read can only
// make this side wrongly conservative (reject a request that would have
// legitimately fit), never let it read/write past data the other side
// hasn't actually finished with.
// =========================================================================

TEST(IpcRingFaultInjectionTest, ProducerRejectsSpoofedReadIdxObservedAtMountTime) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  // Simulate a page that already has real, prior activity before this
  // producer instance ever mounts it: 15 bytes written, 10 already
  // genuinely consumed (5 live, unread bytes remain -> 11 bytes free).
  sim.get_page()->write_idx = 15;
  sim.get_page()->read_idx = 10;

  reloco_ipc_producer p;
  // Arm *before* mounting: corrupts the very first read of read_idx,
  // inside reloco_ipc_producer_init() itself.
  RELOCO_FAULT_SET(fi, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, 0xFFFFFFFFFFFFFFFFull);
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));

  uint8_t dummy[11] = {0};
  // The real, unspoofed state has exactly 11 bytes free (cap=16,
  // in_use=5); a spoofed mount-time read must not let this succeed with
  // a miscomputed offset -- it must reject (conservative failure), never
  // report success while writing over the still-unread [10, 15) region.
  EXPECT_EQ(0, reloco_ipc_try_write(&p, dummy, 11));
}

TEST(IpcRingFaultInjectionTest, ConsumerRejectsSpoofedWriteIdxObservedAtMountTime) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  // 10 bytes genuinely written, nothing consumed yet.
  sim.get_page()->write_idx = 10;
  sim.get_page()->read_idx = 0;

  reloco_ipc_consumer c;
  RELOCO_FAULT_SET(fi, reloco::ipc_fault::consumer_write_idx_refresh, std::uint64_t, 0xFFFFFFFFFFFFFFFFull);
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16));

  uint8_t dummy[10] = {0};
  // The real state has exactly 10 bytes genuinely available to read; the
  // spoofed mount-time write_idx (0xFFFF...FFFF, with real read_idx==0)
  // makes `available = w - r` wrap to far beyond `cap` -- a genuine
  // security-boundary trip -- so it must reject with -EFAULT, rather
  // than read past what the producer actually committed.
  EXPECT_EQ(-EFAULT, reloco_ipc_try_read(&c, dummy, 10));
}

// =========================================================================
// Boundary-value spoofing: off-by-one errors at the `> cap` security
// check are exactly the kind of bug fault injection here is meant to
// catch -- confirm the boundary itself (`in_use`/`available` == cap,
// exactly at the strict-greater-than threshold) is handled correctly in
// both directions, not just clearly-out-of-range values like
// `0xFFFFFFFFFFFFFFFF`.
// =========================================================================

TEST(IpcRingFaultInjectionTest, ProducerAcceptsReadIdxSpoofedToExactlyDrainBuffer) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  reloco_ipc_producer p;
  reloco_ipc_consumer c;
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16));

  uint8_t fill[15] = {0};
  ASSERT_EQ(15, reloco_ipc_try_write(&p, fill, 15)); // real write_idx=15

  // Spoof read_idx to exactly write_idx (in_use == 0, "fully drained" --
  // the most extreme *legitimate-looking* value): the whole capacity
  // must become available, exactly at the boundary, not off by one.
  RELOCO_FAULT_SET(fi, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, 15ull);

  uint8_t dummy[16] = {0};
  EXPECT_EQ(16, reloco_ipc_try_write(&p, dummy, 16)); // exactly cap bytes must fit
}

TEST(IpcRingFaultInjectionTest, ProducerRejectsReadIdxSpoofedOneByteBeyondWriteIdx) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  reloco_ipc_producer p;
  reloco_ipc_consumer c;
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 16));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 16));

  uint8_t fill[15] = {0};
  ASSERT_EQ(15, reloco_ipc_try_write(&p, fill, 15)); // real write_idx=15

  // Spoof read_idx to write_idx + 1 (the consumer claiming to have
  // consumed a byte the producer hasn't even written yet): the smallest
  // possible "impossible" value, one past the legitimate boundary above.
  RELOCO_FAULT_SET(fi, reloco::ipc_fault::producer_read_idx_refresh, std::uint64_t, 16ull);

  // A request of just 1 byte would be satisfied entirely from the
  // producer's *cached* state (15 in use, 1 free) without ever
  // re-reading read_idx, so it would never actually exercise the armed
  // fault point. Request 2 bytes instead: the cache (only 1 byte free)
  // is insufficient, forcing the demand-driven refresh that observes
  // the spoofed read_idx == 16 (write_idx + 1), which makes
  // `in_use = w - r` underflow far past `cap` -- a genuine
  // security-boundary trip.
  uint8_t dummy[2] = {0};
  EXPECT_EQ(-EFAULT, reloco_ipc_try_write(&p, dummy, 2));
}

RELOCO_END_UNSAFE_BUFFER_USAGE

#endif