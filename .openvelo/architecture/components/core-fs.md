# Core FS operations

`include/core_fs.h` + `include/core_internal.h` + `src/core_fs.c` own
the project's central state abstraction, `gbfs_state_t`, and every
operation the FUSE layer routes through (file I/O, directory merging,
overlay/deletion mutators, and per-file chmod/chown/utimens/truncate).

## Key files

| File                       | Responsibility                                                                                              |
|----------------------------|-------------------------------------------------------------------------------------------------------------|
| `include/core_fs.h`        | Public header. Declares the opaque `gbfs_state_t` typedef and the full `gbfs_*` FS-operation API (`gbfs_init`/`_destroy`/`_get_resolved_branch` + the 15 `gbfs_*` ops listed below). |
| `include/core_internal.h`  | Internal header (included by `core_fs.c` and `core_dir.c` only). Lays out the `gbfs_state_t` struct (paths, libgit2 handle + tree pointer, deleted list, default-time) and the `gbfs_file_handle_t` shape (is_overlay flag + fd-or-blob + virtual_data buffer). |
| `src/core_fs.c`            | `gbfs_init` / `gbfs_destroy` / `gbfs_get_resolved_branch`. Also `gbfs_get_attr`, `gbfs_open_file` / `gbfs_read_file` / `gbfs_write_file` / `gbfs_close_file` / `gbfs_create_file` / `gbfs_truncate_file` / `gbfs_utimens_file` / `gbfs_chmod_file` / `gbfs_chown_file`. |
| `src/core_dir.c`           | All directory-level ops: `gbfs_read_dir` (delegates here) / `gbfs_make_dir` / `gbfs_remove_dir` / `gbfs_rename_entry` / `gbfs_unlink_file` + helpers `check_dir_empty_cb` / `is_dir_empty` / `copy_git_dir_rec`. |
| `Makefile`                 | Auto-discovery picks up `src/core_fs.c` and `src/core_dir.c`. No per-TU compile flags beyond the standard `-Iinclude -Wall -Wextra -O2 -std=c11 -D_FILE_OFFSET_BITS=64`. |

## `gbfs_state_t` (`include/core_internal.h`)

```c
struct gbfs_state {
    char              *repo_path;       // strdup'd absolute path
    char              *branch_name;     // strdup'd requested branch
    char              *resolved_branch; // strdup'd from git_reference_shorthand
    char              *overlay_path;    // strdup'd overlay root
    git_repository    *repo;            // from git_wrapper_init
    git_tree          *tree;            // from git_wrapper_init
    deleted_list_t    *deleted;         // loaded from overlay_root/deleted
    time_t             default_time;    // time(NULL) at init, used for tree stat
};
```

`gbfs_init` allocates this struct, `gbfs_destroy` frees it.

## `gbfs_file_handle_t`

```c
typedef struct {
    int     is_overlay;     // 1 -> use fd; 0 -> use blob (or virtual_data)
    int     fd;             // valid when is_overlay == 1
    git_blob *blob;         // valid when is_overlay == 0 and virtual_data == NULL
    char    *virtual_data;  // heap "gitdir: ..." buffer for the synthetic /.git
    size_t   virtual_size;
} gbfs_file_handle_t;
```

Allocated by `gbfs_open_file` or `gbfs_create_file` and freed in
`gbfs_close_file`. Three modes: an overlay `fd` (writable), a
read-only `git_blob` (from the resolved tree), or a heap buffer
holding the synthetic `"gitdir: <overlay_root>/.git\n"` content for
the mount's synthetic `/.git` entry.

## `gbfs_init` lifecycle (`src/core_fs.c:13`)

1. `malloc` the struct, fill in the defaults, `strdup` the three
   string inputs (`repo_path`, `branch_name`, `overlay_path`),
   capture `default_time = time(NULL)`.
2. Call `git_wrapper_init(repo_path, branch_name, &state->repo,
   &state->tree, &state->resolved_branch)`. On any negative return,
   free the strdups + the state and return NULL.
3. `state->deleted = overlay_load_deleted(overlay_path)` — a missing
   `deleted` file yields `NULL` + `count == 0`, which is the initial
   "no tombstones" state.
