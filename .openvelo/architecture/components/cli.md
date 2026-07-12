# CLI component

The CLI is the user-facing boundary of gitbranchfs. Only two subcommands
are supported today: `mount <branch-name> <mount-point>` and
`unmount <mount-point>`. The `mount` flow uses the current working
directory as the git repository (verifying it is one via
`git_wrapper_is_repo`), resolves the mount point to absolute form,
derives the per-mountpoint overlay root under
`~/.gitbranchfs/<repo-name>/<branch>/<mount-id>/` (where `<mount-id>` is
`derive_mount_id(mount_path)`, so each mountpoint owns a dedicated overlay and
`.git`), initialises a `gbfs_state_t` via
`gbfs_init`, and hands off to `run_fuse_fs` which blocks inside
`fuse_main`. The `unmount` flow shells out to `fusermount3 -u` via
`fork` + `execv`.

## Key files

| File                | Responsibility                                                                                            |
|---------------------|-----------------------------------------------------------------------------------------------------------|
| `src/main.c`        | Five-line `main()` whose only job is to delegate to `cli_run`. No logic lives here.                      |
| `src/cli.c`         | `cli_run(int argc, char **argv)`: prints application version and author, loads user settings via `gbfs_settings_load` (which auto-creates `config.json` with default settings if not present), then dispatches using an `argv[1]` switch to either `mount` or `unmount`. The `mount` branch reads the repo from `getcwd`, verifies it with `git_wrapper_is_repo`, resolves the mount point, derives the overlay path based on the configured overlay root, prints the resolved paths, calls `gbfs_init`, and forwards to `run_fuse_fs`. The `unmount` branch realpaths the mountpoint (falling back to raw argv if it disappeared), resolves the overlay root for finding pid files (on Windows), and performs unmounting. |
| `include/cli.h`     | Single public prototype: `int cli_run(int argc, char *argv[]);`. |

## Argv shape

There is no argp, no `--json`, no hidden flags, no subcommand parser
beyond `strcmp(argv[1], "mount")` / `strcmp(argv[1], "unmount")`. The
`mount` keyword is **optional**: when `argv[1]` is neither `"mount"` nor
`"unmount"` **and** `argc == 3`, the invocation is treated as an implicit
mount with `branch = argv[1]` and `mount_point = argv[2]`. Any other
unrecognised first token (or a non-3 argc with an unknown token) produces
an "Unknown command" error.

Edge case: a branch literally named `mount` or `unmount` cannot use the
implicit form (`gbfs mount <mp>` is parsed as the `mount` subcommand
missing an argument); use the explicit `gbfs mount mount <mp>` form.

### `gbfs [mount] <branch-name> <mount-point>`

Explicit form (`gbfs mount <branch> <mp>`) requires `argc == 4`; the
implicit form (`gbfs <branch> <mp>`) requires `argc == 3`. In both cases
`branch` / `mount_point` are resolved to the two positional arguments.
Validation order:

1. Validate `branch`: must be non-empty, must not contain
   `/`, must not contain `..`. Any failure prints the corresponding
   `"Error: Branch name must ..."` message and returns 1.
2. `getcwd(repo_path, PATH_MAX)` — the current working directory is the
   git repository. Failure →
   `"Error: Failed to determine the current working directory."` and
   return 1.
3. `git_wrapper_is_repo(repo_path)` — verify the CWD is a git
   repository. Non-zero → `"Error: Current directory '<cwd>' is not a
   git repository."` and return 1.
