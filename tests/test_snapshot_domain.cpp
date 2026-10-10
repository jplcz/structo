// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/memory_hotplug.hpp>
#include <structo/memory_segment_map.hpp>
#include <structo/snapshot_domain.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo;
using namespace std::chrono_literals;

std::mutex g_wait_mutex;
std::condition_variable g_wait_cv;
std::atomic<std::size_t> g_shard_hint{0};

struct host_traits {
  static constexpr std::size_t shards = 4;
  static std::size_t current_shard() noexcept {
    thread_local const std::size_t mine = g_shard_hint.fetch_add(1);
    return mine;
  }
  using mutex_type = std::mutex;
  static void wait(std::atomic<std::uint32_t> &word, std::uint32_t seen) noexcept {
    std::unique_lock<std::mutex> lk(g_wait_mutex);
    if (word.load() == seen) {
      g_wait_cv.wait_for(lk, 1ms);
    }
  }
  static void wake_all(std::atomic<std::uint32_t> &) noexcept {
    std::lock_guard<std::mutex> lk(g_wait_mutex);
    g_wait_cv.notify_all();
  }
};

struct pair_value {
  std::uint64_t a{0};
  std::uint64_t b{0};
};

TEST(SnapshotDomain, ReadsInitialAndUpdated) {
  snapshot_domain<pair_value, host_traits> d(pair_value{1, 1});
  {
    auto g = d.read();
    EXPECT_EQ(g->a, 1u);
  }
  ASSERT_TRUE(d.update([](const pair_value &c) -> reloco::result<pair_value> { return pair_value{c.a + 1, c.b + 1}; })
                  .has_value());
  EXPECT_EQ(d.read()->a, 2u);
}

TEST(SnapshotDomain, BuildErrorChangesNothing) {
  snapshot_domain<pair_value, host_traits> d(pair_value{5, 5});
  auto r = d.update([](const pair_value &) -> reloco::result<pair_value> {
    return reloco::unexpected(reloco::error::invalid_argument);
  });
  EXPECT_EQ(r.error(), reloco::error::invalid_argument);
  EXPECT_EQ(d.read()->a, 5u);
}

TEST(SnapshotDomain, UpdateWaitsForPinnedReader) {
  snapshot_domain<pair_value, host_traits> d(pair_value{1, 1});
  std::atomic<bool> done{false};
  std::thread writer;
  {
    auto g = d.read(); // pins the old value
    writer = std::thread([&] {
      (void)d.update([](const pair_value &) -> reloco::result<pair_value> { return pair_value{2, 2}; });
      done = true;
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(done.load());          // blocked: a reader still holds the old value
    EXPECT_EQ(g->a, 1u);                // and it still sees the old value, intact
    EXPECT_EQ(d.read()->a, 2u);         // new readers already see the new one
  }
  writer.join();
  EXPECT_TRUE(done.load());
}

TEST(SnapshotDomain, AsyncPublishThenSynchronize) {
  snapshot_domain<pair_value, host_traits> d(pair_value{1, 1});
  std::atomic<bool> synced{false};
  std::thread waiter;
  {
    auto g = d.read();
    ASSERT_TRUE(
        d.update_async([](const pair_value &) -> reloco::result<pair_value> { return pair_value{2, 2}; }).has_value());
    waiter = std::thread([&] {
      d.synchronize();
      synced = true;
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(synced.load());
  }
  waiter.join();
  EXPECT_TRUE(synced.load());
}

TEST(SnapshotDomain, ConcurrentReadersNeverSeeTornValue) {
  snapshot_domain<pair_value, host_traits> d(pair_value{0, 0});
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> bad{0};
  std::vector<std::thread> readers;
  for (int i = 0; i < 4; ++i) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        auto g = d.read();
        const pair_value v = *g;
        if (v.a != v.b) {
          ++bad;
        }
      }
    });
  }
  for (std::uint64_t i = 1; i <= 2000; ++i) {
    ASSERT_TRUE(d.update([i](const pair_value &) -> reloco::result<pair_value> { return pair_value{i, i}; })
                    .has_value());
  }
  stop = true;
  for (auto &t : readers) {
    t.join();
  }
  EXPECT_EQ(bad.load(), 0u);
  EXPECT_EQ(d.read()->a, 2000u);
}