4. `mkdir_rec(overlay_path)` — idempotent.
5. **Synthesise the overlay's `.git/` folder.** This is how `git`
   operations inside the mount resolve against the original repo:
   - Build `<overlay_root>/.git`, `mkdir_rec` it.
   - Write `<overlay_root>/.git/commondir` containing
     `git_repository_is_bare(state->repo) ? <repo> : <repo>/.git\n`
     so libgit2 finds the objects and refs of the **original** repo.
   - Write `<overlay_root>/.git/HEAD` as
     `ref: refs/heads/<resolved_branch>\n`.
   - If `<overlay_root>/.git/index` does not yet exist,
     `git_index_open` → `git_index_read_tree(idx, state->tree)` →
     `git_index_write` → `git_index_free` so `git status` /
     `git diff` immediately have a populated index from the
     resolved tree.
6. Return the state.

`gbfs_destroy` reverses the order: `overlay_free_deleted`,
`git_wrapper_cleanup`, then `free` each strdup and the struct.

## File operations — read

### `gbfs_get_attr(path, &st)`

1. `overlay_is_deleted(state->deleted, path)` → `-ENOENT`.
2. **Synthetic `/.git`**: always returned with `S_IFREG | 0644` and a
   `st_size` equal to `strlen("gitdir: ") + strlen(overlay_path) +
   strlen("/.git\n")`. Reads return the synthetic buffer through
   `gbfs_open_file`'s `virtual_data` arm.
3. Otherwise `lstat(<overlay_path><path>)`; if that succeeds and the
   leaf name is **not** `.gitbranchfs_metadata` (an internal name
   hidden by `gbfs_read_dir`), return the stat. This is the "overlay
   wins over git tree" branch — a file present in the overlay is the
   only thing the user sees, even if the git tree also has it.
4. Fallback: `git_wrapper_stat(state->repo, state->tree, path, &st,
   state->default_time)`. Negative result → `-ENOENT`.
5. Names whose last component is `.gitbranchfs_metadata` are filtered
   (returns `-ENOENT`) even when present on disk.

### `gbfs_open_file(path, flags, &out_fh)`

1. `overlay_is_deleted(state->deleted, path)` → `-ENOENT`.
2. **`/.git`**: writes refused with `-EACCES`; reads get a
   `virtual_data` buffer holding `"gitdir: <overlay_path>/.git\n"`,
   `is_overlay = 0`, `fd = -1`, `blob = NULL`.
3. **Overlay hit**: if `<overlay_path><path>` exists on disk, `open`
   it with the requested flags; populate `is_overlay = 1`, `fd`,
   `blob = NULL`.
