// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>

#include <structo/fs/ext4.hpp>

#include "ext4_test_images.hpp"

#include <reloco/array.hpp>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo::fs;
using reloco::error;
using reloco::span;
using structo::hw::block_device_ref;

struct ram_disk {
  std::vector<std::byte> mem;
  ram_disk() = default;
  ram_disk(const test_images::image_chunk *chunks, std::size_t n) : mem(test_images::ext4_image_size) {
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
  std::uint32_t block_size;
  bool extents;
  std::size_t kernel_size;
  ram_disk disk;
};

std::vector<std::byte> kernel_bytes(std::size_t n) {
  std::vector<std::byte> v(n);
  for (std::size_t i = 0; i < v.size(); ++i)
    v[i] = static_cast<std::byte>((i * 7 + i / 251) & 255);
  return v;
}

// /sparse.bin: 1 KiB chunks of 'A'+i at every 3 KiB, zeros (holes) elsewhere, 37364 bytes.
std::vector<std::byte> sparse_bytes() {
  std::vector<std::byte> v(12 * 3072 + 500);
  for (std::size_t i = 0; i < v.size(); ++i)
    v[i] = (i % 3072 < 1024 && i / 3072 < 12) ? static_cast<std::byte>('A' + i / 3072) : std::byte{0};
  return v;
}

std::string str(const std::vector<std::byte> &v, std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i)
    s.push_back(static_cast<char>(v[i]));
  return s;
}

ext4_filesystem mount_or_die(block_device_ref dev, span<std::byte> scratch) {
  auto r = ext4_filesystem::try_mount(dev, scratch);
  if (!r)
    std::abort(); // fixtures only mount known-good images
  return *r;
}

// Mounted volume plus the filesystem_ref bound to it.
struct mounted {
  block_device_ref dev;
  std::vector<std::byte> scratch = std::vector<std::byte>(4096);
  ext4_filesystem ext;
  filesystem_ref fs;

  explicit mounted(ram_disk &d) : dev(d), ext(mount_or_die(dev, span<std::byte>(scratch))), fs(ext) {}
};

