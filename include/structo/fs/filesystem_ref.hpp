// SPDX-FileCopyrightText: 2026 Jarosław Pelczar <jarek@jpelczar.com>
//
// SPDX-License-Identifier: BSD-2-Clause

#pragma once

/** @file filesystem_ref.hpp
 * @brief `structo::fs::filesystem_ref`: a type-erased, non-owning handle over a mounted filesystem with
 * read/write access, plus the `filesystem_traits<Backend>` customization point a concrete filesystem
 * (FAT, ext4, a RAM fs, a test fake) specializes to be bindable through it. This header contains no
 * filesystem implementation.
 *
 * Paths are `string_view`s whose syntax (separator, case sensitivity, root) is the backend's. The ref only
 * rejects empty paths and paths longer than `max_path_length`.
 *
 * Files and directories are opened as owning, movable, non-copyable `file` / `directory` objects that close
 * their backend handle when dropped. They keep a copy of the `filesystem_ref`, so the backend must outlive them.
 *
 * ## Customization point: `filesystem_traits<Backend>`
 *
 * @code
 * // Why: lets a bootloader load kernels/initrds/configs through one interface whether they live on
 * // FAT, ext4 or a RAM disk. Every function is static, `noexcept`, and takes the backend as first argument.
 *
 * template <> struct structo::fs::filesystem_traits<my_fs> {
 *   // --- mandatory ---
 *   // Opens an existing (or, with open_flags::create, new) regular file. `fs_handle` is an opaque token chosen
 *   // by the backend (e.g. an index into its open-file table); the ref never interprets it. Flags are
 *   // already validated.
 *   static reloco::result<structo::fs::fs_handle> try_open(my_fs &, reloco::string_view path,
 *                                                          structo::fs::open_flags flags) noexcept;
 *   // Opens a directory for iteration. error::not_found if missing.
 *   static reloco::result<structo::fs::fs_handle> try_open_dir(my_fs &, reloco::string_view path) noexcept;
 *   // Releases a handle returned by try_open or try_open_dir (and flushes it if it was written).
 *   static reloco::result<void> try_close(my_fs &, structo::fs::fs_handle) noexcept;
 *   // Reads up to dst.size() bytes at `offset`; returns the count, 0 at or past end of file.
 *   static reloco::result<std::size_t> try_read(my_fs &, structo::fs::fs_handle, std::uint64_t offset,
 *                                               reloco::span<std::byte> dst) noexcept;
 *   // Stores the next directory entry in `out` and returns true, or returns false at the end.
 *   static reloco::result<bool> try_read_dir(my_fs &, structo::fs::fs_handle,
 *                                            structo::fs::dir_entry &out) noexcept;
 *   // Type and size of the object at `path` / behind an open handle.
 *   static reloco::result<structo::fs::file_info> try_stat(my_fs &, reloco::string_view path) noexcept;
 *   static reloco::result<structo::fs::file_info> try_fstat(my_fs &, structo::fs::fs_handle) noexcept;
 *
 *   // --- optional (detected per name; absent means unsupported_operation) ---
 *   // Writes `src` at `offset` (growing the file as needed); returns bytes written. With open_flags::append
 *   // the backend must write at the end regardless of `offset`.
 *   static reloco::result<std::size_t> try_write(my_fs &, structo::fs::fs_handle, std::uint64_t offset,
 *                                                reloco::span<const std::byte> src) noexcept;
 *   static reloco::result<void> try_truncate(my_fs &, structo::fs::fs_handle, std::uint64_t size) noexcept;
 *   static reloco::result<void> try_sync(my_fs &, structo::fs::fs_handle) noexcept;     // flush one file
 *   static reloco::result<void> try_sync_all(my_fs &) noexcept;                          // flush everything
 *   static reloco::result<void> try_mkdir(my_fs &, reloco::string_view path) noexcept;
 *   static reloco::result<void> try_remove(my_fs &, reloco::string_view path) noexcept;  // file or empty dir
 *   static reloco::result<void> try_rename(my_fs &, reloco::string_view from, reloco::string_view to) noexcept;
 *   static reloco::result<structo::fs::filesystem_info> try_statfs(my_fs &) noexcept;
 *   // Mounted read-only. If absent, the fs is writable iff try_write exists.
 *   static bool is_read_only(my_fs &) noexcept;
 * };
 * @endcode
 *
 * ## Using it
 *
 * @code
 * my_fs backend;                                   // the concrete filesystem; must outlive everything below
 * structo::fs::filesystem_ref fs(backend);         // bind (lvalues only)
 *
 * // One-shot helpers for the common bootloader cases.
 * reloco::array<std::byte, 4096> config;
 * auto n = fs.try_read_file("/boot/boot.cfg", config); // path, destination; n = bytes read;
 *                                                      // capacity_exceeded if too small
 * (void)fs.try_write_file("/boot/env", env_bytes);     // create or truncate, write everything, close
 *
 * // Streaming access.
 * auto f = fs.try_open("/boot/vmlinuz", structo::fs::open_flags::read); // `f` owns the handle
 * if (f) {
 *   auto st = f->try_stat();                       // type and size
 *   (void)f->try_read(0, header_buf);              // positional read: offset, destination
 *   (void)f->try_read(chunk);                      // cursor read: continues where the last call stopped
 * }                                                // closed here
 *
 * // Directory listing ("." and ".." are skipped).
 * auto dir = fs.try_open_dir("/boot");
 * if (dir) {
 *   auto it = dir->entries();
 *   for (auto e : it) {
 *     if (!e)
 *       break;                                     // backend error ends the listing
 *     log(e->name_view(), e->type == structo::fs::file_type::directory);
 *   }
 * }
 * @endcode
 */

