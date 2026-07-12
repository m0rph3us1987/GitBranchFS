# Overview

gitbranchfs is a single-process C11 binary that exposes a single git
branch as a FUSE mount with per-mountpoint overlay and deletion
state materialised into a directory under `~/.gitbranchfs/`. Each
mountpoint is keyed by a `<mount-id>` derived from its canonical path, so
the same branch can be mounted at several mountpoints at once, each with
its own dedicated overlay and `.git`. Reads are
resolved against the resolved branch's tree (looked up via libgit2,
creating a new local branch off HEAD when the requested branch is missing)
merged with
the on-disk overlay contents; writes and unlinks go to the overlay only
— the underlying git tree is read-only from inside the mount. This
document gives the layout a future agent needs to find the right module
fast — not a code mirror.

## Directory layout

```
.
├── Makefile                     -- handwritten GNU Make, builds ./gbfs
├── include/                     -- PUBLIC HEADERS (the entire public surface)
│   ├── cli.h                    -- int cli_run(int argc, char **argv);
│   ├── core_fs.h                -- opaque gbfs_state_t + gbfs_* FS op API
│   ├── core_internal.h          -- gbfs_state_t layout + gbfs_file_handle_t
│   │                                (internal; included by src/core_fs.c and
│   │                                 src/core_dir.c, not by external callers)
│   ├── fuse_ops.h               -- int run_fuse_fs(int, char **, gbfs_state_t *);
│   ├── git_wrapper.h            -- libgit2 wrapper (init/stat/get_blob/readdir)
│   ├── overlay.h                -- deleted_list_t + mark/unmark/write_blob
│   ├── settings.h               -- gbfs_settings_t + gbfs_settings_* loading/freeing
│   └── utils.h                  -- path helpers (~ expansion, join, mkdir -p,
│                                  dir/file existence checks)
└── src/
    ├── main.c                   -- 5-line main() that calls cli_run()
    ├── cli.c                    -- two-subcommand argv dispatch (mount/unmount)
    ├── git_wrapper.c            -- libgit2 thin wrapper + the four-level
    │                                branch lookup-or-create (local branch →
    │                                exact ref → DWIM → create off HEAD)
    ├── overlay.c                -- deleted-list file ops + overlay_write_blob
    ├── core_fs.c                -- gbfs_state_t lifecycle + every gbfs_*
    │                                file/dir operation
    ├── core_dir.c               -- the three-source readdir merge
    │                                (git tree ∪ overlay ∪ synthetic .git)
    │                                with deletion filtering
    ├── fuse_ops.c               -- the libfuse3 vtable + the filler_adapter
    │                                shim + run_fuse_fs
    ├── settings.c               -- implementation of include/settings.h (config.json)
    └── utils.c                  -- implementations of include/utils.h
```

There is no `tests/` directory, no `build/` artefact tree, no
`include/gbs/` shadow public-header tree, no `src/cli|src/core|src/fuse|
src/util` sub-hierarchy. Object files live next to their `.c` source
under `src/`.

## Public C API surface

Everything an external translation unit should `#include` lives at the
top level of `include/`. The full list of entry points:

- `int cli_run(int argc, char **argv)` — defined in `src/cli.c`, the
  single function `main()` calls. Two subcommands (`mount` / `unmount`);
  see [components/cli.md](./components/cli.md).
- `gbfs_state_t *gbfs_init(const char *repo_path, const char
  *branch_name, const char *overlay_path)` /
  `void gbfs_destroy(gbfs_state_t *state)` /
  `const char *gbfs_get_resolved_branch(gbfs_state_t *state)` —
  lifecycle + branch-resolution accessor; defined in `src/core_fs.c`.
- FS operations (all in `src/core_fs.c`):
  - `int gbfs_get_attr(state, path, &st)`
  - `int gbfs_read_dir(state, path, buf, filler)` —
    the callback signature is
    `int (*)(void *, const char *, const struct stat *, int64_t)`
    and the real implementation lives in `src/core_dir.c`.
  - `int gbfs_open_file(state, path, flags, &out_fh)`
  - `int gbfs_read_file(state, fh, buf, size, offset)`
  - `int gbfs_write_file(state, fh, buf, size, offset)`
  - `int gbfs_close_file(state, fh)`
  - `int gbfs_create_file(state, path, mode, &out_fh)`
  - `int gbfs_truncate_file(state, path, size)`
  - `int gbfs_unlink_file(state, path)`
  - `int gbfs_make_dir(state, path, mode)`
  - `int gbfs_remove_dir(state, path)`
  - `int gbfs_rename_entry(state, src, dst)`
  - `int gbfs_utimens_file(state, path, ts)`
  - `int gbfs_chmod_file(state, path, mode)`
  - `int gbfs_chown_file(state, path, uid, gid)`
- `int run_fuse_fs(int argc, char **argv, gbfs_state_t *state)` —
  defined in `src/fuse_ops.c`. Wraps `fuse_main(argc, argv, &gbfs_oper,
  state)`.
- libgit2 wrapper (`include/git_wrapper.h` / `src/git_wrapper.c`):
  `git_wrapper_init`, `git_wrapper_cleanup`, `git_wrapper_stat`,
  `git_wrapper_get_blob`, `git_wrapper_readdir` +
  `git_readdir_cb` callback typedef. See
  [components/git-wrapper.md](./components/git-wrapper.md).