class Ext4Test : public ::testing::Test {
protected:
  void SetUp() override {
    using namespace test_images;
    vars.push_back({"a: 1k extents+journal", 1024, true, 40000, ram_disk(ext4_a, std::size(ext4_a))});
    vars.push_back({"b: 4k extents", 4096, true, 40000, ram_disk(ext4_b, std::size(ext4_b))});
    vars.push_back({"c: 1k block maps", 1024, false, 300000, ram_disk(ext4_c, std::size(ext4_c))});
    vars.push_back({"d: 1k htree", 1024, true, 40000, ram_disk(ext4_d, std::size(ext4_d))});
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

std::vector<std::string> list(filesystem_ref fs, reloco::string_view path, std::vector<file_type> *types = nullptr) {
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
    if (types)
      types->push_back(e->type);
  }
  return names;
}

bool has(const std::vector<std::string> &v, const std::string &s) {
  for (auto &x : v)
    if (x == s)
      return true;
  return false;
}

std::string read_str(filesystem_ref fs, reloco::string_view path) {
  std::vector<std::byte> buf(256);
  auto n = fs.try_read_file(path, span<std::byte>(buf));
  EXPECT_TRUE(n) << std::string(path.data(), path.size());
  return n ? str(buf, *n) : std::string();
}

TEST_F(Ext4Test, MountReportsGeometry) {
  for (auto &v : vars) {
    SCOPED_TRACE(v.name);
    block_device_ref dev(v.disk);
    std::vector<std::byte> scratch(4096);
    auto ext = ext4_filesystem::try_mount(dev, span<std::byte>(scratch));
    ASSERT_TRUE(ext);
    EXPECT_EQ(ext->block_size(), v.block_size);
    EXPECT_EQ(ext->uses_extents(), v.extents);
    EXPECT_GE(ext->group_count(), 1u);
    EXPECT_EQ(ext->open_count(), 0u);
  }
}

TEST_F(Ext4Test, MountRejectsInvalidVolumes) {
  std::vector<std::byte> scratch(4096);

  ram_disk blank;
  blank.mem.resize(1 << 20);
  block_device_ref dev(blank);
  EXPECT_EQ(ext4_filesystem::try_mount(dev, span<std::byte>(scratch)).error(), error::invalid_argument);

  ram_disk bad = vars[0].disk;
  bad.mem[1024 + 56] = std::byte{0};
  block_device_ref dev2(bad);
  EXPECT_EQ(ext4_filesystem::try_mount(dev2, span<std::byte>(scratch)).error(), error::invalid_argument);

  ram_disk trunc = vars[0].disk;
  trunc.mem.resize(1 << 20); // smaller than the filesystem the superblock describes
  block_device_ref dev3(trunc);
  EXPECT_EQ(ext4_filesystem::try_mount(dev3, span<std::byte>(scratch)).error(), error::invalid_argument);

  std::vector<std::byte> tiny(100);
  block_device_ref dev4(vars[0].disk);
  EXPECT_FALSE(ext4_filesystem::try_mount(dev4, span<std::byte>(tiny)));
}

TEST_F(Ext4Test, MountRefusesUnsupportedFeaturesAndDirtyJournal) {
  std::vector<std::byte> scratch(4096);
  auto with_incompat = [&](std::uint8_t set_low, std::uint8_t set_high) {
    ram_disk d = vars[0].disk;
    d.mem[1024 + 96] = static_cast<std::byte>(static_cast<std::uint8_t>(d.mem[1024 + 96]) | set_low);
    d.mem[1024 + 97] = static_cast<std::byte>(static_cast<std::uint8_t>(d.mem[1024 + 97]) | set_high);
    block_device_ref dev(d);
    return ext4_filesystem::try_mount(dev, span<std::byte>(scratch)).error();
  };
  EXPECT_EQ(with_incompat(0x04, 0), error::invalid_state);         // needs_recovery
  EXPECT_EQ(with_incompat(0x10, 0), error::unsupported_operation); // meta_bg
  EXPECT_EQ(with_incompat(0, 0x80), error::unsupported_operation); // inline_data
}

TEST_F(Ext4Test, ReadsSmallFilesAndPaths) {
  for_each([](mounted &m, variant &) {
    EXPECT_EQ(read_str(m.fs, "/hello.txt"), "hello ext4");
    EXPECT_EQ(read_str(m.fs, "hello.txt"), "hello ext4");
    EXPECT_EQ(read_str(m.fs, "//boot///deep.txt"), "deep content");
    EXPECT_EQ(read_str(m.fs, "/boot/./deep.txt"), "deep content");
    EXPECT_EQ(read_str(m.fs, "/boot/../hello.txt"), "hello ext4");
    EXPECT_EQ(read_str(m.fs, "/../hello.txt"), "hello ext4"); // root's ".." is the root
    EXPECT_EQ(read_str(m.fs, "/zażółć.txt"), "unicode");
    EXPECT_FALSE(m.fs.exists("/HELLO.TXT")); // case-sensitive
  });
}

TEST_F(Ext4Test, ReadsLargeFile) {
  for_each([](mounted &m, variant &v) {
    const auto want = kernel_bytes(v.kernel_size);
    std::vector<std::byte> buf(v.kernel_size + 100);
    auto n = m.fs.try_read_file("/boot/Kernel-Image.bin", span<std::byte>(buf));
    ASSERT_TRUE(n);
    ASSERT_EQ(*n, want.size());
    EXPECT_TRUE(std::equal(want.begin(), want.end(), buf.begin()));
  });
}

TEST_F(Ext4Test, SparseFileHolesReadAsZeros) {
  const auto want = sparse_bytes();
  for_each([&](mounted &m, variant &) {
    std::vector<std::byte> buf(want.size() + 10);
    auto n = m.fs.try_read_file("/sparse.bin", span<std::byte>(buf));
    ASSERT_TRUE(n);
    ASSERT_EQ(*n, want.size());
    EXPECT_TRUE(std::equal(want.begin(), want.end(), buf.begin()));
  });
}

TEST_F(Ext4Test, PositionalAndCursorReads) {
  for_each([](mounted &m, variant &v) {
    const auto want = kernel_bytes(v.kernel_size);
    auto f = m.fs.try_open("/boot/Kernel-Image.bin");
    ASSERT_TRUE(f);
    std::vector<std::byte> buf(3000);
    auto n = f->try_read(1500, span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_EQ(*n, 3000u);
    EXPECT_TRUE(std::equal(buf.begin(), buf.end(), want.begin() + 1500));

    n = f->try_read(7, span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_TRUE(std::equal(buf.begin(), buf.begin() + static_cast<long>(*n), want.begin() + 7));

    std::vector<std::byte> all;
    for (;;) {
      auto r = f->try_read(span<std::byte>(buf));
      ASSERT_TRUE(r);
      if (*r == 0)
        break;
      all.insert(all.end(), buf.begin(), buf.begin() + static_cast<long>(*r));
    }
    EXPECT_EQ(all, want);

    n = f->try_read(v.kernel_size, span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_EQ(*n, 0u);
    n = f->try_read(v.kernel_size - 10, span<std::byte>(buf));
    ASSERT_TRUE(n);
    EXPECT_EQ(*n, 10u);
  });
}

TEST_F(Ext4Test, RootListingTypes) {
  for_each([](mounted &m, variant &) {
    std::vector<file_type> types;
    auto names = list(m.fs, "/", &types);
    for (const char *n : {"hello.txt", "boot", "many", "sparse.bin", "lost+found", "rel-link", "dirlink"})
      EXPECT_TRUE(has(names, n)) << n;
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (names[i] == "boot" || names[i] == "many" || names[i] == "lost+found")
        EXPECT_EQ(types[i], file_type::directory) << names[i];
      if (names[i] == "hello.txt")
        EXPECT_EQ(types[i], file_type::regular);
      if (names[i] == "rel-link")
        EXPECT_EQ(types[i], file_type::symlink);
    }
    EXPECT_FALSE(has(names, "."));
    EXPECT_FALSE(has(names, ".."));
  });
}

TEST_F(Ext4Test, LargeDirectory) {
  for_each([](mounted &m, variant &) {
    auto names = list(m.fs, "/many");
    EXPECT_EQ(names.size(), 300u);
    for (int i = 1; i <= 300; i += 7)
      EXPECT_TRUE(has(names, "file-with-a-longish-name-" + std::to_string(i) + ".txt")) << i;
    EXPECT_EQ(read_str(m.fs, "/many/file-with-a-longish-name-300.txt"), "x");
    EXPECT_EQ(read_str(m.fs, "/many/file-with-a-longish-name-1.txt"), "x");
    EXPECT_EQ(m.fs.try_stat("/many/file-with-a-longish-name-301.txt").error(), error::not_found);
  });
}

TEST_F(Ext4Test, DirectoryEntrySizes) {
  for_each([](mounted &m, variant &v) {
    auto d = m.fs.try_open_dir("/boot");
    ASSERT_TRUE(d);
    bool saw = false;
    auto it = d->entries();
    for (auto e : it) {
      ASSERT_TRUE(e);
      if (e->name_view() == "Kernel-Image.bin") {
        saw = true;
        EXPECT_EQ(e->size, v.kernel_size);
        EXPECT_EQ(e->type, file_type::regular);
      }
    }
    EXPECT_TRUE(saw);
  });
}

TEST_F(Ext4Test, SymlinksAreFollowed) {
  for_each([](mounted &m, variant &) {
    EXPECT_EQ(read_str(m.fs, "/rel-link"), "hello ext4");
    EXPECT_EQ(read_str(m.fs, "/abs-link"), "deep content");
    EXPECT_EQ(read_str(m.fs, "/boot/up-link"), "hello ext4");
    EXPECT_EQ(read_str(m.fs, "/long-link"), "deep content"); // slow symlink (target stored in a data block)
    EXPECT_EQ(read_str(m.fs, "/dirlink/deep.txt"), "deep content");
    EXPECT_TRUE(list(m.fs, "/dirlink").size() >= 2);
    auto st = m.fs.try_stat("/rel-link");
    ASSERT_TRUE(st);
    EXPECT_TRUE(st->is_regular());
    EXPECT_EQ(st->size, 10u);
    auto dst = m.fs.try_stat("/dirlink");
    ASSERT_TRUE(dst);
    EXPECT_TRUE(dst->is_directory());
  });
}

TEST_F(Ext4Test, SymlinkLoopIsRejected) {
  for_each([](mounted &m, variant &) {
    EXPECT_EQ(m.fs.try_stat("/loop-a").error(), error::invalid_argument);
    EXPECT_EQ(m.fs.try_open("/loop-b").error(), error::invalid_argument);
    EXPECT_EQ(m.ext.open_count(), 0u);
  });
}

TEST_F(Ext4Test, StatAndErrors) {
  for_each([](mounted &m, variant &v) {
    auto st = m.fs.try_stat("/boot/Kernel-Image.bin");
    ASSERT_TRUE(st);
    EXPECT_EQ(st->size, v.kernel_size);
    auto root = m.fs.try_stat("/");
    ASSERT_TRUE(root);
    EXPECT_TRUE(root->is_directory());
    EXPECT_EQ(m.fs.try_stat("/missing").error(), error::not_found);
    EXPECT_EQ(m.fs.try_stat("/boot/missing/x").error(), error::not_found);
    EXPECT_EQ(m.fs.try_stat("/hello.txt/x").error(), error::invalid_argument);
    EXPECT_EQ(m.fs.try_open("/boot").error(), error::invalid_argument);
    EXPECT_EQ(m.fs.try_open_dir("/hello.txt").error(), error::invalid_argument);
    std::vector<std::byte> buf(8);
    EXPECT_EQ(m.fs.try_read_file("/boot", span<std::byte>(buf)).error(), error::invalid_argument);
    EXPECT_EQ(m.fs.try_read_file("/hello.txt", span<std::byte>(buf)).error(), error::capacity_exceeded);
  });
}

TEST_F(Ext4Test, IsReadOnly) {
  for_each([](mounted &m, variant &) {
    EXPECT_TRUE(m.fs.is_read_only());
    EXPECT_EQ(m.fs.try_open("/hello.txt", open_flags::write).error(), error::permission_denied);
    EXPECT_EQ(m.fs.try_mkdir("/x").error(), error::permission_denied);
    EXPECT_EQ(m.fs.try_remove("/hello.txt").error(), error::permission_denied);
  });
}

TEST_F(Ext4Test, OpenObjectTableLimit) {
  for_each([](mounted &m, variant &) {
    std::vector<directory> dirs;
    for (std::size_t i = 0; i < ext4_filesystem::max_open_objects; ++i) {
      auto d = m.fs.try_open_dir("/boot");
      ASSERT_TRUE(d) << i;
      dirs.push_back(std::move(*d));
    }
    EXPECT_EQ(m.fs.try_open_dir("/boot").error(), error::busy);
    EXPECT_EQ(m.fs.try_open("/hello.txt").error(), error::busy);
    dirs.pop_back();
    EXPECT_TRUE(m.fs.try_open("/hello.txt"));
    dirs.clear();
    EXPECT_EQ(m.ext.open_count(), 0u);
  });
}

TEST_F(Ext4Test, CorruptMetadataReportsIoError) {
  // After mounting, overwrite everything past the group descriptors: inodes and directories become garbage.
  ram_disk disk = vars[0].disk;
  mounted m(disk);
  std::memset(disk.mem.data() + 4096, 0xFF, disk.mem.size() - 4096);
  std::vector<std::byte> buf(64);
  auto n = m.fs.try_read_file("/hello.txt", span<std::byte>(buf));
  EXPECT_FALSE(n);
  EXPECT_FALSE(m.fs.try_stat("/boot/deep.txt"));
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