#include <reloco/array.hpp>
#include <reloco/error.hpp>
#include <reloco/expected.hpp>
#include <reloco/iterator.hpp>
#include <reloco/lifetime.hpp>
#include <reloco/optional.hpp>
#include <reloco/span.hpp>
#include <reloco/string_view.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace structo::fs {

using namespace reloco;

/** @brief Opaque per-open-object token chosen by the backend. */
using fs_handle = std::uint64_t;

/** @brief Longest path the ref accepts. */
inline constexpr std::size_t max_path_length = 4096;
/** @brief Longest directory entry name. */
inline constexpr std::size_t max_name_length = 255;

enum class file_type : std::uint8_t { regular, directory, symlink, other };

enum class open_flags : std::uint8_t {
  none = 0,
  read = 1 << 0,
  write = 1 << 1,
  /** @brief Create the file if it does not exist. Needs `write`. */
  create = 1 << 2,
  /** @brief Discard existing contents on open. Needs `write`. */
  truncate = 1 << 3,
  /** @brief Every write goes to the end of the file. Needs `write`. */
  append = 1 << 4,
  /** @brief With `create`: fail with `error::already_exists` if the file exists. Needs `write`. */
  exclusive = 1 << 5,
};

[[nodiscard]] constexpr open_flags operator|(open_flags a, open_flags b) noexcept {
  return static_cast<open_flags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr open_flags operator&(open_flags a, open_flags b) noexcept {
  return static_cast<open_flags>(static_cast<std::uint8_t>(a) & static_cast<std::uint8_t>(b));
}
/** @brief True if @p flags contains every bit of @p bits. */
[[nodiscard]] constexpr bool has_flag(open_flags flags, open_flags bits) noexcept { return (flags & bits) == bits; }

struct file_info {
  file_type type = file_type::regular;
  std::uint64_t size = 0;

  [[nodiscard]] constexpr bool is_directory() const noexcept { return type == file_type::directory; }
  [[nodiscard]] constexpr bool is_regular() const noexcept { return type == file_type::regular; }
};

struct dir_entry {
  array<char, max_name_length> name{};
  std::uint8_t name_length = 0;
  file_type type = file_type::regular;
  /** @brief Size in bytes if the backend knows it cheaply, else 0. */
  std::uint64_t size = 0;

  [[nodiscard]] string_view name_view() const noexcept { return string_view(name.data(), name_length); }

  /** @brief Copies @p s into `name` (truncating at `max_name_length`). Helper for backends. */
  void set_name(string_view s) noexcept {
    const std::size_t n = s.size() < max_name_length ? s.size() : max_name_length;
    for (std::size_t i = 0; i < n; ++i)
      name[i] = s[i];
    name_length = static_cast<std::uint8_t>(n);
  }
};

struct filesystem_info {
  std::uint32_t block_size = 0;
  std::uint64_t total_blocks = 0;
  std::uint64_t free_blocks = 0;
};

template <typename Backend> struct filesystem_traits;

namespace detail {

template <typename Backend, typename = void> struct has_filesystem_traits : std::false_type {};
template <typename Backend>
struct has_filesystem_traits<
    Backend,
    std::void_t<decltype(&filesystem_traits<Backend>::try_open), decltype(&filesystem_traits<Backend>::try_open_dir),
                decltype(&filesystem_traits<Backend>::try_close), decltype(&filesystem_traits<Backend>::try_read),
                decltype(&filesystem_traits<Backend>::try_read_dir), decltype(&filesystem_traits<Backend>::try_stat),
                decltype(&filesystem_traits<Backend>::try_fstat)>> : std::true_type {};

// Detects an optional static member `T::member`.
#define STRUCTO_FS_DETECT(member)                                                                                      \
  template <typename T, typename = void> struct has_##member : std::false_type {};                                     \
  template <typename T> struct has_##member<T, std::void_t<decltype(&T::member)>> : std::true_type {};

STRUCTO_FS_DETECT(try_write)
STRUCTO_FS_DETECT(try_truncate)
STRUCTO_FS_DETECT(try_sync)
STRUCTO_FS_DETECT(try_sync_all)
STRUCTO_FS_DETECT(try_mkdir)
STRUCTO_FS_DETECT(try_remove)
STRUCTO_FS_DETECT(try_rename)
STRUCTO_FS_DETECT(try_statfs)
STRUCTO_FS_DETECT(is_read_only)

#undef STRUCTO_FS_DETECT

} // namespace detail

class file;
class directory;
class directory_iterator;

class RELOCO_POINTER filesystem_ref {
public:
  struct vtable {
    result<fs_handle> (*open)(void *, string_view, open_flags) noexcept;
    result<fs_handle> (*open_dir)(void *, string_view) noexcept;
    result<void> (*close)(void *, fs_handle) noexcept;
    result<std::size_t> (*read)(void *, fs_handle, std::uint64_t, span<std::byte>) noexcept;
    result<std::size_t> (*write)(void *, fs_handle, std::uint64_t, span<const std::byte>) noexcept;
    result<bool> (*read_dir)(void *, fs_handle, dir_entry &) noexcept;
    result<file_info> (*stat)(void *, string_view) noexcept;
    result<file_info> (*fstat)(void *, fs_handle) noexcept;
    result<void> (*truncate)(void *, fs_handle, std::uint64_t) noexcept;
    result<void> (*sync)(void *, fs_handle) noexcept;
    result<void> (*sync_all)(void *) noexcept;
    result<void> (*mkdir)(void *, string_view) noexcept;
    result<void> (*remove)(void *, string_view) noexcept;
    result<void> (*rename)(void *, string_view, string_view) noexcept;
    result<filesystem_info> (*statfs)(void *) noexcept;
    bool (*is_read_only)(void *) noexcept;
  };

  constexpr filesystem_ref() noexcept = default;

  template <typename Backend, std::enable_if_t<detail::has_filesystem_traits<Backend>::value, int> = 0>
  constexpr explicit filesystem_ref(Backend &b RELOCO_LIFETIMEBOUND RELOCO_LIFETIME_CAPTURE_BY_THIS) noexcept
      : ctx_(std::addressof(b)), vtbl_(&s_vtbl<Backend>) {}

  template <typename Backend, std::enable_if_t<!std::is_lvalue_reference_v<Backend>, int> = 0>
  filesystem_ref(Backend &&) = delete;

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return vtbl_ != nullptr; }

  /** @brief True if nothing can be written: unbound, no write support, or mounted read-only. */
  [[nodiscard]] bool is_read_only() const noexcept { return !vtbl_ || vtbl_->is_read_only(ctx_); }

  /** @brief Opens a file. Fails with `error::invalid_argument` for a bad path or inconsistent @p flags (neither
   * read nor write; create/truncate/append/exclusive without write), and `error::permission_denied` if write
   * access is requested on a read-only filesystem. */
  [[nodiscard]] result<file> try_open(string_view path, open_flags flags = open_flags::read) const noexcept;

  /** @brief Opens a directory for listing. */
  [[nodiscard]] result<directory> try_open_dir(string_view path) const noexcept;

  [[nodiscard]] result<file_info> try_stat(string_view path) const noexcept {
    if (auto r = check_path(path); !r)
      return unexpected(r.error());
    return vtbl_->stat(ctx_, path);
  }

  /** @brief True if @p path exists. */
  [[nodiscard]] bool exists(string_view path) const noexcept { return try_stat(path).has_value(); }

  [[nodiscard]] result<void> try_mkdir(string_view path) const noexcept {
    if (auto r = check_mutation(path); !r)
      return r;
    return vtbl_->mkdir(ctx_, path);
  }

  /** @brief Removes a file or an empty directory. */
  [[nodiscard]] result<void> try_remove(string_view path) const noexcept {
    if (auto r = check_mutation(path); !r)
      return r;
    return vtbl_->remove(ctx_, path);
  }

  [[nodiscard]] result<void> try_rename(string_view from, string_view to) const noexcept {
    if (auto r = check_mutation(from); !r)
      return r;
    if (auto r = check_path(to); !r)
      return r;
    return vtbl_->rename(ctx_, from, to);
  }

  /** @brief Flushes all pending writes of the whole filesystem. */
  [[nodiscard]] result<void> try_sync_all() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->sync_all(ctx_);
  }

  [[nodiscard]] result<filesystem_info> try_statfs() const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    return vtbl_->statfs(ctx_);
  }

  /** @brief Reads the whole file at @p path into @p dst and returns its size. `error::capacity_exceeded` if the
   * file is larger than @p dst. */
  [[nodiscard]] result<std::size_t> try_read_file(string_view path, span<std::byte> dst) const noexcept;

  /** @brief Creates or truncates the file at @p path, writes all of @p src and closes it. */
  [[nodiscard]] result<void> try_write_file(string_view path, span<const std::byte> src) const noexcept;

