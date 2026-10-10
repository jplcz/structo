// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#include <gtest/gtest.h>
#include <structo/fs/filesystem_ref.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Test fixtures index raw buffers freely; bounds are checked by the assertions.
RELOCO_BEGIN_UNSAFE_BUFFER_USAGE

namespace {

using namespace structo::fs;
using reloco::error;
using reloco::span;
using reloco::string_view;

struct node {
  file_type type = file_type::regular;
  std::vector<std::byte> data;
};

struct open_obj {
  std::string path;
  bool dir = false;
  std::size_t dir_pos = 0;
  open_flags flags = open_flags::none;
};

// Flat in-memory filesystem: full path -> node. Directories are explicit nodes.
struct ram_fs {
  std::map<std::string, node> nodes;
  std::map<fs_handle, open_obj> open;
  fs_handle next = 1;
  bool read_only = false;
  int syncs = 0;

  ram_fs() { nodes["/"] = node{file_type::directory, {}}; }

  [[nodiscard]] std::size_t live() const { return open.size(); }
};

// Same backend without any optional operation.
struct ro_fs {
  ram_fs inner;
};

// Owns the bytes and converts implicitly to a span for the duration of a full expression.
struct blob {
  std::vector<std::byte> v;
  operator span<const std::byte>() const noexcept { return span<const std::byte>(v.data(), v.size()); }
};

blob bytes(string_view s) {
  blob b;
  for (char c : s)
    b.v.push_back(static_cast<std::byte>(c));
  return b;
}

template <typename T> T ok(reloco::result<T> r) {
  EXPECT_TRUE(r.has_value());
  return r.has_value() ? *r : T{};
}

std::string str(const std::vector<std::byte> &v, std::size_t n) {
  std::string s;
  for (std::size_t i = 0; i < n; ++i)
    s.push_back(static_cast<char>(v[i]));
  return s;
}

} // namespace

