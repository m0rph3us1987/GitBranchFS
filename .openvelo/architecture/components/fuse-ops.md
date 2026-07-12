# FUSE callback table

`include/fuse_ops.h` + `src/fuse_ops.c` is the project's libfuse3
glue. Its job is narrow: own the `gbfs_oper` vtable, provide a
`filler_adapter` so the project's `(void *, const char *,
const struct stat *, int64_t)`-shaped `gbfs_read_dir` filler
matches libfuse3's `fuse_fill_dir_t`, and run `fuse_main` from
`run_fuse_fs`.

Every FUSE callback is a thin shim: it pulls `gbfs_state_t *state`
out of `fuse_get_context()->private_data` and forwards to the
matching `gbfs_*` function (see
[components/core-fs.md](./core-fs.md) and
[components/core-dir.md](./core-dir.md)). This file is not where the
logic lives.

## Key files

| File                | Responsibility                                                                                              |
|---------------------|-------------------------------------------------------------------------------------------------------------|
| `include/fuse_ops.h`| Public header. Single prototype: `int run_fuse_fs(int argc, char **argv, gbfs_state_t *state);`.             |
| `src/fuse_ops.c`    | The `gbfs_oper` vtable (15 callbacks registered), `filler_adapter` shim, `gbfs_fuse_*` thin callback bodies, and `run_fuse_fs` (one-line `fuse_main`). |
| `Makefile`          | `wildcard src/*.c` picks up `src/fuse_ops.c`. The Makefile defines `#define FUSE_USE_VERSION 31` at the top of the TU so libfuse3 exposes the right API. |

## The `gbfs_oper` vtable

`src/fuse_ops.c:123-139` declares 15 callbacks wired into a single
`struct fuse_operations`:

```c
static const struct fuse_operations gbfs_oper = {
    .getattr  = gbfs_fuse_getattr,
    .readdir  = gbfs_fuse_readdir,
    .open     = gbfs_fuse_open,
    .read     = gbfs_fuse_read,
    .write    = gbfs_fuse_write,
    .release  = gbfs_fuse_release,
    .create   = gbfs_fuse_create,
    .unlink   = gbfs_fuse_unlink,
    .mkdir    = gbfs_fuse_mkdir,
    .rmdir    = gbfs_fuse_rmdir,
    .rename   = gbfs_fuse_rename,
    .truncate = gbfs_fuse_truncate,
    .utimens  = gbfs_fuse_utimens,
    .chmod    = gbfs_fuse_chmod,
    .chown    = gbfs_fuse_chown,
};
```

`symlink`, `readlink`, `access`, `flush`, `opendir`,
`releasedir` are deliberately not registered — those FUSE ops are
unsupported in this build.

`statfs` *is* registered (`src/fuse_ops.c:gbfs_fuse_statfs`). WinFSP uses it
to answer Windows' `FileFsSizeInformation` queries; without it WinFSP
reports zero free bytes and every write fails with `ERROR_DISK_FULL`
("There is not enough space on the disk."). The callback forwards to
`gbfs_statvfs` (`include/compat.h`) which queries the overlay host volume
via `statvfs` on POSIX and `GetDiskFreeSpaceExA` + `GetDiskFreeSpaceA` on
Windows, with a non-zero fallback so a transient query failure cannot
reintroduce the disk-full symptom.

## Callback shape

Every callback uses the same template:

```c
static int gbfs_fuse_<op>(...) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    return gbfs_<op>(state, ...);
}
```

with minor variations where libfuse3 uses a different type or an
extra parameter:

- `gbfs_fuse_open(path, fi)` — calls
  `gbfs_open_file(state, path, fi->flags, &fh)`, stashes `fh` into
  `fi->fh` cast through `(uintptr_t)` on success.
- `gbfs_fuse_read(path, buf, size, offset, fi)` — calls
  `gbfs_read_file(state, (void *)(uintptr_t)fi->fh, buf, size,
  (int64_t)offset)`.
- `gbfs_fuse_write`, `gbfs_fuse_release` — same pattern as `read`.
- `gbfs_fuse_create(path, mode, fi)` — calls
  `gbfs_create_file(state, path, mode, &fh)`, stashes `fh` into
  `fi->fh`.
- `gbfs_fuse_unlink` / `gbfs_fuse_mkdir` / `gbfs_fuse_rmdir` /
  `gbfs_fuse_rename` — call the matching `gbfs_*` and forward
  results. (`gbfs_fuse_rename` refuses cross-rename with the
  `flags` argument: `if (flags) return -EINVAL;` per libfuse3
  conventions.)
- `gbfs_fuse_truncate` / `gbfs_fuse_utimens` / `gbfs_fuse_chmod` /
  `gbfs_fuse_chown` — call the matching `gbfs_*`.

## The `filler_adapter` shim

The project's `gbfs_read_dir` filler signature is:

```c
int (*filler)(void *, const char *, const struct stat *, int64_t);
```

libfuse3's `fuse_fill_dir_t` is:

```c
typedef int (*fuse_fill_dir_t)(void *buf, const char *name,
                               const struct stat *st,
                               off_t off, unsigned flags);
```

`filler_adapter` (`src/fuse_ops.c:14-17`) bridges them:

```c
static int filler_adapter(void *ctx_void, const char *name,
                          const struct stat *st, int64_t offset) {
    struct readdir_ctx *ctx = (struct readdir_ctx *)ctx_void;
    return ctx->filler(ctx->buf, name, st, (off_t)offset, 0);
}
```

It carries a small `struct readdir_ctx { void *buf; fuse_fill_dir_t
filler; }` so the `readdir` callback can hand the project's filler
plus the libfuse3 buffer to the adapter.

`gbfs_fuse_readdir` allocates the `readdir_ctx`, then calls
`gbfs_read_dir(state, path, &ctx, filler_adapter)`, which iterates the
merged entries and invokes `filler_adapter` once per name; the adapter
forwards to libfuse3's filler with the right buffer and a zero
`flags` argument.

## `run_fuse_fs`

```c
int run_fuse_fs(int argc, char *argv[], gbfs_state_t *state) {
    return fuse_main(argc, argv, &gbfs_oper, state);
}
```

That's the whole entry point. The `gbfs_oper` table inside the same
TU is what libfuse3 looks up by name to dispatch each kernel-issued
request; the `state` argument becomes the
`fuse_get_context()->private_data` for every callback.

`cli.c` calls this with a platform-specific `argv` after `gbfs_init`
returns. Linux/macOS:

```c
char *fuse_argv[5];
fuse_argv[0] = argv[0];
fuse_argv[1] = mount_path;
fuse_argv[2] = "-s";                             // single-threaded
fuse_argv[3] = "-odefault_permissions";          // kernel permission checks
int fuse_argc = 4;
int ret = run_fuse_fs(fuse_argc, fuse_argv, state);
```

Windows (WinFSP-FUSE3):

```c
char *fuse_argv[6];
fuse_argv[0] = argv[0];
fuse_argv[1] = mount_path;
fuse_argv[2] = "-s";                             // single-threaded
fuse_argv[3] = "-ouid=-1";                       // WinFSP: sentinel for
fuse_argv[4] = "-ogid=-1";                       //   current-token uid/gid
int fuse_argc = 5;
int ret = run_fuse_fs(fuse_argc, fuse_argv, state);
```

WinFSP-FUSE3 silently discards `default_permissions` and resolves
`-ouid=-1`/`-ogid=-1` to the current process's real UID/GID via the
security token (`fsp_fuse_get_token_uidgid`), so files are owned by the
mounting user. libfuse3 on Linux instead interprets `-1` literally as
`(unsigned)-1 = 4294967295` for every stat reply, which would deny all
writes — hence the platform branch.

`fuse_main` blocks until the mount is unmounted (either via
`gbfs unmount` → `fusermount3 -u`, or via Ctrl-C / external
`fusermount3 -u`). The return value propagates back as the CLI's exit
code.

## Integration points

- `src/cli.c:cli_run` — owns the argv preparation and the call to
  `run_fuse_fs`. See [components/cli.md](./cli.md).
- `src/core_fs.c:gbfs_init` / `gbfs_destroy` — create/destroy the
  `gbfs_state_t` that `run_fuse_fs` hands to libfuse3.
- `src/fuse_ops.c:gbfs_fuse_*` — every callback forwards into either
  `src/core_fs.c` or `src/core_dir.c`.
- `Makefile` — `wildcard src/*.c` picks up `src/fuse_ops.c`. Note the
  leading `#define FUSE_USE_VERSION 31` so the `<fuse3/fuse.h>`
  declarations match the libfuse3 ABI we compiled against. libfuse3
  also defines `fuse_fill_dir_t` only when this version is set at
  compile time.
- `pkg-config fuse3` — provides the `-I` and `-l` flags the
  Makefile picks up via `CFLAGS` / `LIBS`.

## Non-goals

- No `fs_ops_t` portability vtable. The old codebase declared an
  `fs_ops_t` indirection (`include/gbs/fs.h`) with a POSIX backend in
  `src/util/fs_posix.c`; that layer is gone. Every callback in
  `gbfs_oper` routes directly into the gbfs / git_wrapper / overlay
  implementations in `src/`.
- No `--mount-internal` self-re-exec path, no fork+exec of the same
  binary. `cli_run` runs `fuse_main` in the same process and blocks
  until unmount.
- No SIGUSR1 graceful-unmount handler, no signal-based escalation.
  Unmount is performed by another `gbfs unmount` invocation
  that shells out to `fusermount3 -u`, which terminates `fuse_main`
  by detaching the kernel mount.
- No `opendir` / `releasedir` — listing a directory does not allocate
  per-open state, so there is nothing to release.
