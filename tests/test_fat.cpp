// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/fs/fat.hpp>

#include "fat_test_images.hpp"

#include <reloco/array.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo::fs;
using structo::hw::block_device_ref;
using reloco::error;
using reloco::span;

struct ram_disk {
  std::vector<std::byte> mem;
  ram_disk() = default;
  ram_disk(std::size_t size, const test_images::image_chunk *chunks, std::size_t n) : mem(size) {
    for (std::size_t i = 0; i < n; ++i)
      std::memcpy(mem.data() + chunks[i].offset, chunks[i].data, chunks[i].size);
  }
};

} // namespace

template <> struct structo::hw::block_device_traits<ram_disk> {
  static std::size_t block_size(ram_disk &) noexcept { return 512; }
  static std::uint64_t block_count(ram_disk &d) noexcept { return d.mem.size() / 512; }
  static reloco::result<void> try_read_blocks(ram_disk &d, std::uint64_t lba, span<std::byte> dst) noexcept {
    std::memcpy(dst.data(), d.mem.data() + lba * 512, dst.size());
    return {};
  }
};

namespace {

struct variant {
  const char *name;
  fat_type type;
  ram_disk disk;
};

std::vector<std::byte> kernel_bytes() {
  std::vector<std::byte> v(5000);
  for (std::size_t i = 0; i < v.size(); ++i)
    v[i] = static_cast<std::byte>((i * 7 + i / 251) & 255);
  return v;
}

std::vector<std::byte> frag_bytes() {
  std::vector<std::byte> v(3000);
  for (std::size_t i = 0; i < v.size(); ++i)
    v[i] = static_cast<std::byte>((i * 13 + 5) & 255);
  return v;
}

std::string str(const std::vector<std::byte> &v, std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i)
    s.push_back(static_cast<char>(v[i]));
  return s;
}

// Mounted volume plus the filesystem_ref bound to it.
fat_filesystem mount_or_die(block_device_ref dev, span<std::byte> scratch) {
  auto r = fat_filesystem::try_mount(dev, scratch);
  if (!r)
    std::abort(); // fixtures only mount known-good images
  return *r;
}

struct mounted {
  block_device_ref dev;
  std::vector<std::byte> scratch = std::vector<std::byte>(4096);
  fat_filesystem fat;
  filesystem_ref fs;

  explicit mounted(ram_disk &d)
      : dev(d), fat(mount_or_die(dev, span<std::byte>(scratch))), fs(fat) {}
};

class FatTest : public ::testing::Test {
protected:
  void SetUp() override {
    using namespace test_images;
    vars.push_back({"fat12", fat_type::fat12, ram_disk(fat12_image_size, fat12_image, std::size(fat12_image))});
    vars.push_back({"fat16", fat_type::fat16, ram_disk(fat16_image_size, fat16_image, std::size(fat16_image))});
    vars.push_back({"fat32", fat_type::fat32, ram_disk(fat32_image_size, fat32_image, std::size(fat32_image))});
  }

  std::vector<variant> vars;

  template <typename F> void for_each(F &&f) {
    for (auto &v : vars) {
      SCOPED_TRACE(v.name);
      mounted m(v.disk);
      f(m, v);
    }
  }
};

std::vector<std::string> list(filesystem_ref fs, reloco::string_view path) {
  std::vector<std::string> names;
  auto d = fs.try_open_dir(path);
  EXPECT_TRUE(d);
  if (!d)
    return names;
  auto it = d->entries();
  for (auto e : it) {
    EXPECT_TRUE(e);
    if (!e)
      break;
    names.emplace_back(e->name_view().data(), e->name_view().size());
  }
  return names;
}

bool has(const std::vector<std::string> &v, const std::string &s) {
  for (auto &x : v)
    if (x == s)
      return true;
  return false;
}

TEST_F(FatTest, MountDetectsVariant) {
  for (auto &v : vars) {
    SCOPED_TRACE(v.name);
    block_device_ref dev(v.disk);
    std::vector<std::byte> scratch(4096);
    auto fat = fat_filesystem::try_mount(dev, span<std::byte>(scratch));
    ASSERT_TRUE(fat);
    EXPECT_EQ(fat->type(), v.type);
    EXPECT_EQ(fat->cluster_size(), 512u);
    EXPECT_EQ(fat->open_count(), 0u);
  }
}