4. `realpath(mount_point, mount_path)`.
   - If it **fails** (mount point doesn't exist yet), prompt the user
     interactively via `prompt_create_dir`:
     `"Mount point '<mp>' does not exist. Create it? [Y/n] "`. An empty
     answer / EOF / leading `y`/`Y` means yes (the default); a leading
     `n`/`N` means no. If declined → `"Aborted: mount point '<mp>' was
     not created."` and return 1. If accepted → `mkdir_rec(mount_point)`
     (on failure `"Error: Mount point '<mp>' could not be created."`,
     return 1), mark it created, then re-`realpath` for the canonical
     path.
   - If it **succeeds** but the resolved path is not a directory
     (`!dir_exists`), print `"Error: Mount point '<mp>' exists but is
     not a directory."` and return 1.
5. `path_is_within(mount_path, repo_path)` — the mount point must **not**
   be the repository directory itself or a subfolder of it (both compared
   as canonical absolute paths). If it is, print
   `"Error: Mount point '<mp>' must not be inside the git repository
   '<repo>'."` and return 1. If the mount-point directory was created in
   step 4 for this failed attempt, it is `rmdir`-ed to avoid leaving a
   stray directory behind.
6. If the mount point **pre-existed** (was not created in step 4), it must
   be empty: `dir_is_empty(mount_path)` → `0` gives `"Error: Mount point
   '<mp>' is not empty."`; `-1` gives `"Error: Mount point '<mp>' could
   not be read."`; either returns 1. A freshly created directory is empty
   by construction and skips this check.
7. Derive `repo_name` from the last `/`-delimited component of
   `repo_path` (after stripping any trailing `/`). Failure to parse →
   `"Error: Failed to parse repository name."` and return 1.
8. Derive `mount_id = derive_mount_id(mount_path)` (a deterministic,
   filesystem-safe `<basename>-<hash>` token from the canonical mount
   point). Failure → `"Error: Failed to derive mount identifier."` and
   return 1. Build `overlay_root` as
   `"<overlay_root_prefix>/<repo_name>/<branch>/<mount-id>"`, where
   `<overlay_root_prefix>` is `settings.overlay_root` if configured, or
   `~/.gitbranchfs` by default, then `resolve_home_path` to expand `~`
   against the user's home directory.
9. Print the four resolved paths (repo, branch, mount, overlay_root) to
   stdout and call `gbfs_init(repo_path, branch, overlay_path)`. On
   `NULL` return, print
   `"Error: Failed to initialize GitBranchFS state. Check repo path
   and branch name."` and return 1.
10. If `gbfs_get_resolved_branch(state)` is non-NULL **and different
    from** the requested branch, print a resolution note
    (`"  -> NOTE: Resolved to branch: <resolved> (fallback)"`). With the
    current lookup-or-create chain the resolved branch equals the
    requested one (an existing match, or a freshly created branch of the
    same name), so this note is effectively dormant; it would only fire if
    a future lookup rule resolved to a differently named ref.
11. Build `fuse_argv`. On Linux/macOS:
    `{ argv[0], mount_path, "-s", "-odefault_permissions" }`. `"-s"` runs
    libfuse3 single-threaded (safer with libgit2 handles);
    `"-odefault_permissions"` lets the kernel enforce standard permission
    checks against the UIDs/GIDs reported in stat replies (so
    `git_wrapper_stat`'s `st_uid = getuid()` is what the kernel honours).
    On Windows (WinFSP-FUSE3): `{ argv[0], mount_path, "-s", "-ouid=-1",
    "-ogid=-1" }`. WinFSP-FUSE3 silently discards `default_permissions`
    and treats `-ouid=-1`/`-ogid=-1` as a sentinel that resolves to the
    current process's real UID/GID via the security token
    (`fsp_fuse_get_token_uidgid` in winfsp/src/dll/fuse/fuse.c), so files
    are reported owned by the mounting user. The Linux form breaks on
    libfuse3 because `set_stat()` would overwrite every reported UID/GID
    with 4294967295 (nobody/nogroup); the Windows form breaks on libfuse3
    for the same reason. Hence the platform branch.
12. Print `"Mounting filesystem..."` and call
    `run_fuse_fs(fuse_argc, fuse_argv, state)`. The fuse main return
    value is propagated as the CLI's exit code. `gbfs_destroy(state)`
    runs unconditionally on the way out.

### `gbfs unmount <mount-point>`

argc must be at least 3. The mountpoint is realpathed, with fallback to
the raw argv string (so unmount succeeds even after the directory has
been removed). The command `fusermount3 -u "<mount_path>"` is run via
`system(3)`; a zero return prints `"Unmounted successfully."` and exits
0; a non-zero return prints
`"Error: Failed to unmount <mount_path>."` to stderr and exits 1.

## Exit-code contract

| Code | Meaning                                                                                                |
|------|--------------------------------------------------------------------------------------------------------|
| `0`  | Successful mount (returns fuse_main's exit code on the way out, which is normally 0 on graceful unmount). Successful unmount (fusermount3 returned 0). |
| `1`  | Missing/extra argv for the chosen subcommand (also prints `print_usage`). `getcwd` failed ("Failed to determine the current working directory."). Current directory is not a git repository ("Current directory '…' is not a git repository."). Invalid branch name. User declined to create a missing mount point ("Aborted: mount point '…' was not created."). `mkdir_rec` failed ("Mount point '…' could not be created."). Mount point exists but is not a directory ("… exists but is not a directory."). Mount point is inside the repository ("Mount point '…' must not be inside the git repository '…'."). Pre-existing mount point is not empty / unreadable ("… is not empty." / "… could not be read."). Could not derive `repo_name`. `resolve_home_path` returned NULL. `gbfs_init` returned NULL ("Failed to initialize GitBranchFS state."). `fusermount3 -u` exited non-zero. |
| other| `fuse_main`'s exit code propagates on `mount` — typically `0` on graceful unmount. |

## What the CLI does **not** do

The CLI does not take any lockfile, does not write any metadata repo,
does not run a daemon, does not fork+exec, does not consult
`/proc/self/exe`, does not validate the branch name beyond passing it to
`gbfs_init` (which lets `git_wrapper_init`'s four-level lookup-or-create
chain resolve an existing branch or create a new one off HEAD), and does
not emit JSON. There is no `--json` flag, no
`--help`, but it prints a version string on startup. Concurrency control and overlay/deletion
state are owned entirely by `core_fs.c` + `overlay.c` operating on the
per-mountpoint directory under the configured overlay path root.

## State on disk after a successful mount

The mountpoint itself is a normal FUSE mount — there is no
`.gitbranchfs.db*` material inside it. All mutable state lives in the
overlay root, resolved as `<overlay_path_prefix>/<repo-name>/<branch>/<mount-id>/` (default prefix `~/.gitbranchfs` or custom prefix configured in `~/.gitbranchfs/config.json`):

```
<overlay_root>/                    # <overlay_path_prefix>/<repo-name>/<branch>/<mount-id>/
├── deleted                        -- newline list of tombstoned paths
├── <path mirroring mount tree>   -- overlay file bodies written by the user
└── .git/                          -- synthesised by gbfs_init
    ├── commondir                  -- <repo>/.git  (or <repo> if bare)
    └── HEAD                       -- empty / default
```

The `commondir` indirection means `git status`, `git diff`, etc. run
from inside the mount see the original repo's objects directly, while
file content the user modifies through the mount lives in the overlay
and takes precedence over the git tree.
