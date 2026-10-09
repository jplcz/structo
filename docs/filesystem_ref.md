# filesystem_ref

`include/structo/fs/filesystem_ref.hpp` — a type-erased, non-owning handle over a mounted filesystem with read/write access. It contains no concrete filesystem; FAT and ext4 readers will implement `filesystem_traits` on top of it.

```cpp
// A concrete filesystem (FAT, ext4, RAM fs, ...) opts in by specializing the traits. The seven
// mandatory functions are open/open_dir/close/read/read_dir/stat/fstat; everything else is optional.
// See the header for the full list and the meaning of every argument.
template <> struct structo::fs::filesystem_traits<my_fs> { /* try_open, try_read, ... */ };

my_fs backend;                              // must outlive the ref and every file/directory opened from it
structo::fs::filesystem_ref fs(backend);    // non-owning; lvalues only

// One-shot helpers: load a config file into a caller buffer. Returns the file size, or
// capacity_exceeded (nothing partially returned) if the buffer is too small.
reloco::array<std::byte, 4096> buf;
auto n = fs.try_read_file("/boot/boot.cfg", buf);

// Streaming: `f` owns the backend handle and closes it when destroyed (or via try_close()).
auto f = fs.try_open("/boot/vmlinuz", structo::fs::open_flags::read);
auto got = f->try_read(0, header);          // positional read: offset, destination (0 bytes = EOF)

// Writing needs open_flags::write; create/truncate/append/exclusive are only valid together with it.
auto w = fs.try_open("/boot/env", structo::fs::open_flags::write | structo::fs::open_flags::create);

// Directory listing; "." and ".." are skipped. A backend error appears as one error item.
auto dir = fs.try_open_dir("/boot");
auto it = dir->entries();
for (auto e : it) { if (!e) break; use(e->name_view(), e->type); }
```

Notes:

- Optional operations absent from the traits fail with `unsupported_operation`; `try_sync` / `try_sync_all` succeed trivially. A backend without `try_write` is read-only; any write-type call on a read-only filesystem returns `permission_denied`.
- Invalid input (empty or over-long path, inconsistent flags) returns `invalid_argument`. An unbound ref returns `unsupported_operation`.
- `file` / `directory` are movable, not copyable; inactive (moved-from or closed) objects return `invalid_state`.
- Writes may be short; `try_write_file` loops until everything is written.