TEST_F(FatTest, MountRejectsNonFat) {
  ram_disk blank;
  blank.mem.resize(1 << 20);
  block_device_ref dev(blank);
  std::vector<std::byte> scratch(4096);
  EXPECT_EQ(fat_filesystem::try_mount(dev, span<std::byte>(scratch)).error(), error::invalid_argument);

  ram_disk bad = vars[1].disk;
  bad.mem[510] = std::byte{0};
  block_device_ref dev2(bad);
  EXPECT_EQ(fat_filesystem::try_mount(dev2, span<std::byte>(scratch)).error(), error::invalid_argument);

  ram_disk trunc = vars[1].disk;
  trunc.mem.resize(1 << 20); // smaller than the volume the BPB describes
  block_device_ref dev3(trunc);
  EXPECT_EQ(fat_filesystem::try_mount(dev3, span<std::byte>(scratch)).error(), error::invalid_argument);

  std::vector<std::byte> tiny(100);
  EXPECT_FALSE(fat_filesystem::try_mount(dev, span<std::byte>(tiny)));
}

TEST_F(FatTest, ReadsShortNameFileCaseInsensitively) {
  for_each([](mounted &m, variant &) {
    std::vector<std::byte> buf(64);
    for (const char *p : {"/HELLO.TXT", "/hello.txt", "hello.txt", "//Hello.Txt/"}) {
      if (std::string(p).back() == '/')
        continue;
      auto n = m.fs.try_read_file(p, span<std::byte>(buf));
      ASSERT_TRUE(n) << p;
      EXPECT_EQ(str(buf, *n), "hello fat");
    }
  });
}

TEST_F(FatTest, ReadsMultiClusterLongNameFile) {
  const auto want = kernel_bytes();
  for_each([&](mounted &m, variant &) {
    std::vector<std::byte> buf(6000);
    auto n = m.fs.try_read_file("/boot/Kernel-Image.bin", span<std::byte>(buf));
    ASSERT_TRUE(n);
    ASSERT_EQ(*n, want.size());
    EXPECT_TRUE(std::equal(want.begin(), want.end(), buf.begin()));
    EXPECT_TRUE(m.fs.try_read_file("/BOOT/kernel-image.BIN", span<std::byte>(buf)));
  });
}

TEST_F(FatTest, ReadsFragmentedFile) {
  const auto want = frag_bytes();
  for_each([&](mounted &m, variant &) {
    std::vector<std::byte> buf(4000);
    auto n = m.fs.try_read_file("/frag.bin", span<std::byte>(buf));
    ASSERT_TRUE(n);
    ASSERT_EQ(*n, want.size());
    EXPECT_TRUE(std::equal(want.begin(), want.end(), buf.begin()));
  });
}

