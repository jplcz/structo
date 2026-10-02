#if !defined(_MSC_VER)
#include <cerrno>
#include <cstring>
#include <gtest/gtest.h>
#include <reloco/lifetime.hpp>
#include <structo/reloco_ipc_ring.hpp>

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

// =========================================================================
// TEST HELPERS
// =========================================================================

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
// TEST CASES
// =========================================================================

TEST(IpcRingBufferCppTest, FallibleMountValidation) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 1024);

  // 1. Success case
  auto p_res = reloco::ipc_producer::create(sim.data(), 1024);
  auto c_res = reloco::ipc_consumer::create(sim.data(), 1024);
  EXPECT_TRUE(p_res.has_value());
  EXPECT_TRUE(c_res.has_value());

  // 2. Graceful Failure: Expected capacity does not match
  auto bad_cap_res = reloco::ipc_producer::create(sim.data(), 2048);
  EXPECT_FALSE(bad_cap_res.has_value());

  // 3. Graceful Failure: Flags field was tampered with
  auto *page = static_cast<reloco_ipc_spsc_page *>(sim.data());
  page->flags = 1; // Tamper with ABI flags

  auto tampered_res = reloco::ipc_consumer::create(sim.data(), 1024);
  EXPECT_FALSE(tampered_res.has_value());
}

TEST(IpcRingBufferCppTest, BasicByteStreamWriteAndRead) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 32);
  auto p = reloco::ipc_producer::create(sim.data(), 32).value();
  auto c = reloco::ipc_consumer::create(sim.data(), 32).value();

  reloco::array<uint8_t, 4> in_data = {0xDE, 0xAD, 0xBE, 0xEF};

  // Write 4 bytes
  EXPECT_EQ(4, p.try_write(reloco::span<const uint8_t>(in_data.data(), in_data.size())).value());

  reloco::array<uint8_t, 6> out_data = {0};

  // try_read is strictly all-or-nothing: requesting 6 bytes when only 4
  // are available is a benign "not enough data yet" (Ok(0)), not a
  // partial read.
  EXPECT_EQ(0, c.try_read(reloco::span<uint8_t>(out_data.data(), out_data.size())).value());

  // Requesting exactly the 4 available bytes succeeds in full.
  EXPECT_EQ(4, c.try_read(reloco::span<uint8_t>(out_data.data(), 4)).value());

  EXPECT_EQ(0xDE, out_data[0]);
  EXPECT_EQ(0xAD, out_data[1]);
  EXPECT_EQ(0xBE, out_data[2]);
  EXPECT_EQ(0xEF, out_data[3]);
  EXPECT_EQ(0x00, out_data[4]); // Untouched
}

TEST(IpcRingBufferCppTest, ZeroCopyTransactions_Linear) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  auto p = reloco::ipc_producer::create(sim.data(), 16).value();
  auto c = reloco::ipc_consumer::create(sim.data(), 16).value();

  // ---- PRODUCER ----
  {
    auto tx_res = p.begin_write(5);
    ASSERT_TRUE(tx_res.has_value());
    auto tx = std::move(tx_res).value();
    ASSERT_TRUE(static_cast<bool>(tx));

    EXPECT_EQ(16, tx.chunk1().size()); // 16 contiguous bytes available
    EXPECT_EQ(0, tx.chunk2().size());

    // Write a 5-byte sequence
    uint8_t seq[] = {'H', 'E', 'L', 'L', 'O'};

    auto chunk1 = tx.chunk1();
    std::memcpy(chunk1.data(), seq, 5);

    tx.commit(5);
  }

  // ---- CONSUMER ----
  {
    auto tx_res = c.begin_read(3);
    ASSERT_TRUE(tx_res.has_value());
    auto tx = std::move(tx_res).value();
    ASSERT_TRUE(static_cast<bool>(tx));
    auto chunk1 = tx.chunk1();

    EXPECT_EQ(5, chunk1.size());

    EXPECT_EQ('H', chunk1[0]);
    EXPECT_EQ('E', chunk1[1]);

    tx.consume(2); // Consume only 'H' and 'E'
  }
}