private:
  friend class file;
  friend class directory;
  friend class directory_iterator;

  [[nodiscard]] result<void> check_path(string_view path) const noexcept {
    if (!vtbl_)
      return unexpected(error::unsupported_operation);
    if (path.empty() || path.size() > max_path_length)
      return unexpected(error::invalid_argument);
    return {};
  }
  [[nodiscard]] result<void> check_mutation(string_view path) const noexcept {
    if (auto r = check_path(path); !r)
      return r;
    if (is_read_only())
      return unexpected(error::permission_denied);
    return {};
  }

  template <typename Backend> static result<fs_handle> open_entry(void *c, string_view p, open_flags f) noexcept {
    return filesystem_traits<Backend>::try_open(*static_cast<Backend *>(c), p, f);
  }
  template <typename Backend> static result<fs_handle> open_dir_entry(void *c, string_view p) noexcept {
    return filesystem_traits<Backend>::try_open_dir(*static_cast<Backend *>(c), p);
  }
  template <typename Backend> static result<void> close_entry(void *c, fs_handle h) noexcept {
    return filesystem_traits<Backend>::try_close(*static_cast<Backend *>(c), h);
  }
  template <typename Backend>
  static result<std::size_t> read_entry(void *c, fs_handle h, std::uint64_t off, span<std::byte> dst) noexcept {
    return filesystem_traits<Backend>::try_read(*static_cast<Backend *>(c), h, off, dst);
  }
  template <typename Backend> static result<bool> read_dir_entry(void *c, fs_handle h, dir_entry &out) noexcept {
    return filesystem_traits<Backend>::try_read_dir(*static_cast<Backend *>(c), h, out);
  }
  template <typename Backend> static result<file_info> stat_entry(void *c, string_view p) noexcept {
    return filesystem_traits<Backend>::try_stat(*static_cast<Backend *>(c), p);
  }
  template <typename Backend> static result<file_info> fstat_entry(void *c, fs_handle h) noexcept {
    return filesystem_traits<Backend>::try_fstat(*static_cast<Backend *>(c), h);
  }

  // Optional operations: forward when the traits provide them, else fail with unsupported_operation.
  template <typename Backend>
  static result<std::size_t> write_entry(void *c, fs_handle h, std::uint64_t off, span<const std::byte> src) noexcept {
    using traits = filesystem_traits<Backend>;
    if constexpr (detail::has_try_write<traits>::value) {
      return traits::try_write(*static_cast<Backend *>(c), h, off, src);
    } else {
      (void)c, (void)h, (void)off, (void)src;
      return unexpected(error::unsupported_operation);
    }
  }
  template <typename Backend> static result<void> truncate_entry(void *c, fs_handle h, std::uint64_t size) noexcept {
    using traits = filesystem_traits<Backend>;
    if constexpr (detail::has_try_truncate<traits>::value) {
      return traits::try_truncate(*static_cast<Backend *>(c), h, size);
    } else {
      (void)c, (void)h, (void)size;
      return unexpected(error::unsupported_operation);
    }
  }
  // sync/sync_all succeed trivially when the backend has no write-back state.
  template <typename Backend> static result<void> sync_entry(void *c, fs_handle h) noexcept {
    using traits = filesystem_traits<Backend>;
    if constexpr (detail::has_try_sync<traits>::value) {
      return traits::try_sync(*static_cast<Backend *>(c), h);
    } else {
      (void)c, (void)h;
      return {};
    }
  }
  template <typename Backend> static result<void> sync_all_entry(void *c) noexcept {
    using traits = filesystem_traits<Backend>;
    if constexpr (detail::has_try_sync_all<traits>::value) {
      return traits::try_sync_all(*static_cast<Backend *>(c));
    } else {
      (void)c;
      return {};
    }
  }
  template <typename Backend> static result<void> mkdir_entry(void *c, string_view p) noexcept {
    using traits = filesystem_traits<Backend>;
    if constexpr (detail::has_try_mkdir<traits>::value) {
      return traits::try_mkdir(*static_cast<Backend *>(c), p);
    } else {
      (void)c, (void)p;
      return unexpected(error::unsupported_operation);
    }
  }
  template <typename Backend> static result<void> remove_entry(void *c, string_view p) noexcept {
    using traits = filesystem_traits<Backend>;
    if constexpr (detail::has_try_remove<traits>::value) {
      return traits::try_remove(*static_cast<Backend *>(c), p);
    } else {
      (void)c, (void)p;
      return unexpected(error::unsupported_operation);
    }
  }
  template <typename Backend> static result<void> rename_entry(void *c, string_view a, string_view b) noexcept {
    using traits = filesystem_traits<Backend>;
    if constexpr (detail::has_try_rename<traits>::value) {
      return traits::try_rename(*static_cast<Backend *>(c), a, b);
    } else {
      (void)c, (void)a, (void)b;
      return unexpected(error::unsupported_operation);
    }
  }
  template <typename Backend> static result<filesystem_info> statfs_entry(void *c) noexcept {
    using traits = filesystem_traits<Backend>;
    if constexpr (detail::has_try_statfs<traits>::value) {
      return traits::try_statfs(*static_cast<Backend *>(c));
    } else {
      (void)c;
      return unexpected(error::unsupported_operation);
    }
  }
  template <typename Backend> static bool is_read_only_entry(void *c) noexcept {
    using traits = filesystem_traits<Backend>;
    if constexpr (!detail::has_try_write<traits>::value) {
      (void)c;
      return true;
    } else if constexpr (detail::has_is_read_only<traits>::value) {
      return traits::is_read_only(*static_cast<Backend *>(c));
    } else {
      (void)c;
      return false;
    }
  }

  template <typename Backend>
  static constexpr vtable s_vtbl{
      &open_entry<Backend>,     &open_dir_entry<Backend>, &close_entry<Backend>,    &read_entry<Backend>,
      &write_entry<Backend>,    &read_dir_entry<Backend>, &stat_entry<Backend>,     &fstat_entry<Backend>,
      &truncate_entry<Backend>, &sync_entry<Backend>,     &sync_all_entry<Backend>, &mkdir_entry<Backend>,
      &remove_entry<Backend>,   &rename_entry<Backend>,   &statfs_entry<Backend>,   &is_read_only_entry<Backend>};

  void *ctx_ = nullptr;
  const vtable *vtbl_ = nullptr;
};