template <> struct structo::fs::filesystem_traits<ram_fs> {
  static reloco::result<fs_handle> try_open(ram_fs &f, string_view path, open_flags fl) noexcept {
    std::string p(path.data(), path.size());
    auto it = f.nodes.find(p);
    if (it == f.nodes.end()) {
      if (!has_flag(fl, open_flags::create))
        return reloco::unexpected(error::not_found);
      it = f.nodes.emplace(p, node{}).first;
    } else {
      if (has_flag(fl, open_flags::create | open_flags::exclusive))
        return reloco::unexpected(error::already_exists);
      if (it->second.type != file_type::regular)
        return reloco::unexpected(error::invalid_argument);
    }
    if (has_flag(fl, open_flags::truncate))
      it->second.data.clear();
    const fs_handle h = f.next++;
    f.open[h] = open_obj{p, false, 0, fl};
    return h;
  }
  static reloco::result<fs_handle> try_open_dir(ram_fs &f, string_view path) noexcept {
    std::string p(path.data(), path.size());
    auto it = f.nodes.find(p);
    if (it == f.nodes.end())
      return reloco::unexpected(error::not_found);
    if (it->second.type != file_type::directory)
      return reloco::unexpected(error::invalid_argument);
    const fs_handle h = f.next++;
    f.open[h] = open_obj{p, true, 0, open_flags::read};
    return h;
  }
  static reloco::result<void> try_close(ram_fs &f, fs_handle h) noexcept {
    return f.open.erase(h) ? reloco::result<void>{} : reloco::result<void>(reloco::unexpected(error::invalid_argument));
  }
  static reloco::result<std::size_t> try_read(ram_fs &f, fs_handle h, std::uint64_t off, span<std::byte> dst) noexcept {
    auto &d = f.nodes[f.open[h].path].data;
    if (off >= d.size())
      return std::size_t{0};
    // Short reads on purpose (max 3 bytes) so callers must loop.
    std::size_t n = std::min<std::size_t>({dst.size(), d.size() - static_cast<std::size_t>(off), 3});
    for (std::size_t i = 0; i < n; ++i)
      dst[i] = d[static_cast<std::size_t>(off) + i];
    return n;
  }
  static reloco::result<bool> try_read_dir(ram_fs &f, fs_handle h, dir_entry &out) noexcept {
    auto &o = f.open[h];
    std::string prefix = o.path == "/" ? "/" : o.path + "/";
    std::size_t idx = 0;
    for (auto &[k, v] : f.nodes) {
      if (k == o.path || k.compare(0, prefix.size(), prefix) != 0 || k.find('/', prefix.size()) != std::string::npos)
        continue;
      if (idx++ < o.dir_pos)
        continue;
      ++o.dir_pos;
      out.set_name(string_view(k.data() + prefix.size(), k.size() - prefix.size()));
      out.type = v.type;
      out.size = v.data.size();
      return true;
    }
    return false;
  }
  static reloco::result<file_info> try_stat(ram_fs &f, string_view path) noexcept {
    auto it = f.nodes.find(std::string(path.data(), path.size()));
    if (it == f.nodes.end())
      return reloco::unexpected(error::not_found);
    return file_info{it->second.type, it->second.data.size()};
  }
  static reloco::result<file_info> try_fstat(ram_fs &f, fs_handle h) noexcept {
    auto &n = f.nodes[f.open[h].path];
    return file_info{n.type, n.data.size()};
  }

  static reloco::result<std::size_t> try_write(ram_fs &f, fs_handle h, std::uint64_t off,
                                               span<const std::byte> src) noexcept {
    auto &o = f.open[h];
    auto &d = f.nodes[o.path].data;
    std::size_t pos = has_flag(o.flags, open_flags::append) ? d.size() : static_cast<std::size_t>(off);
    const std::size_t n = std::min<std::size_t>(src.size(), 4); // short writes
    if (d.size() < pos + n)
      d.resize(pos + n);
    for (std::size_t i = 0; i < n; ++i)
      d[pos + i] = src[i];
    return n;
  }
  static reloco::result<void> try_truncate(ram_fs &f, fs_handle h, std::uint64_t size) noexcept {
    f.nodes[f.open[h].path].data.resize(static_cast<std::size_t>(size));
    return {};
  }
  static reloco::result<void> try_sync(ram_fs &f, fs_handle) noexcept {
    ++f.syncs;
    return {};
  }
  static reloco::result<void> try_mkdir(ram_fs &f, string_view path) noexcept {
    auto r = f.nodes.emplace(std::string(path.data(), path.size()), node{file_type::directory, {}});
    if (!r.second)
      return reloco::unexpected(error::already_exists);
    return {};
  }
  static reloco::result<void> try_remove(ram_fs &f, string_view path) noexcept {
    return f.nodes.erase(std::string(path.data(), path.size()))
               ? reloco::result<void>{}
               : reloco::result<void>(reloco::unexpected(error::not_found));
  }
  static reloco::result<void> try_rename(ram_fs &f, string_view from, string_view to) noexcept {
    auto it = f.nodes.find(std::string(from.data(), from.size()));
    if (it == f.nodes.end())
      return reloco::unexpected(error::not_found);
    f.nodes[std::string(to.data(), to.size())] = std::move(it->second);
    f.nodes.erase(it);
    return {};
  }
  static reloco::result<filesystem_info> try_statfs(ram_fs &f) noexcept {
    return filesystem_info{512, 100, 100 - f.nodes.size()};
  }
  static bool is_read_only(ram_fs &f) noexcept { return f.read_only; }
};

// Read-only backend: mandatory operations only.
template <> struct structo::fs::filesystem_traits<ro_fs> {
  using base = filesystem_traits<ram_fs>;
  static reloco::result<fs_handle> try_open(ro_fs &f, string_view p, open_flags fl) noexcept {
    return base::try_open(f.inner, p, fl);
  }
  static reloco::result<fs_handle> try_open_dir(ro_fs &f, string_view p) noexcept {
    return base::try_open_dir(f.inner, p);
  }
  static reloco::result<void> try_close(ro_fs &f, fs_handle h) noexcept { return base::try_close(f.inner, h); }
  static reloco::result<std::size_t> try_read(ro_fs &f, fs_handle h, std::uint64_t o, span<std::byte> d) noexcept {
    return base::try_read(f.inner, h, o, d);
  }
  static reloco::result<bool> try_read_dir(ro_fs &f, fs_handle h, dir_entry &e) noexcept {
    return base::try_read_dir(f.inner, h, e);
  }
  static reloco::result<file_info> try_stat(ro_fs &f, string_view p) noexcept { return base::try_stat(f.inner, p); }
  static reloco::result<file_info> try_fstat(ro_fs &f, fs_handle h) noexcept { return base::try_fstat(f.inner, h); }
};