- Overlay (`include/overlay.h` / `src/overlay.c`):
  `overlay_load_deleted`, `overlay_free_deleted`,
  `overlay_is_deleted`, `overlay_mark_deleted`,
  `overlay_unmark_deleted`, `overlay_write_blob`. See
  [components/overlay.md](./components/overlay.md).
- Path / FS utilities (`include/utils.h` / `src/utils.c`):
  `resolve_home_path`, `join_paths`, `mkdir_rec`, `dir_exists`,
  `file_exists`. See [components/utils.md](./components/utils.md).
- Settings (`include/settings.h` / `src/settings.c`):
  `gbfs_settings_init`, `gbfs_settings_free`, `gbfs_settings_load`. See [components/settings.md](./components/settings.md).

No libgit2 types (`git_repository`, `git_tree`, `git_blob`, ...) leak
through any header except `git_wrapper.h` itself (which transitively
pulls in `<git2.h>` because the wrapper deals in libgit2 pointers). The
FUSE vtable type comes from libfuse3 in `src/fuse_ops.c` only — no
public header mentions it.

## Build

```
$ make            # builds ./gbfs
$ make clean      # removes src/*.o and ./gbfs
```

That is the full surface area. See
[core/build-system.md](./core/build-system.md) for the Makefile shape:
`wildcard src/*.c` auto-discovery (no Makefile edit required to add a
new source), `pkg-config` guards for libgit2 and libfuse3 (the Makefile
fails fast with a missing-pkg-config diagnostic if either is absent),
`-lpthread` in `LIBS`, `-Wall -Wextra -O2 -std=c11
-D_FILE_OFFSET_BITS=64` in `CFLAGS`, `-Iinclude` in `INCLUDES`. There
is **no `make test`** and no `tests/` directory.

## End-to-end invocation

```
$ cd /path/to/repo
$ gbfs mount mybranch /tmp/gbfs-mp
Initializing GitBranchFS:
  Git Repository: /path/to/repo
  Branch:         mybranch
  Mount Point:    /tmp/gbfs-mp
  Overlay Path:   /home/<user>/.gitbranchfs/<repo-name>/mybranch/<mount-id>
Mounting filesystem...
# ... blocks in foreground FUSE loop; Ctrl-C stops it.
```

In another shell:

```
$ gbfs unmount /tmp/gbfs-mp
Unmounting /tmp/gbfs-mp...
Unmounted successfully.
```

After mount, the mountpoint shows the resolved branch's tree merged
with whatever overlay file bodies sit under
`~/.gitbranchfs/<repo-name>/<branch>/<mount-id>/` minus any paths listed in
that overlay's `deleted` file:

```
<overlay_root>/                    # ~/.gitbranchfs/<repo-name>/<branch>/<mount-id>/
├── deleted                        -- newline list of tombstones
├── <path mirroring mount tree>   -- one file per overlay entry, bytes
│                                     tracked by the user's write/create
│                                     through the mount
└── .git/                          -- synthesised by gbfs_init
    ├── commondir                  -- points back at <repo>/.git
    │                                 (or <repo> for a bare repo), so
    │                                 `git` operations inside the mount
    │                                 resolve against the original repo
    └── HEAD                       -- left empty/default
```

Read paths under the mount are resolved by the matching `gbfs_*`
function: `getattr` / `readdir` consult git via `git_wrapper_stat` /
`git_wrapper_readdir` (or the overlay equivalent), merging deletion
filtering inline. Writes land in the overlay directory via
`overlay_write_blob` (or `gbfs_create_file`'s direct overlay create
path), and unlinks append to `deleted` while also unlinking any
overlay file body.

## Where to start when adding code

1. New libgit2 lookup or tree walk → see
   [components/git-wrapper.md](./components/git-wrapper.md). This is the
   single point where libgit2 is touched; everything else deals in
   `gbfs_state_t` + git_blobs.
2. New overlay-layer behaviour (different on-disk format for the
   deleted list, different write materialisation) → see
   [components/overlay.md](./components/overlay.md). `overlay_*` is
   the entire extension surface, called from `core_fs.c` on every
   unlink / create / truncation / write that needs the overlay.
3. New FS operation in the gbfs API (e.g., a new `gbfs_*` entry point)
   → edit both `include/core_fs.h` and `src/core_fs.c`, then add the
   matching libfuse3 callback to `gbfs_oper` in `src/fuse_ops.c`. The
   FUSE callback is always a one-liner: pull `gbfs_state_t *state`
   from `fuse_get_context()->private_data` and forward to the gbfs
   function. See [components/fuse-ops.md](./components/fuse-ops.md).
4. Changes to `gbfs_read_dir` (the three-source merge) → see
   [components/core-dir.md](./components/core-dir.md). Note that the
   `filler` callback type used here is the project's own
   `(void *, const char *, const struct stat *, int64_t)` — there is
   a `filler_adapter` shim in `fuse_ops.c` that bridges it to
   libfuse3's `fuse_fill_dir_t`.
5. New CLI subcommand, error message, or validation rule → edit
   `src/cli.c`. See [components/cli.md](./components/cli.md) for the
   current argv shape.
6. New path helper → edit both `include/utils.h` and `src/utils.c`;
   the Makefile picks up the new source via `wildcard src/*.c`.
7. Need a new external library → wire its `pkg-config` flags into the
   Makefile (see [core/build-system.md](./core/build-system.md)) and
   document the use in the relevant component doc.
8. Changes to global configuration parsing or default config files → see [components/settings.md](./components/settings.md).