// ---- memory_hotplug --------------------------------------------------------------------------------

struct hpage {
  std::uint32_t flags{0};
};
struct hnode {
  std::uint8_t node{0};
};
using seg_map = fixed_memory_segment_map<hpage, hnode, 4, 32, page_4k, default_phys_space, 4>;
using pfn_t = seg_map::pfn_type;

TEST(MemoryHotplug, AddMakesSegmentVisible) {
  memory_hotplug<seg_map, host_traits> hp;
  std::vector<hpage> pages(32);
  ASSERT_TRUE(hp.add({pfn_t{0x100}, 32, hnode{1}, reloco::span<hpage>(pages)}).has_value());
  auto g = hp.read();
  auto h = g->find(pfn_t{0x105});
  ASSERT_TRUE(h.has_value());
  EXPECT_EQ(&g->page_at(*h), &pages[5]);
  // Overlap is rejected and leaves the published map alone.
  std::vector<hpage> other(16);
  EXPECT_FALSE(hp.add({pfn_t{0x110}, 16, hnode{1}, reloco::span<hpage>(other)}).has_value());
}

TEST(MemoryHotplug, RemoveWaitsForReadersBeforeRelease) {
  memory_hotplug<seg_map, host_traits> hp;
  std::vector<hpage> pages(32);
  ASSERT_TRUE(hp.add({pfn_t{0x100}, 32, hnode{1}, reloco::span<hpage>(pages)}).has_value());

  std::atomic<bool> released{false};
  std::thread remover;
  {
    auto g = hp.read(); // holds a pointer into pages[] through the old map
    auto h = g->find(pfn_t{0x100});
    ASSERT_TRUE(h.has_value());
    hpage &held = g->page_at(*h);
    remover = std::thread([&] {
      EXPECT_TRUE(hp.remove(pfn_t{0x100}, [&](const seg_map::segment &s) {
                      EXPECT_EQ(s.page_count, 32u);
                      released = true; // here the caller would free the descriptor array
                    }).has_value());
    });
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(released.load()); // still pinned: 'held' must stay valid
    held.flags = 1;
    EXPECT_FALSE(hp.read()->find(pfn_t{0x100}).has_value()); // but new lookups no longer find it
  }
  remover.join();
  EXPECT_TRUE(released.load());
  EXPECT_EQ(pages[0].flags, 1u);
}

TEST(MemoryHotplug, RemoveMissingSegment) {
  memory_hotplug<seg_map, host_traits> hp;
  bool called = false;
  auto r = hp.remove(pfn_t{0x100}, [&](const seg_map::segment &) { called = true; });
  EXPECT_EQ(r.error(), reloco::error::not_found);
  EXPECT_FALSE(called);
}

TEST(MemoryHotplug, AddThenRemoveCycles) {
  memory_hotplug<seg_map, host_traits> hp;
  std::vector<hpage> a(16), b(16);
  for (int i = 0; i < 10; ++i) {
    ASSERT_TRUE(hp.add({pfn_t{0x100}, 16, hnode{0}, reloco::span<hpage>(a)}).has_value());
    ASSERT_TRUE(hp.add({pfn_t{0x200}, 16, hnode{1}, reloco::span<hpage>(b)}).has_value());
    EXPECT_TRUE(hp.read()->find(pfn_t{0x205}).has_value());
    ASSERT_TRUE(hp.remove(pfn_t{0x100}, [](const seg_map::segment &) {}).has_value());
    ASSERT_TRUE(hp.remove(pfn_t{0x200}, [](const seg_map::segment &) {}).has_value());
    EXPECT_EQ(hp.read()->size(), 0u);
  }
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
