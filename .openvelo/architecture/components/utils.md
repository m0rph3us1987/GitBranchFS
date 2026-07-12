# Path and filesystem utilities

`include/utils.h` + `src/utils.c` provide the small bag of
path-and-fs helpers every other component leans on:
`resolve_home_path` (`~` expansion), `join_paths` (slash-canonical
path concatenation), `mkdir_rec` (`mkdir -p` equivalent),
`dir_exists`, `file_exists`, and `derive_mount_id` (deterministic
per-mountpoint overlay identifier).

## Key files

| File              | Responsibility                                                                                              |
|-------------------|-------------------------------------------------------------------------------------------------------------|
| `include/utils.h` | Public prototypes: `resolve_home_path`, `join_paths`, `mkdir_rec`, `dir_exists`, `file_exists`, `derive_mount_id`.            |
| `src/utils.c`     | Implementation. Defines `_GNU_SOURCE` at the top of the TU for `getpwuid_r`-style helpers (specifically `getpwuid` for the `HOME` fallback inside `resolve_home_path`). |
| `Makefile`        | Auto-discovery picks up `src/utils.c`. |

## `resolve_home_path(const char *path)`

If `path` starts with `~`, expand it against the user's home
directory:

1. Try `getenv("HOME")` first.
2. If that fails, fall back to `getpwuid(getuid())->pw_dir`.
3. If both fail, treat home as the empty string.
4. Allocate `home_len + (strlen(path) - 1) + 1`, copy `home` then
   `path + 1` (i.e., the rest of `path` after the leading `~`).
5. Return the new heap string. Caller `free`s.

If `path` does not start with `~`, return `strdup(path)`. `NULL` in
→ `NULL` out.

Used in `src/cli.c` to turn `"~/.gitbranchfs/<repo>/<branch>/<mount-id>"`
into the absolute overlay root path before calling `gbfs_init`.

## `join_paths(const char *base, const char *relative)`

Slashes are normalised and directory traversal segments (`.` and `..`) are collapsed. It ensures that the resolved path does not escape the `base` directory boundary:

1. `NULL` in for either → `NULL` out.
2. Invokes `sanitize_path` to safely join the paths, collapse any relative directory segments, and verify that the result remains within the `base` directory.
3. If path sanitization succeeds, returns a heap-allocated string of the resolved path; otherwise returns `NULL`.

Used pervasively throughout `core_fs.c` and `core_dir.c` to build
absolute paths into the overlay directory (e.g.,
`join_paths(state->overlay_path, "/some/path")` →
`"<overlay_root>/some/path"`).

## `mkdir_rec(const char *path)`

`mkdir -p` equivalent:

1. `strdup` the input (mutated by the algorithm).
2. Strip trailing slashes, ignoring a sole `/`.
3. Walk character by character; every time a `/` is hit, replace it
   with `\0`, `mkdir(copypath, 0755)` (ignoring `EEXIST`), restore
   the `/`.
4. Final `mkdir(copypath, 0755)` (ignoring `EEXIST`).
5. Return 0 on success, `-1` on `mkdir` failure other than `EEXIST`.
   OOM inside `strdup` → `-1` as well.

Used in `gbfs_init` to ensure the overlay root exists, in
`overlay_write_blob` to create intermediate directories for new
overlay file bodies, in `gbfs_create_file` to create the
file's parent directory, and in `gbfs_make_dir` /
`gbfs_rename_entry` / `gbfs_remove_dir` for the matching mkdir
operations.

## `dir_exists(const char *path)`

`stat(path, &st)`; return `S_ISDIR(st.st_mode)` on success, 0
otherwise (treats every error — `ENOENT`, `EACCES`, ... — as
"doesn't exist as a directory").

Used by `gbfs_remove_dir` to decide whether the path is in the
overlay before `rmdir`'ing it.

## `file_exists(const char *path)`

`stat(path, &st)`; return 1 on success, 0 otherwise. Returns true
for directories and symlinks too — callers that need strict
file-only checks should follow up with `S_ISREG`.

Used by `gbfs_open_file` (overlay-hit branch), `gbfs_unlink_file`
(`in_overlay` decision), `gbfs_create_file`'s parent-directory
construction, `gbfs_truncate_file`'s promotion-from-tree flow,
`gbfs_chmod_file` / `gbfs_chown_file` / `gbfs_utimens_file` (overlay
or git-tree sourcing), and `gbfs_rename_entry` (`src_in_overlay`
decision).

## `derive_mount_id(const char *mount_path)`

Produces the deterministic, filesystem-safe identifier that keys each
mountpoint's dedicated overlay directory (`~/.gitbranchfs/<repo>/<branch>/<mount-id>/`):

1. Compute a 64-bit FNV-1a hash over the full canonical `mount_path`.
   This is the stable component — the same mountpoint always hashes to
   the same value, so remounting resumes its overlay, while distinct
   mountpoints (even for the same branch) diverge.
2. Extract the mountpoint basename (ignoring trailing slashes) and
   sanitize it to `[A-Za-z0-9._-]`, replacing any other byte with `_`
   (empty basename falls back to `mnt`).
3. Return a freshly-allocated `"<sanitized-basename>-<16-hex-hash>"`
   string, e.g. `gbfs-1a2b3c4d5e6f7a8b`. Caller `free`s. `NULL`/empty
   input or OOM → `NULL`.

Used in `src/cli.c:cli_run` (after `realpath` resolves `mount_path`) to
build the overlay relative path. Because `core_fs.c` synthesises the
overlay `.git` (`commondir`, `HEAD`, `index`) entirely from
`state->overlay_path`, keying that path per-mountpoint automatically
gives every mount its own dedicated `.git` database.

## Integration points

- `src/cli.c:cli_run` — uses `derive_mount_id` (to key the overlay by
  mountpoint) and `resolve_home_path` (after building
  `~/.gitbranchfs/<repo>/<branch>/<mount-id>`) and `mkdir_rec` (to bring up
  the mountpoint if it does not exist).
- `src/core_fs.c` — uses `join_paths` on every overlay-path lookup
  (read / write / metadata-modify paths), `mkdir_rec` to bring up
  the overlay root inside `gbfs_init` and intermediate directories
  inside `gbfs_create_file`, plus `file_exists` for promotion
  decisions.
- `src/core_dir.c` — uses `dir_exists` (`gbfs_remove_dir`,
  `gbfs_rename_entry`); `file_exists` (`gbfs_unlink_file`,
  `gbfs_rename_entry`).
- `src/overlay.c:overlay_write_blob` — uses `mkdir_rec` to create
  the intermediate directory chain for the overlay file body.
- `Makefile` — `wildcard src/*.c` picks up `src/utils.c`.
