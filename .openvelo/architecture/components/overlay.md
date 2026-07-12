# Overlay & deletion layer

`include/overlay.h` + `src/overlay.c` materialises the per-mountpoint
overlay state into the filesystem under
`~/.gitbranchfs/<repo-name>/<branch>/<mount-id>/`. Overlay file bodies mirror the
mount's tree layout (one file per overlay entry, at the corresponding
relative path). The deletion state is a single newline-delimited text
file at `~/.gitbranchfs/<repo-name>/<branch>/<mount-id>/deleted`.

This is the entire persistence layer. There is no metadata git repo,
no CBOR, no commit history — just two filesystem primitives (an
arbitrary directory tree for overlay file bodies, plus a flat
newline-list text file for tombstones). The FUSE layer reads from and
mutates this state through the `overlay_*` and `gbfs_*` entry points.

## Key files

| File                    | Responsibility                                                                                              |
|-------------------------|-------------------------------------------------------------------------------------------------------------|
| `include/overlay.h`     | Public prototypes: `overlay_load_deleted`, `overlay_free_deleted`, `overlay_is_deleted`, `overlay_mark_deleted`, `overlay_unmark_deleted`, `overlay_write_blob`. Also declares `deleted_list_t` (heap-allocated `char **paths`, `count`, `capacity`). |
| `src/overlay.c`         | Implementation. On-disk format is one relative path per line in the `deleted` file (no escaping, no quoting, trailing newline optional). `overlay_write_blob` materialises a `git_blob` as a file under the overlay at the corresponding relative path, `mkdir -p`-style for intermediate directories. |
| `src/core_fs.c`         | The only consumer. Loads a `deleted_list_t` at `gbfs_init` time; checks it inside every read-path op (and `gbfs_create_file`); mutates it inside unlink / create / truncate flows; calls `overlay_write_blob` from `gbfs_open_file`'s overlay branch and from `gbfs_create_file`'s initial write. |

## Data structures

```c
typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
} deleted_list_t;
```

A flat heap-allocated array of `strdup`'d relative paths. Functions
that mutate (`_mark_deleted`, `_unmark_deleted`) write the list back
to disk atomically after the in-memory change. `overlay_load_deleted`
returns `NULL` (and `count == 0`) when the file does not exist — an
absent `deleted` file is the "no tombstones" initial state.

## On-disk format

```
<overlay_root>/                 # ~/.gitbranchfs/<repo>/<branch>/<mount-id>/
├── deleted                     # one relative path per line, '\n'-terminated
├── <path a>/                    # one file per overlay entry, mirroring
│   └── <path b>                   the mount's tree; intermediate
└── .git/                          directories created as needed
    ├── commondir
    └── HEAD
```

A path is recorded by **mount-relative** form (the same string the
FUSE layer passes via `path` in callbacks). Whitespace and embedded
newlines inside a path are not supported — the on-disk format treats
`\n` as the separator.

## Operations

- `deleted_list_t *overlay_load_deleted(const char *overlay_root)` —
  reads `<overlay_root>/deleted`, splitting on `'\n'`, dropping
  trailing empty lines. Returns `NULL` if the file does not exist
  (treated as zero entries). Each path is `strdup`'d into the
  `paths[]` array.
- `void overlay_free_deleted(deleted_list_t *dl)` — frees each
  `paths[i]` plus the `paths` array plus the `deleted_list_t`
  struct itself. Safe on `NULL` (no-op).
- `int overlay_is_deleted(deleted_list_t *dl, const char *path)` —
  linear scan of `dl->paths[]` comparing against `path` with
  `strcmp`. Returns 1 if found, 0 otherwise. Every read-path op
  (`gbfs_get_attr`, `gbfs_read_dir`'s filter loop, `gbfs_open_file`)
  consults this before resolving through the git tree.
- `int overlay_mark_deleted(const char *overlay_root,
  deleted_list_t *dl, const char *path)` — appends `path` to
  `dl->paths` (growing the array with `realloc` and a doubled
  capacity strategy) if not already present, then rewrites the
  on-disk `deleted` file. Returns 0 on success, non-zero on
  failure.
- `int overlay_unmark_deleted(const char *overlay_root,
  deleted_list_t *dl, const char *path)` — removes `path` from
  `dl->paths` (shifting subsequent entries down) and rewrites the
  `deleted` file. Called when a user creates a file with a path
  previously marked deleted — the new content takes precedence.
- `int overlay_write_blob(const char *overlay_root, const char
  *path, git_blob *blob, mode_t mode)` — writes the blob's bytes to
  `<overlay_root>/<path>`, creating any intermediate directories via
  the same `mkdir_rec`-style recursion used elsewhere in the
  project. `mode` is applied via `chmod(2)` so the overlay file's
  permission bits match what the rest of the mount sees.

## Write semantics — what actually changes on disk

When a user creates or modifies a file through the mount:

1. `gbfs_create_file` (or the overlay branch of `gbfs_open_file`)
   writes bytes into the overlay file at the mount-relative path.
2. If the path was previously marked deleted (which is the common
   case after a `gbfs_unlink_file` + later re-create), the
   `deleted_list_t` has that path removed via
   `overlay_unmark_deleted` and the on-disk `deleted` file is
   rewritten without it.
3. If the path is brand new, `overlay_mark_deleted` is **not**
   called — `deleted` stays unchanged.

When a user unlinks a file:

1. `gbfs_unlink_file` removes the overlay file body (if any) at the
   mount-relative path via `unlink(2)`.
2. `overlay_mark_deleted(<overlay_root>, &state->deleted, path)`
   appends the path to `dl->paths` (if not already there) and
   rewrites the `deleted` file.

When a user renames a path (`gbfs_rename_entry`):

- Renaming an overlay file means moving the file body inside the
  overlay directory via `rename(2)`. The `deleted` list is **not**
  touched — the new path was never deleted, and the old path is
  consumed by the rename.
- If the new path was previously marked deleted, that tombstone is
  removed via `overlay_unmark_deleted` so subsequent reads do not
  silently `ENOENT` the freshly-named file.

## Concurrency

There is no in-process mutex. The FUSE layer runs single-threaded
(argv includes `"-s"`), so the C-level `deleted_list_t` is safe to
mutate without locking. Each mountpoint now gets its own
`<overlay_root>` (keyed by `<mount-id>`), so mounting the same branch at
distinct mountpoints is isolated; what remains unserialised is two
concurrent `gbfs` processes pointed at the **same** `<overlay_root>`
(i.e. the same mountpoint) — mount each mountpoint at most once. (See
the project's open work / non-goals section for notes on adding a pid
lockfile back.)

## Integration points

- `src/core_fs.c:gbfs_init` — `overlay_load_deleted(overlay_path)`
  populates `state->deleted`.
- `src/core_fs.c:gbfs_destroy` — `overlay_free_deleted(state->deleted)`.
- `src/core_fs.c:gbfs_get_attr`, `gbfs_read_dir` — consult
  `overlay_is_deleted` on every read.
- `src/core_fs.c:gbfs_open_file`, `gbfs_create_file` — write through
  `overlay_write_blob`.
- `src/core_fs.c:gbfs_unlink_file` — `overlay_mark_deleted` +
  `unlink(2)`.
- `src/core_fs.c:gbfs_rename_entry` — `rename(2)` inside the
  overlay; possibly `overlay_unmark_deleted` if the destination
  was previously tombstoned.
- `include/utils.h:mkdir_rec` — used inside `overlay_write_blob`
  to create intermediate directories.
- `Makefile` — `src/overlay.c` is picked up by `wildcard src/*.c`.