namespace {

class FilesystemRefTest : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_TRUE(fs.try_mkdir("/boot"));
    ASSERT_TRUE(fs.try_write_file("/boot/kernel", bytes("0123456789")));
    ASSERT_TRUE(fs.try_write_file("/boot/cfg", bytes("hello")));
    ASSERT_TRUE(fs.try_mkdir("/boot/sub"));
  }
  ram_fs backend;
  filesystem_ref fs{backend};
};

TEST_F(FilesystemRefTest, UnboundRefFailsGracefully) {
  filesystem_ref none;
  EXPECT_FALSE(none);
  EXPECT_TRUE(none.is_read_only());
  EXPECT_EQ(none.try_stat("/x").error(), error::unsupported_operation);
  EXPECT_EQ(none.try_open("/x").error(), error::unsupported_operation);
  EXPECT_FALSE(none.exists("/x"));
}

TEST_F(FilesystemRefTest, ReadFileHandlesShortReads) {
  std::vector<std::byte> buf(32);
  auto n = fs.try_read_file("/boot/kernel", span<std::byte>(buf));
  ASSERT_TRUE(n);
  EXPECT_EQ(*n, 10u);
  EXPECT_EQ(str(buf, *n), "0123456789");
}

TEST_F(FilesystemRefTest, ReadFileTooSmallReportsCapacity) {
  std::vector<std::byte> buf(4);
  EXPECT_EQ(fs.try_read_file("/boot/kernel", span<std::byte>(buf)).error(), error::capacity_exceeded);
  EXPECT_EQ(backend.live(), 0u);
}

TEST_F(FilesystemRefTest, ReadFileOnDirectoryRejected) {
  std::vector<std::byte> buf(4);
  EXPECT_EQ(fs.try_read_file("/boot/sub", span<std::byte>(buf)).error(), error::invalid_argument);
  EXPECT_EQ(backend.live(), 0u);
}

TEST_F(FilesystemRefTest, MissingFile) {
  EXPECT_EQ(fs.try_open("/nope").error(), error::not_found);
  EXPECT_FALSE(fs.exists("/nope"));
  EXPECT_TRUE(fs.exists("/boot/cfg"));
}

TEST_F(FilesystemRefTest, PathValidation) {
  EXPECT_EQ(fs.try_open("").error(), error::invalid_argument);
  const std::string longp(max_path_length + 1, 'a');
  EXPECT_EQ(fs.try_stat(string_view(longp.data(), longp.size())).error(), error::invalid_argument);
  EXPECT_EQ(fs.try_mkdir("").error(), error::invalid_argument);
}

TEST_F(FilesystemRefTest, FlagValidation) {
  EXPECT_EQ(fs.try_open("/boot/cfg", open_flags::none).error(), error::invalid_argument);
  EXPECT_EQ(fs.try_open("/boot/cfg", open_flags::read | open_flags::create).error(), error::invalid_argument);
  EXPECT_EQ(fs.try_open("/boot/cfg", open_flags::read | open_flags::truncate).error(), error::invalid_argument);
  EXPECT_EQ(fs.try_open("/boot/cfg", open_flags::read | open_flags::append).error(), error::invalid_argument);
  EXPECT_EQ(fs.try_open("/boot/cfg", open_flags::write | open_flags::create | open_flags::exclusive).error(),
            error::already_exists);
}

TEST_F(FilesystemRefTest, CursorAndPositionalReads) {
  auto f = fs.try_open("/boot/kernel");
  ASSERT_TRUE(f);
  std::vector<std::byte> b(4);
  auto n = f->try_read(2, span<std::byte>(b));
  ASSERT_TRUE(n);
  EXPECT_EQ(str(b, *n), "234");
  EXPECT_EQ(f->tell(), 0u);

  std::string all;
  for (;;) {
    auto r = f->try_read(span<std::byte>(b));
    ASSERT_TRUE(r);
    if (*r == 0)
      break;
    all += str(b, *r);
  }
  EXPECT_EQ(all, "0123456789");
  EXPECT_EQ(f->tell(), 10u);
  f->seek(8);
  auto r = f->try_read(span<std::byte>(b));
  ASSERT_TRUE(r);
  EXPECT_EQ(str(b, *r), "89");
  auto st = f->try_stat();
  ASSERT_TRUE(st);
  EXPECT_EQ(st->size, 10u);
  EXPECT_TRUE(st->is_regular());
}

TEST_F(FilesystemRefTest, ReadOnlyHandleCannotWrite) {
  auto f = fs.try_open("/boot/cfg", open_flags::read);
  ASSERT_TRUE(f);
  EXPECT_EQ(f->try_write(0, bytes("x")).error(), error::permission_denied);
  EXPECT_EQ(f->try_truncate(0).error(), error::permission_denied);
}

