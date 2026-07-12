# Directory merging

`src/core_dir.c` owns everything directory-shaped: the readdir merge
plus the small set of directory-only operations
(`gbfs_make_dir`, `gbfs_remove_dir`, `gbfs_rename_entry`,
`gbfs_unlink_file`). It depends on the project-local
`dir_entries` dedup helper and on `git_wrapper_readdir` /
`overlay_is_deleted` / `overlay_mark_deleted` /
`overlay_unmark_deleted` / `overlay_write_blob` /
`git_wrapper_stat`.

`gbfs_read_dir` is implemented here but its prototype lives in
`include/core_fs.h` (it's part of the gbfs_* public surface).

## Key files

| File             | Responsibility                                                                                              |
|------------------|-------------------------------------------------------------------------------------------------------------|
| `src/core_dir.c` | All directory operations. The `dir_entries` dedup helper, `git_readdir_callback` adapter (translates libgit2's `(name, oid, filemode)` callback shape into the project's `(name, "")` shape), `gbfs_read_dir`, `gbfs_make_dir`, `is_dir_empty`, `gbfs_remove_dir`, `copy_git_dir_rec`, `gbfs_rename_entry`, `gbfs_unlink_file`. |

## `dir_entries`

Tiny helper used to deduplicate names across the merge sources:

```c
struct dir_entries { char **names; size_t count; size_t capacity; };
static void add_dir_entry(struct dir_entries *de, const char *name);
```

Doubles capacity (`16 → 32 → ...`) via `realloc`, does a linear scan
to skip duplicates already added, `strdup`s the name into the next
slot. The caller frees each `names[i]` then `names` once consumed.

## `git_readdir_callback`

Adapter between libgit2's `git_readdir_cb` (the typedef in
`include/git_wrapper.h`) and `dir_entries`:

```c
static int git_readdir_callback(void *buf, const char *name,
                                const git_oid *oid, unsigned int filemode) {
    (void)oid; (void)filemode;
    add_dir_entry((struct dir_entries *)buf, name);
    return 0;
}
```

The libgit2 oid and filemode are deliberately ignored — readdir
listings in this project care only about names.

## `gbfs_read_dir` algorithm

Signature (declared in `include/core_fs.h`):

```c
int gbfs_read_dir(gbfs_state_t *state, const char *path,
                  void *buf,
                  int (*filler)(void *, const char *,
                                const struct stat *, int64_t));
```

Steps:

1. `overlay_is_deleted(state->deleted, path)` → `-ENOENT`. (A
   tombstoned directory is invisible — there is no
   `ENOTDIR`-vs-`ENOENT` distinction at this layer.)
2. Allocate a `dir_entries de` on the stack.
3. **Source 1 — git tree**: `git_wrapper_readdir(state->repo,
   state->tree, path, &de, git_readdir_callback)` invokes
   `git_readdir_callback` once per direct child of the resolved
   tree at `path`.