/** @brief An open file. Owns its backend handle (closed on destruction); movable, not copyable. A
 * default-constructed, moved-from or closed file is inactive and every call fails with `error::invalid_state`. */
class file {
public:
  file() noexcept = default;
  file(const file &) = delete;
  file &operator=(const file &) = delete;
  file(file &&o) noexcept { take(o); }
  file &operator=(file &&o) noexcept {
    if (this != &o) {
      reset();
      take(o);
    }
    return *this;
  }
  ~file() { reset(); }

  [[nodiscard]] explicit operator bool() const noexcept { return active_; }

  /** @brief Reads up to `dst.size()` bytes at @p offset; 0 means end of file. Needs `open_flags::read`. */
  [[nodiscard]] result<std::size_t> try_read(std::uint64_t offset, span<std::byte> dst) const noexcept {
    if (!active_)
      return unexpected(error::invalid_state);
    if (!has_flag(flags_, open_flags::read))
      return unexpected(error::permission_denied);
    return fs_.vtbl_->read(fs_.ctx_, handle_, offset, dst);
  }

  /** @brief Reads at the cursor and advances it by the bytes read. */
  [[nodiscard]] result<std::size_t> try_read(span<std::byte> dst) noexcept {
    auto n = try_read(pos_, dst);
    if (n)
      pos_ += *n;
    return n;
  }