TEST_F(FatTest, PositionalAndCursorReadsAcrossClusters) {
  const auto want = kernel_bytes();
  for_each([&](mounted &m, variant &) {
    auto f = m.fs.try_open("/boot/Kernel-Image.bin");
    ASSERT_TRUE(f);
    std::vector<std::byte> buf(700);
    auto n = f->try_read(1000, span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_EQ(*n, 700u);
    EXPECT_TRUE(std::equal(buf.begin(), buf.end(), want.begin() + 1000));

    // Backwards positional read after a forward one exercises the chain hint reset.
    n = f->try_read(10, span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_TRUE(std::equal(buf.begin(), buf.begin() + static_cast<long>(*n), want.begin() + 10));

    std::vector<std::byte> all;
    for (;;) {
      auto r = f->try_read(span<std::byte>(buf));
      ASSERT_TRUE(r);
      if (*r == 0)
        break;
      all.insert(all.end(), buf.begin(), buf.begin() + static_cast<long>(*r));
    }
    EXPECT_EQ(all, want);

    n = f->try_read(5000, span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_EQ(*n, 0u);
    n = f->try_read(4990, span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_EQ(*n, 10u);
  });
}

TEST_F(FatTest, UnicodeLongName) {
  for_each([](mounted &m, variant &) {
    std::vector<std::byte> buf(16);
    auto n = m.fs.try_read_file("/zażółć gęślą.txt", span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_EQ(str(buf, *n), "unicode");
    EXPECT_TRUE(has(list(m.fs, "/"), "zażółć gęślą.txt"));
  });
}

TEST_F(FatTest, RootListing) {
  for_each([](mounted &m, variant &) {
    auto names = list(m.fs, "/");
    for (const char *n : {"HELLO.TXT", "boot", "many", "frag.bin", "small.txt", "hole2.bin"})
      EXPECT_TRUE(has(names, n)) << n;
    EXPECT_FALSE(has(names, "hole.bin")); // deleted
    EXPECT_FALSE(has(names, "TESTVOL"));  // volume label is not an entry
  });
}

TEST_F(FatTest, SubdirectoryListingTypesAndSizes) {
  for_each([](mounted &m, variant &) {
    auto d = m.fs.try_open_dir("/boot");
    ASSERT_TRUE(d);
    bool saw_kernel = false, saw_sub = false;
    auto it = d->entries();
    for (auto e : it) {
      ASSERT_TRUE(e);
      EXPECT_NE(e->name_view(), ".");
      EXPECT_NE(e->name_view(), "..");
      if (e->name_view() == "Kernel-Image.bin") {
        saw_kernel = true;
        EXPECT_EQ(e->type, file_type::regular);
        EXPECT_EQ(e->size, 5000u);
      } else if (e->name_view() == "sub") {
        saw_sub = true;
        EXPECT_EQ(e->type, file_type::directory);
      }
    }
    EXPECT_TRUE(saw_kernel);
    EXPECT_TRUE(saw_sub);
  });
}

TEST_F(FatTest, DirectorySpanningManyClusters) {
  for_each([](mounted &m, variant &) {
    auto names = list(m.fs, "/many");
    EXPECT_EQ(names.size(), 40u);
    for (int i = 1; i <= 40; ++i)
      EXPECT_TRUE(has(names, "file" + std::to_string(i) + ".txt")) << i;
    std::vector<std::byte> buf(4);
    auto n = m.fs.try_read_file("/many/file40.txt", span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_EQ(str(buf, *n), "x");
  });
}

TEST_F(FatTest, StatAndNavigation) {
  for_each([](mounted &m, variant &) {
    auto st = m.fs.try_stat("/boot/Kernel-Image.bin");
    ASSERT_TRUE(st);
    EXPECT_TRUE(st->is_regular());
    EXPECT_EQ(st->size, 5000u);
    auto root = m.fs.try_stat("/");
    ASSERT_TRUE(root);
    EXPECT_TRUE(root->is_directory());
    auto boot = m.fs.try_stat("/boot");
    ASSERT_TRUE(boot);
    EXPECT_TRUE(boot->is_directory());

    EXPECT_EQ(m.fs.try_stat("/missing").error(), error::not_found);
    EXPECT_EQ(m.fs.try_stat("/boot/missing/x").error(), error::not_found);
    EXPECT_EQ(m.fs.try_stat("/HELLO.TXT/x").error(), error::invalid_argument);

    // "." and ".." are real entries in subdirectories; ".." at the root stays at the root.
    EXPECT_TRUE(m.fs.exists("/boot/sub/../Kernel-Image.bin"));
    EXPECT_TRUE(m.fs.exists("/boot/../HELLO.TXT"));
    EXPECT_TRUE(m.fs.exists("/../HELLO.TXT"));
    EXPECT_TRUE(m.fs.exists("/boot/./sub/deep.txt"));
  });
}

TEST_F(FatTest, WrongObjectKind) {
  for_each([](mounted &m, variant &) {
    EXPECT_EQ(m.fs.try_open("/boot").error(), error::invalid_argument);
    EXPECT_EQ(m.fs.try_open_dir("/HELLO.TXT").error(), error::invalid_argument);
    std::vector<std::byte> buf(8);
    EXPECT_EQ(m.fs.try_read_file("/boot", span<std::byte>(buf)).error(), error::invalid_argument);
  });
}

TEST_F(FatTest, IsReadOnly) {
  for_each([](mounted &m, variant &) {
    EXPECT_TRUE(m.fs.is_read_only());
    EXPECT_EQ(m.fs.try_open("/HELLO.TXT", open_flags::write).error(), error::permission_denied);
    EXPECT_EQ(m.fs.try_mkdir("/x").error(), error::permission_denied);
    EXPECT_EQ(m.fs.try_remove("/HELLO.TXT").error(), error::permission_denied);
    EXPECT_EQ(m.fs.try_write_file("/HELLO.TXT", span<const std::byte>()).error(), error::permission_denied);
  });
}

TEST_F(FatTest, OpenObjectTableLimit) {
  for_each([](mounted &m, variant &) {
    std::vector<directory> dirs;
    for (std::size_t i = 0; i < fat_filesystem::max_open_objects; ++i) {
      auto d = m.fs.try_open_dir("/boot");
      ASSERT_TRUE(d) << i;
      dirs.push_back(std::move(*d));
    }
    EXPECT_EQ(m.fat.open_count(), fat_filesystem::max_open_objects);
    EXPECT_EQ(m.fs.try_open_dir("/boot").error(), error::busy);
    EXPECT_EQ(m.fs.try_open("/HELLO.TXT").error(), error::busy);
    dirs.pop_back();
    EXPECT_TRUE(m.fs.try_open("/HELLO.TXT"));
    dirs.clear();
    EXPECT_EQ(m.fat.open_count(), 0u);
  });
}

TEST_F(FatTest, CorruptChainReportsIoError) {
  ram_disk disk = vars[1].disk;
  mounted m(disk);
  auto st = m.fs.try_stat("/boot/Kernel-Image.bin");
  ASSERT_TRUE(st);
  // Zero the start of the disk (reserved area and FAT): chains become "free" links, which must not be followed.
  for (std::size_t i = 512; i < 512 * 80; ++i)
    disk.mem[i] = std::byte{0};
  std::vector<std::byte> buf(6000);
  auto n = m.fs.try_read_file("/boot/Kernel-Image.bin", span<std::byte>(buf));
  EXPECT_FALSE(n);
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