TEST(IpcRingBufferCppTest, ZeroCopyTransactions_WrapAroundSplit) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 8); // Tiny 8-byte capacity to force wrap
  auto p = reloco::ipc_producer::create(sim.data(), 8).value();
  auto c = reloco::ipc_consumer::create(sim.data(), 8).value();

  // Step 1: Advance indices near the end. write_idx = 6, read_idx = 6.
  uint8_t dummy[6] = {0};
  std::ignore = p.try_write(reloco::span<const uint8_t>(dummy, 6));
  std::ignore = c.try_read(reloco::span<uint8_t>(dummy, 6));

  // Step 2: The producer requests 4 bytes.
  // Physical index is 6, capacity is 8. It must split (2 bytes at end, 2 at start).
  {
    auto tx_res = p.begin_write(4);
    ASSERT_TRUE(tx_res.has_value());
    auto tx = std::move(tx_res).value();
    ASSERT_TRUE(static_cast<bool>(tx));

    auto chunk1 = tx.chunk1();
    auto chunk2 = tx.chunk2();

    ASSERT_EQ(2, chunk1.size());
    ASSERT_EQ(6, chunk2.size()); // 6 bytes remaining at the wrap

    chunk1[0] = 0x11;
    chunk1[1] = 0x22;
    chunk2[0] = 0x33;
    chunk2[1] = 0x44;

    tx.commit(4);
  }

  // Step 3: The consumer reads the 4 bytes.
  {
    auto tx_res = c.begin_read(4);
    ASSERT_TRUE(tx_res.has_value());
    auto tx = std::move(tx_res).value();
    ASSERT_TRUE(static_cast<bool>(tx));

    auto chunk1 = tx.chunk1();
    auto chunk2 = tx.chunk2();

    ASSERT_EQ(2, chunk1.size());
    ASSERT_EQ(2, chunk2.size()); // Only 2 bytes available in chunk2

    EXPECT_EQ(0x11, chunk1[0]);
    EXPECT_EQ(0x22, chunk1[1]);
    EXPECT_EQ(0x33, chunk2[0]);
    EXPECT_EQ(0x44, chunk2[1]);

    tx.consume(4);
  }
}

TEST(IpcRingBufferCppTest, TransactionMoveSemantics) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 8);
  auto p = reloco::ipc_producer::create(sim.data(), 8).value();

  auto tx1_res = p.begin_write(2);
  ASSERT_TRUE(tx1_res.has_value());
  auto tx1 = std::move(tx1_res).value();
  ASSERT_TRUE(static_cast<bool>(tx1));

  auto tx2 = std::move(tx1);

  EXPECT_FALSE(static_cast<bool>(tx1)); // Invalidated by move
  ASSERT_TRUE(static_cast<bool>(tx2));  // Takes ownership

  auto chunk1 = tx2.chunk1();

  chunk1[0] = 0xFF;
  tx2.commit(1);
}

TEST(IpcRingBufferCApiTest, ValidationAndMounting) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 1024);
  auto *page = sim.get_page();

  // 1. Success case
  EXPECT_EQ(0, reloco_ipc_validate_mount(page, 1024));

  // 2. Capacity Mismatch
  EXPECT_NE(0, reloco_ipc_validate_mount(page, 512));

  // 3. Tampered Magic
  page->magic = 0xDEADBEEF;
  EXPECT_NE(0, reloco_ipc_validate_mount(page, 1024));
  page->magic = RELOCO_IPC_MAGIC; // Restore

  // 4. Invalid Capacity (Not power of 2)
  page->untrusted_capacity = 1000;
  EXPECT_NE(0, reloco_ipc_validate_mount(page, 1000));
}

TEST(IpcRingBufferCApiTest, BasicWriteAndRead) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 32);

  reloco_ipc_producer p{};
  reloco_ipc_consumer c{};
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 32));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 32));

  uint8_t in_data[] = {0xAA, 0xBB, 0xCC};

  // Write 3 bytes
  EXPECT_EQ(3, reloco_ipc_try_write(&p, in_data, 3));

  // Read 3 bytes
  uint8_t out_data[3] = {0};
  EXPECT_EQ(3, reloco_ipc_try_read(&c, out_data, 3));

  EXPECT_EQ(0xAA, out_data[0]);
  EXPECT_EQ(0xBB, out_data[1]);
  EXPECT_EQ(0xCC, out_data[2]);
}

TEST(IpcRingBufferCApiTest, MemoryWrapAround) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 8); // Tiny 8-byte capacity

  reloco_ipc_producer p{};
  reloco_ipc_consumer c{};
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, sim.get_page(), 8));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, sim.get_page(), 8));

  uint8_t stream_in[] = {10, 20, 30, 40, 50, 60, 70};
  uint8_t stream_out[7] = {0};

  // 1. Advance the ring buffer physical index to 6.
  EXPECT_EQ(6, reloco_ipc_try_write(&p, stream_in, 6));
  EXPECT_EQ(6, reloco_ipc_try_read(&c, stream_out, 6));

  // 2. Write 4 bytes. This WRAPS the physical buffer (index 6, 7, 0, 1)
  uint8_t wrap_in[] = {0x11, 0x22, 0x33, 0x44};
  EXPECT_EQ(4, reloco_ipc_try_write(&p, wrap_in, 4));

  // 3. Read 4 bytes (wrapping read).
  uint8_t wrap_out[4] = {0};
  EXPECT_EQ(4, reloco_ipc_try_read(&c, wrap_out, 4));

  EXPECT_EQ(0x11, wrap_out[0]);
  EXPECT_EQ(0x22, wrap_out[1]);
  EXPECT_EQ(0x33, wrap_out[2]);
  EXPECT_EQ(0x44, wrap_out[3]);
}