  /** @brief Writes `src` at @p offset (the end of the file with `open_flags::append`); returns bytes written,
   * which may be less than `src.size()`. Needs `open_flags::write`. */
  [[nodiscard]] result<std::size_t> try_write(std::uint64_t offset, span<const std::byte> src) const noexcept {
    if (!active_)
      return unexpected(error::invalid_state);
    if (!has_flag(flags_, open_flags::write))
      return unexpected(error::permission_denied);
    return fs_.vtbl_->write(fs_.ctx_, handle_, offset, src);
  }

  /** @brief Writes at the cursor and advances it. */
  [[nodiscard]] result<std::size_t> try_write(span<const std::byte> src) noexcept {
    auto n = try_write(pos_, src);
    if (n)
      pos_ += *n;
    return n;
  }

  [[nodiscard]] result<file_info> try_stat() const noexcept {
    if (!active_)
      return unexpected(error::invalid_state);
    return fs_.vtbl_->fstat(fs_.ctx_, handle_);
  }

  [[nodiscard]] result<void> try_truncate(std::uint64_t size) const noexcept {
    if (!active_)
      return unexpected(error::invalid_state);
    if (!has_flag(flags_, open_flags::write))
      return unexpected(error::permission_denied);
    return fs_.vtbl_->truncate(fs_.ctx_, handle_, size);
  }