4. **Source 2 — overlay directory**: `opendir(<overlay_path><path>)`
   and `readdir`, skipping `.`, `..`, and the internal name
   `.gitbranchfs_metadata`. Each surviving `d_name` is added via
   `add_dir_entry`. (An `opendir` failure is silently ignored —
   the overlay subdirectory simply doesn't exist yet.)
5. **Source 3 — synthetic `.git` at the root**: if `path == "/"`
   then `add_dir_entry(&de, ".git")`.
6. **Standard dot entries**: `filler(buf, ".", NULL, 0)` and
   `filler(buf, "..", NULL, 0)`.
7. **Final loop**: for each name in `de.names[]`:
   - Build `entry_path = join_paths(path, name)` (mount-relative
     absolute form).
   - If `overlay_is_deleted(state->deleted, entry_path)` → skip.
   - Otherwise call `gbfs_get_attr(state, entry_path, &st)` to fill
     a `struct stat` (which itself does the overlay-vs-tree merge)
     and `filler(buf, name, &st, 0)`.
   - Free `entry_path`, free `de.names[i]`.
8. `free(de.names)`, return 0.

Note that the synthetic dot entries are emitted **before** the
deletion filter runs, but `.` and `..` are never in the deleted
list in practice.

## `gbfs_make_dir(path, mode)`

1. `overlay_unmark_deleted(state->overlay_path, state->deleted, path)`
   — directories don't live in the deleted list, but unmarking here
   keeps the invariant simple should a path get renamed.
2. `local_path = join_paths(state->overlay_path, path)`.
3. `mkdir_rec(local_path)`. The `mode` argument is currently
   ignored (the `(void)mode;` cast at the top of the function
   signals this — future enhancement can plumb mode through
   `mkdir_rec`).
4. Returns `-errno` on `mkdir_rec` failure, otherwise 0.

## `gbfs_remove_dir(path)`

Refuses with `-ENOTEMPTY` if the directory has any visible children
(either git tree children minus deletions, or overlay directory
children minus deletions — see `is_dir_empty` below). Then:

1. Build `local_path = join_paths(state->overlay_path, path)`.
2. Decide `in_overlay = dir_exists(local_path)` and `in_git =
   (git_tree_entry_bypath(tree, path_without_leading_slashes) == 0
   && entry type == GIT_OBJECT_TREE)`.
3. `-ENOENT` if neither is true.
4. `rmdir(local_path)` if in_overlay, recording `-errno` in `err`.
5. If `err == 0 && in_git` → `overlay_mark_deleted(state->overlay_path,
   state->deleted, path)`.

The git tree is read-only; removing a directory does **not** unlink
the tree entry. It only tombstones the path so future reads treat it
as deleted, plus unlinks any overlay copy.

## `gbfs_rename_entry(src, dst)`

`src/core_dir.c:250`. The most involved of the dir ops:

1. Refuse if `src` is in the deleted list (`-ENOENT`).
2. Resolve `src_in_overlay = file_exists(local_src) ||
   dir_exists(local_src)` and
   `src_in_git = git_tree_entry_bypath(tree, src) == 0`. `-ENOENT`
   if neither.
3. Build `mkdir_rec`-style the parent of `local_dst` so the
   destination directory chain exists.
4. Three branches:
   - **`src_in_overlay` only**: `rename(local_src, local_dst)`. No
     deletion-list mutation; the file (or directory) was already
     outside the git tree.
   - **`src_in_git`, regular file**: `git_wrapper_get_blob` →
     `overlay_write_blob(<overlay_root>, dst, blob, mode)` to
     materialise the blob at the new path; `git_blob_free`.
   - **`src_in_git`, directory**: `mkdir_rec(local_dst)`, then
     `copy_git_dir_rec(state, src, dst)` recursively writes every
     child entry into the overlay at the new path (`mkdir_rec` for
     sub-dirs, `git_wrapper_get_blob` + `overlay_write_blob` for
     sub-files).
5. On success, if `src_in_git` → `overlay_mark_deleted(state->overlay_path,
   state->deleted, src)`, and `overlay_unmark_deleted` on `dst`.

## `gbfs_unlink_file(path)`

`src/core_dir.c:319` — the file-unlink counterpart of
`gbfs_remove_dir`. Pattern is identical: `in_overlay` /
`in_git` probe (`git_tree_entry_bypath`), `-ENOENT` if neither,
`unlink(local_path)` if `in_overlay`, `overlay_mark_deleted` if
`in_git`.

## Why this lives in its own translation unit

`core_dir.c` keeps the `dir_entries` and `git_readdir_callback`
helpers private and groups every directory-shaped op together so that
the recursive `copy_git_dir_rec` + `is_dir_empty` interplay with the
overlap between tree entries and overlay entries stays in one place.
`core_fs.c` instead focuses on per-file operations and the state
lifecycle, both of which are far more linear.

## Integration points

- `src/core_fs.c:gbfs_get_attr` — the deletion-check + `lstat` +
  `git_wrapper_stat` fallback is mirrored by `gbfs_read_dir`'s
  per-entry filter (`gbfs_get_attr` is called once per entry to
  produce the `struct stat` passed to the FUSE filler).
- `src/fuse_ops.c:gbfs_fuse_readdir` — the filler callback is the
  `filler_adapter` shim in `fuse_ops.c`, which translates the
  `(void *, const char *, const struct stat *, int64_t)` signature
  the gbfs API uses into libfuse3's
  `fuse_fill_dir_t(void *, const char *, const struct stat *,
  off_t, unsigned)` shape.
- `src/overlay.c` — `overlay_is_deleted` / `_mark_deleted` /
  `_unmark_deleted` / `_write_blob` from the same module.
- `Makefile` — auto-discovery picks up `src/core_dir.c`.