TEST(IpcRingBufferCApiTest, SecurityIndexSpoofing) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 16);
  auto *page = sim.get_page();

  reloco_ipc_producer p{};
  reloco_ipc_consumer c{};
  ASSERT_EQ(0, reloco_ipc_producer_init(&p, page, 16));
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c, page, 16));

  uint8_t dummy = 0xFF;
  EXPECT_EQ(1, reloco_ipc_try_write(&p, &dummy, 1));

  // ATTACK 1: Consumer maliciously advances read_idx past write_idx
  page->read_idx = 500;

  // Exhaust local cache first so it reads the malicious shared memory value
  uint8_t fill[15] = {0};
  EXPECT_EQ(15, reloco_ipc_try_write(&p, fill, 15));

  // The trap snaps shut - returns -EFAULT (corruption detected), not a
  // benign 0, and the caller must stop using the ring altogether.
  EXPECT_EQ(-EFAULT, reloco_ipc_try_write(&p, &dummy, 1));

  // Reset page for Attack 2
  format_shared_page(sim.data(), 16);
  reloco_ipc_producer_init(&p, page, 16);
  reloco_ipc_consumer_init(&c, page, 16);

  // ATTACK 2: Producer maliciously advances write_idx > capacity
  page->write_idx = 64;

  // Consumer trap snaps shut
  EXPECT_EQ(-EFAULT, reloco_ipc_try_read(&c, &dummy, 1));
}

// =========================================================================
// INTEROP TESTS (C Kernel API <-> C++ User-Space API)
// =========================================================================

TEST(IpcRingBufferInteropTest, CProducer_To_CppConsumer) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 32);

  // Mount C Producer (e.g., The Kernel)
  reloco_ipc_producer p_c{};
  ASSERT_EQ(0, reloco_ipc_producer_init(&p_c, sim.get_page(), 32));

  // Mount C++ Consumer (e.g., The Host App)
  auto c_cpp = reloco::ipc_consumer::create(sim.data(), 32).value();

  // Kernel writes data via C API
  uint8_t payload_in[] = {0xAA, 0xBB, 0xCC, 0xDD};
  EXPECT_EQ(4, reloco_ipc_try_write(&p_c, payload_in, 4));

  // Host reads data via C++ Zero-Copy API
  auto tx_res = c_cpp.begin_read(4);
  ASSERT_TRUE(tx_res.has_value());
  auto tx = std::move(tx_res).value();
  ASSERT_TRUE(static_cast<bool>(tx));

  auto chunk1 = tx.chunk1();
  ASSERT_EQ(4, chunk1.size());
  EXPECT_EQ(0xAA, chunk1[0]);
  EXPECT_EQ(0xDD, chunk1[3]);

  tx.consume(4);
}

TEST(IpcRingBufferInteropTest, CppProducer_To_CConsumer_WithWrap) {
  SharedMemorySim sim;
  format_shared_page(sim.data(), 8); // Tiny 8-byte capacity to force wrap

  // Mount C++ Producer (e.g., The Host App)
  auto p_cpp = reloco::ipc_producer::create(sim.data(), 8).value();

  // Mount C Consumer (e.g., The Kernel)
  reloco_ipc_consumer c_c{};
  ASSERT_EQ(0, reloco_ipc_consumer_init(&c_c, sim.get_page(), 8));

  // Shift indices near the end to force a wrap. write_idx = 6.
  uint8_t dummy[6] = {0};
  std::ignore = p_cpp.try_write(reloco::span<const uint8_t>(dummy, 6));
  reloco_ipc_try_read(&c_c, dummy, 6);

  // Host writes data via C++ Zero-Copy API (Spanning across the wrap)
  {
    auto tx_res = p_cpp.begin_write(4);
    ASSERT_TRUE(tx_res.has_value());
    auto tx = std::move(tx_res).value();
    ASSERT_TRUE(static_cast<bool>(tx));

    auto chunk1 = tx.chunk1();
    auto chunk2 = tx.chunk2();

    ASSERT_EQ(2, chunk1.size());
    ASSERT_EQ(6, chunk2.size()); // 6 bytes theoretically available after wrap

    chunk1[0] = 0x11;
    chunk1[1] = 0x22;
    chunk2[0] = 0x33;
    chunk2[1] = 0x44;

    tx.commit(4);
  }

  // Kernel reads data via C API
  // The C API does the math internally and issues two memcpy calls to stitch it.
  uint8_t payload_out[4] = {0};
  EXPECT_EQ(4, reloco_ipc_try_read(&c_c, payload_out, 4));

  EXPECT_EQ(0x11, payload_out[0]);
  EXPECT_EQ(0x22, payload_out[1]);
  EXPECT_EQ(0x33, payload_out[2]);
  EXPECT_EQ(0x44, payload_out[3]);
}

RELOCO_END_UNSAFE_BUFFER_USAGE
#endif