  /** @brief Flushes this file's pending writes. */
  [[nodiscard]] result<void> try_sync() const noexcept {
    if (!active_)
      return unexpected(error::invalid_state);
    return fs_.vtbl_->sync(fs_.ctx_, handle_);
  }

  void seek(std::uint64_t pos) noexcept { pos_ = pos; }
  [[nodiscard]] std::uint64_t tell() const noexcept { return pos_; }
  [[nodiscard]] open_flags flags() const noexcept { return flags_; }

  /** @brief Closes the file now and reports the backend's result. Inactive afterwards either way. */
  [[nodiscard]] result<void> try_close() noexcept {
    if (!active_)
      return unexpected(error::invalid_state);
    active_ = false;
    return fs_.vtbl_->close(fs_.ctx_, handle_);
  }

private:
  friend class filesystem_ref;

  file(filesystem_ref fs, fs_handle h, open_flags flags) noexcept : fs_(fs), handle_(h), flags_(flags), active_(true) {}

  void reset() noexcept {
    if (active_)
      (void)try_close();
  }
  void take(file &o) noexcept {
    fs_ = o.fs_;
    handle_ = o.handle_;
    flags_ = o.flags_;
    pos_ = o.pos_;
    active_ = o.active_;
    o.active_ = false;
  }

  filesystem_ref fs_;
  fs_handle handle_ = 0;
  open_flags flags_ = open_flags::none;
  std::uint64_t pos_ = 0;
  bool active_ = false;
};

/** @brief Single-pass iterator over a directory's entries (skips "." and ".."). A backend error yields one error
 * item, then iteration ends. Valid while the `directory` it came from stays open. */
class RELOCO_POINTER directory_iterator : public iterator_adaptor<directory_iterator, result<dir_entry>> {
public:
  using item_type = result<dir_entry>;

  directory_iterator() noexcept = default;
  directory_iterator(filesystem_ref fs, fs_handle h) noexcept : fs_(fs), handle_(h) {}

  [[nodiscard]] optional<item_type> next_impl() noexcept {
    while (!done_) {
      dir_entry e;
      auto more = fs_.vtbl_->read_dir(fs_.ctx_, handle_, e);
      if (!more) {
        done_ = true;
        return optional<item_type>(item_type(unexpected(more.error())));
      }
      if (!*more) {
        done_ = true;
        return nullopt;
      }
      const auto n = e.name_view();
      if (n == "." || n == "..")
        continue;
      return optional<item_type>(item_type(e));
    }
    return nullopt;
  }

private:
  filesystem_ref fs_;
  fs_handle handle_ = 0;
  bool done_ = false;
};