TEST_F(FilesystemRefTest, WriteOnlyHandleCannotRead) {
  auto f = fs.try_open("/boot/cfg", open_flags::write);
  ASSERT_TRUE(f);
  std::vector<std::byte> b(2);
  EXPECT_EQ(f->try_read(0, span<std::byte>(b)).error(), error::permission_denied);
}

TEST_F(FilesystemRefTest, WriteAtOffsetAndCursor) {
  auto f = fs.try_open("/boot/cfg", open_flags::read | open_flags::write);
  ASSERT_TRUE(f);
  auto w = f->try_write(1, bytes("EL"));
  ASSERT_TRUE(w);
  EXPECT_EQ(*w, 2u);
  auto c = bytes("!!");
  auto w2 = f->try_write(span<const std::byte>(c));
  ASSERT_TRUE(w2);
  EXPECT_EQ(f->tell(), 2u);
  ASSERT_TRUE(f->try_close());
  std::vector<std::byte> b(16);
  auto n = fs.try_read_file("/boot/cfg", span<std::byte>(b));
  ASSERT_TRUE(n);
  EXPECT_EQ(str(b, *n), "!!Llo");
}

TEST_F(FilesystemRefTest, AppendAndTruncate) {
  {
    auto f = fs.try_open("/boot/cfg", open_flags::write | open_flags::append);
    ASSERT_TRUE(f);
    ASSERT_TRUE(f->try_write(0, bytes("++")));
  }
  std::vector<std::byte> b(16);
  EXPECT_EQ(ok(fs.try_read_file("/boot/cfg", span<std::byte>(b))), 7u);
  {
    auto f = fs.try_open("/boot/cfg", open_flags::write);
    ASSERT_TRUE(f);
    ASSERT_TRUE(f->try_truncate(2));
  }
  auto n = fs.try_read_file("/boot/cfg", span<std::byte>(b));
  ASSERT_TRUE(n);
  EXPECT_EQ(str(b, *n), "he");
}

TEST_F(FilesystemRefTest, WriteFileTruncatesAndSyncs) {
  const int before = backend.syncs;
  ASSERT_TRUE(fs.try_write_file("/boot/cfg", bytes("a much longer body")));
  EXPECT_GT(backend.syncs, before);
  std::vector<std::byte> b(64);
  auto n = fs.try_read_file("/boot/cfg", span<std::byte>(b));
  ASSERT_TRUE(n);
  EXPECT_EQ(str(b, *n), "a much longer body");
  ASSERT_TRUE(fs.try_write_file("/boot/cfg", bytes("s")));
  EXPECT_EQ(ok(fs.try_read_file("/boot/cfg", span<std::byte>(b))), 1u);
}

TEST_F(FilesystemRefTest, MoveSemanticsAndCloseOnDestroy) {
  EXPECT_EQ(backend.live(), 0u);
  {
    auto r = fs.try_open("/boot/cfg");
    ASSERT_TRUE(r);
    file a = std::move(*r);
    EXPECT_TRUE(a);
    EXPECT_EQ(backend.live(), 1u);
    file b = std::move(a);
    EXPECT_FALSE(a);
    EXPECT_TRUE(b);
    EXPECT_EQ(backend.live(), 1u);
    std::vector<std::byte> buf(1);
    EXPECT_EQ(a.try_read(0, span<std::byte>(buf)).error(), error::invalid_state);

    file c;
    c = std::move(b);
    EXPECT_EQ(backend.live(), 1u);
    ASSERT_TRUE(c.try_close());
    EXPECT_EQ(backend.live(), 0u);
    EXPECT_EQ(c.try_close().error(), error::invalid_state);
  }
  EXPECT_EQ(backend.live(), 0u);
}

TEST_F(FilesystemRefTest, MoveAssignClosesPreviousHandle) {
  auto r1 = fs.try_open("/boot/cfg");
  auto r2 = fs.try_open("/boot/kernel");
  ASSERT_TRUE(r1);
  ASSERT_TRUE(r2);
  file a = std::move(*r1);
  file b = std::move(*r2);
  EXPECT_EQ(backend.live(), 2u);
  a = std::move(b);
  EXPECT_EQ(backend.live(), 1u);
}