4. **First-write promotion from tree**: if the flags include
   `O_WRONLY` / `O_RDWR` / `O_TRUNC` / `O_CREAT` **and** the path
   exists in the git tree but not in the overlay, the tree blob is
   `git_wrapper_get_blob`'d, materialised via `overlay_write_blob`
   (preserving the tree's mode bits), and the resulting fd is
   handed back. This is what makes `echo > existing-file-in-tree.txt`
   create a writable overlay copy on first touch.
5. **Read-only tree path**: hand back a `gbfs_file_handle_t` with
   `is_overlay = 0` and the `git_blob *` owned by the handle.

### `gbfs_read_file(fh, buf, size, offset)`

- `virtual_data` → `memcpy` from the heap buffer; clamped to
  `virtual_size`.
- `is_overlay` → `pread(fd, ...)`.
- Otherwise → `memcpy` from `git_blob_rawcontent(blob)`, clamped to
  `git_blob_rawsize(blob)`.

### `gbfs_close_file(fh)`

Frees `virtual_data`, closes `fd`, or `git_blob_free(blob)`, then
`free(handle)`. Errors are not surfaced (always returns 0).

## File operations — write/mutate

### `gbfs_create_file(path, mode, &out_fh)`

1. `overlay_unmark_deleted(overlay_path, state->deleted, path)` —
   resurrecting a tombstoned path clears it.
2. Build parent directory with `mkdir_rec`-on-the-father (string
   surgery: `strrchr(local_path, '/')`, replace with `'\0'`,
   `mkdir_rec`, restore).
3. `open(local_path, O_CREAT | O_RDWR | O_TRUNC, mode)` →
   `gbfs_file_handle_t` with `is_overlay = 1`. Returns the fd cast
   through `void *`.

### `gbfs_truncate_file(path, size)`

`ENOENT` if the path is deleted. If the file isn't already in the
overlay but is in the git tree, first promote via `git_wrapper_get_blob`
+ `overlay_write_blob` (preserving the tree's mode), then
`truncate(local_path, size)`.

### `gbfs_write_file`

`pwrite(fd, buf, size, offset)` straight through the overlay `fd`.
Refused with `-EBADF` on a non-overlay handle (i.e., the user tried to
write through a read-only `git_blob`-backed handle, which should not
happen because `gbfs_open_file` always promotes tree-backed writes to
the overlay on first touch).

### `gbfs_utimens_file` / `gbfs_chmod_file` / `gbfs_chown_file`

All three follow the same promotion pattern: if the path isn't already
in the overlay, look it up in the git tree — for files, materialise via
`overlay_write_blob` with the appropriate mode; for directories, fall
back to `mkdir_rec`. Then run `utimensat(AT_FDCWD, local_path, ts,
AT_SYMLINK_NOFOLLOW)` / `chmod` / `lchown` against the local path.
Return `-errno` on libc failure; `-ENOENT` if the path is not in the
tree either.

### `gbfs_unlink_file(path)`

1. Compute `local_path = overlay_path/<path>`.
2. Compute `in_overlay = file_exists(local_path)` and
   `in_git = (git_tree_entry_bypath(tree, path) == 0)`.
3. If neither → `-ENOENT`.
4. If `in_overlay` → `unlink(local_path)` (preserving `-errno`).
5. If `in_git` → `overlay_mark_deleted(overlay_path, state->deleted,
   path)`. (The git tree is not mutated; subsequent reads treat the
   path as deleted.)

### `gbfs_rename_entry(src, dst)`

- Refuse early if `src` is in the deleted list.
- Decide whether `src` exists in the overlay, the git tree, or both
  (via `git_tree_entry_bypath`).
- Ensure the destination parent directory exists in the overlay
  (`mkdir_rec` on the path's parent).
- Three branches:
  - `src` is only in the overlay → `rename(local_src, local_dst)`;
    no deletion-list mutation.
  - `src` is a regular file in the tree only → materialise the blob
    at `dst` via `overlay_write_blob`. Then mark `src` deleted and
    unmark `dst` if it was previously tombstoned.
  - `src` is a directory in the tree only → `mkdir_rec` `dst`, then
    `copy_git_dir_rec(state, src, dst)` recursively writes every
    subfile/subdir blob into the overlay at the new path. Then mark
    `src` deleted and unmark `dst` if necessary.
- Returns 0 on success, `-errno` on libc failure, `-ENOMEM` on bad
  allocation.

## `gbfs_get_resolved_branch`

Trivial accessor: returns the `resolved_branch` field set by
`git_wrapper_init` (or `NULL` if `state` is `NULL`). Used by
`src/cli.c` to print the resolution notice; with the current
lookup-or-create chain the resolved branch matches the requested name
(an existing branch or a freshly created one off HEAD).

## Error semantics

All operations follow the libfuse3 convention: return a **negative
errno** (`-ENOENT`, `-EACCES`, `-ENOMEM`, `-errno`, ...) on failure.
The FUSE callback layer propagates the negation. Success codes for
read/write ops are the byte count returned (a positive `ssize_t`,
which FUSE interprets as the count served).

## Integration points

- `src/cli.c:cli_run` — calls `gbfs_init` to bootstrap the state,
  then `run_fuse_fs(argc, argv, state)` which in turn invokes the
  `gbfs_oper` callbacks (see [components/fuse-ops.md](./fuse-ops.md))
  that forward into these `gbfs_*` entry points.
- `src/fuse_ops.c:run_fuse_fs` — `fuse_main` only knows `gbfs_oper`
  and `state`; every callback pulls `state` from
  `fuse_get_context()->private_data`.
- `src/git_wrapper.c` — every `git_wrapper_*` consumer is in this
  file; read it to understand the libgit2 side of the operations.
- `src/core_dir.c` — the directory merging logic, called from
  `gbfs_read_dir`'s seam in `core_fs.c`. `gbfs_make_dir` /
  `_remove_dir` / `_rename_entry` / `_unlink_file` live there so that
  the directory operations keep their dependency on the
  `dir_entries` dedup helper local to one translation unit.
- `src/overlay.c` — `overlay_is_deleted` / `_mark_deleted` /
  `_unmark_deleted` / `_write_blob` from `core_fs.c` and
  `core_dir.c`.
- `Makefile` — auto-discovery, no special flags.