/** @brief An open directory. Owns its backend handle; movable, not copyable. */
class directory {
public:
  directory() noexcept = default;
  directory(const directory &) = delete;
  directory &operator=(const directory &) = delete;
  directory(directory &&o) noexcept { take(o); }
  directory &operator=(directory &&o) noexcept {
    if (this != &o) {
      reset();
      take(o);
    }
    return *this;
  }
  ~directory() { reset(); }

  [[nodiscard]] explicit operator bool() const noexcept { return active_; }

  /** @brief A fresh iterator continuing from the backend's current directory position. Keep this directory open
   * while iterating. */
  [[nodiscard]] directory_iterator entries() const noexcept RELOCO_LIFETIMEBOUND {
    return active_ ? directory_iterator(fs_, handle_) : directory_iterator();
  }

  /** @brief Closes the directory now and reports the backend's result. */
  [[nodiscard]] result<void> try_close() noexcept {
    if (!active_)
      return unexpected(error::invalid_state);
    active_ = false;
    return fs_.vtbl_->close(fs_.ctx_, handle_);
  }

private:
  friend class filesystem_ref;

  directory(filesystem_ref fs, fs_handle h) noexcept : fs_(fs), handle_(h), active_(true) {}

  void reset() noexcept {
    if (active_)
      (void)try_close();
  }
  void take(directory &o) noexcept {
    fs_ = o.fs_;
    handle_ = o.handle_;
    active_ = o.active_;
    o.active_ = false;
  }

  filesystem_ref fs_;
  fs_handle handle_ = 0;
  bool active_ = false;
};

inline result<file> filesystem_ref::try_open(string_view path, open_flags flags) const noexcept {
  if (auto r = check_path(path); !r)
    return unexpected(r.error());
  const bool wants_write = has_flag(flags, open_flags::write);
  const bool wants_read = has_flag(flags, open_flags::read);
  const open_flags write_only_bits =
      open_flags::create | open_flags::truncate | open_flags::append | open_flags::exclusive;
  if (!wants_read && !wants_write)
    return unexpected(error::invalid_argument);
  if (!wants_write && (flags & write_only_bits) != open_flags::none)
    return unexpected(error::invalid_argument);
  if (wants_write && is_read_only())
    return unexpected(error::permission_denied);
  auto h = vtbl_->open(ctx_, path, flags);
  if (!h)
    return unexpected(h.error());
  return file(*this, *h, flags);
}

inline result<directory> filesystem_ref::try_open_dir(string_view path) const noexcept {
  if (auto r = check_path(path); !r)
    return unexpected(r.error());
  auto h = vtbl_->open_dir(ctx_, path);
  if (!h)
    return unexpected(h.error());
  return directory(*this, *h);
}

inline result<std::size_t> filesystem_ref::try_read_file(string_view path, span<std::byte> dst) const noexcept {
  auto f = try_open(path, open_flags::read);
  if (!f)
    return unexpected(f.error());
  auto st = f->try_stat();
  if (!st)
    return unexpected(st.error());
  if (!st->is_regular())
    return unexpected(error::invalid_argument);
  if (st->size > dst.size())
    return unexpected(error::capacity_exceeded);
  const auto total = static_cast<std::size_t>(st->size);
  std::size_t done = 0;
  while (done < total) {
    auto n = f->try_read(done, dst.subspan(done, total - done));
    if (!n)
      return unexpected(n.error());
    if (*n == 0)
      break; // The file shrank under us; report what was read.
    done += *n;
  }
  return done;
}

inline result<void> filesystem_ref::try_write_file(string_view path, span<const std::byte> src) const noexcept {
  auto f = try_open(path, open_flags::write | open_flags::create | open_flags::truncate);
  if (!f)
    return unexpected(f.error());
  std::size_t done = 0;
  while (done < src.size()) {
    auto n = f->try_write(done, src.subspan(done));
    if (!n)
      return unexpected(n.error());
    if (*n == 0)
      return unexpected(error::io_error);
    done += *n;
  }
  if (auto r = f->try_sync(); !r)
    return r;
  return f->try_close();
}

} // namespace structo::fs