TEST_F(FilesystemRefTest, DirectoryEnumeration) {
  auto d = fs.try_open_dir("/boot");
  ASSERT_TRUE(d);
  std::vector<std::string> names;
  std::vector<file_type> types;
  auto it = d->entries();
  for (auto e : it) {
    ASSERT_TRUE(e);
    names.emplace_back(e->name_view().data(), e->name_view().size());
    types.push_back(e->type);
  }
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[0], "cfg");
  EXPECT_EQ(names[1], "kernel");
  EXPECT_EQ(names[2], "sub");
  EXPECT_EQ(types[2], file_type::directory);
  EXPECT_EQ(fs.try_open_dir("/boot/cfg").error(), error::invalid_argument);
  EXPECT_EQ(fs.try_open_dir("/missing").error(), error::not_found);
}

TEST_F(FilesystemRefTest, DotEntriesSkipped) {
  ASSERT_TRUE(fs.try_mkdir("/boot/."));
  ASSERT_TRUE(fs.try_mkdir("/boot/.."));
  auto d = fs.try_open_dir("/boot");
  ASSERT_TRUE(d);
  std::size_t count = 0;
  auto it = d->entries();
  for (auto e : it) {
    ASSERT_TRUE(e);
    EXPECT_NE(e->name_view(), ".");
    EXPECT_NE(e->name_view(), "..");
    ++count;
  }
  EXPECT_EQ(count, 3u);
}

TEST_F(FilesystemRefTest, DirectoryClosedOnDestroy) {
  {
    auto d = fs.try_open_dir("/boot");
    ASSERT_TRUE(d);
    EXPECT_EQ(backend.live(), 1u);
  }
  EXPECT_EQ(backend.live(), 0u);
}

TEST_F(FilesystemRefTest, MkdirRemoveRename) {
  ASSERT_TRUE(fs.try_mkdir("/data"));
  EXPECT_EQ(fs.try_mkdir("/data").error(), error::already_exists);
  ASSERT_TRUE(fs.try_rename("/boot/cfg", "/data/cfg"));
  EXPECT_FALSE(fs.exists("/boot/cfg"));
  EXPECT_TRUE(fs.exists("/data/cfg"));
  ASSERT_TRUE(fs.try_remove("/data/cfg"));
  EXPECT_EQ(fs.try_remove("/data/cfg").error(), error::not_found);
  EXPECT_EQ(fs.try_rename("/a", "").error(), error::invalid_argument);
}

TEST_F(FilesystemRefTest, StatfsAndSyncAll) {
  auto info = fs.try_statfs();
  ASSERT_TRUE(info);
  EXPECT_EQ(info->block_size, 512u);
  EXPECT_TRUE(fs.try_sync_all());
}

TEST_F(FilesystemRefTest, ReadOnlyMountRejectsMutation) {
  backend.read_only = true;
  EXPECT_TRUE(fs.is_read_only());
  EXPECT_EQ(fs.try_mkdir("/x").error(), error::permission_denied);
  EXPECT_EQ(fs.try_remove("/boot/cfg").error(), error::permission_denied);
  EXPECT_EQ(fs.try_rename("/boot/cfg", "/boot/c2").error(), error::permission_denied);
  EXPECT_EQ(fs.try_open("/boot/cfg", open_flags::write).error(), error::permission_denied);
  EXPECT_EQ(fs.try_write_file("/boot/cfg", bytes("x")).error(), error::permission_denied);
  EXPECT_TRUE(fs.try_open("/boot/cfg", open_flags::read));
}

TEST(FilesystemRefOptional, MandatoryOnlyBackendIsReadOnly) {
  ro_fs ro;
  ro.inner.nodes["/f"] = node{file_type::regular, bytes("abc").v};
  filesystem_ref fs(ro);
  EXPECT_TRUE(fs.is_read_only());
  std::vector<std::byte> b(8);
  auto n = fs.try_read_file("/f", span<std::byte>(b));
  ASSERT_TRUE(n);
  EXPECT_EQ(str(b, *n), "abc");
  EXPECT_EQ(fs.try_open("/f", open_flags::write).error(), error::permission_denied);
  EXPECT_EQ(fs.try_mkdir("/d").error(), error::permission_denied);
  EXPECT_EQ(fs.try_statfs().error(), error::unsupported_operation);
  EXPECT_TRUE(fs.try_sync_all());
  auto f = fs.try_open("/f");
  ASSERT_TRUE(f);
  EXPECT_TRUE(f->try_sync());
}

} // namespace

RELOCO_END_UNSAFE_BUFFER_USAGE
