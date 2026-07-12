# gitbranchfs Architecture

gitbranchfs is a single-process C11 FUSE filesystem that exposes the
contents of a single git branch as a mountable directory, with overlay
and deletion state materialised into a per-mountpoint directory
under `~/.gitbranchfs/<repo-name>/<branch>/<mount-id>/` (the `<mount-id>` is
derived from the canonical mount point, so each mountpoint owns a dedicated
overlay and `.git`). The CLI takes
`gbfs [mount] <branch-name> <mount-point>` (the `mount` keyword is
optional — two positional args default to a mount) and uses the current
working directory as the git repository (erroring out if it is not one),
initialises an internal `gbfs_state_t` (resolving the branch against
the repo via libgit2, creating a new local branch off HEAD when the branch
isn't found),
and runs `fuse_main` in-process. The FUSE read path routes every
`getattr`/`readdir`/`open`/`read` callback through the same `gbfs_*`
helpers, merging the git tree against the on-disk overlay and filtering
out names in the deleted list. The write path materialises modified or
newly created file bodies into the overlay directory (not into the git
tree) and mutates the deleted list for unlinks; truncates, chmod, chown
and utimens operate directly on the overlay file. `gbfs unmount
<mount-point>` shells out to `fusermount3 -u`.

This index is the entry point for the Planner and Reviewer agents. Open
only the domain docs relevant to the task at hand.

## Domain map

| Domain                       | Doc                                                | Summary                                                                                                              |
|------------------------------|----------------------------------------------------|----------------------------------------------------------------------------------------------------------------------|
| Top-level project layout     | [overview.md](./overview.md)                       | Directory map, end-to-end invocation, public API surface, build entry points.                                        |
| Command-line interface       | [components/cli.md](./components/cli.md)           | `gbfs [mount] <branch> <mountpoint>` (repo = current working directory; `mount` keyword optional) and `gbfs unmount <mountpoint>`. arg validation (git-repo check / realpath / mkdir_rec / overlay-path derivation), exit-code contract, hand-off to `gbfs_init` + `run_fuse_fs`. |
| libgit2 wrapper              | [components/git-wrapper.md](./components/git-wrapper.md) | `git_wrapper_init` / `_cleanup` / `_stat` / `_get_blob` / `_readdir` + the `git_readdir_cb` callback type. The four-level branch lookup-or-create (local → exact ref → DWIM → create new branch off HEAD). |
| Overlay & deletion layer     | [components/overlay.md](./components/overlay.md)   | `deleted_list_t` (heap `char **`) loaded from `~/.gitbranchfs/<repo>/<branch>/deleted`; `overlay_load_deleted` / `_free_deleted` / `_is_deleted` / `_mark_deleted` / `_unmark_deleted`, plus `overlay_write_blob` (materialises a `git_blob` under the overlay at the corresponding relative path). |
| Core FS operations           | [components/core-fs.md](./components/core-fs.md)   | `gbfs_state_t` opaque handle. `gbfs_init` lifecycle (strdup paths, `git_wrapper_init`, load deleted list, mkdir_rec overlay, synthesise the overlay's `.git` folder with a `commondir` pointing back at the original repo). Per-operation semantics: `get_attr` / `read_dir` / `open_file` / `read_file` / `write_file` / `close_file` / `create_file` / `truncate_file` / `unlink_file` / `make_dir` / `remove_dir` / `rename_entry` / `utimens_file` / `chmod_file` / `chown_file`. |
| Directory merging            | [components/core-dir.md](./components/core-dir.md) | The `dir_entries` dedup helper, the `git_readdir_callback` adapter, and `gbfs_read_dir`'s three-source merge (git tree ∪ overlay subdir ∪ synthetic `.git` at the root) with deletion filtering. |
| FUSE callback table          | [components/fuse-ops.md](./components/fuse-ops.md) | The `gbfs_oper` vtable (15 callbacks), the `filler_adapter` shim that bridges `gbfs_read_dir`'s callback signature to libfuse3's `fuse_fill_dir_t`, and `run_fuse_fs` (one-liner around `fuse_main`). |
| Path / filesystem utilities  | [components/utils.md](./components/utils.md)       | `resolve_home_path` (`~` expansion), `join_paths`, `mkdir_rec`, `dir_exists`, `file_exists`. |
| Settings layer               | [components/settings.md](./components/settings.md) | Loaded from `~/.gitbranchfs/config.json`. Manages application settings such as `overlay_path`. Automatically creates the configuration file with default settings if it does not exist on startup. |
| Build system                 | [core/build-system.md](./core/build-system.md)     | Handwritten GNU Make, `wildcard src/*.c` auto-discovery, `pkg-config` guards for libgit2/fuse3, `-lpthread`, targets `all` (default → `./gbfs`) and `clean`. |

## How a request flows today

```
$ gbfs [mount] <branch-name> <mount-point>
        |
        v
   src/main.c:main()                         -- int main(int, char **)
        |
        v
   src/cli.c:cli_run(argc, argv)             -- two subcommands; argv[1] switch
        |   loads settings via settings_load (creating config.json with defaults if not present)
        |   "mount": getcwd(repo) + git-repo check, realpath/mkdir(mount), derive repo-name,
        |           derive mount-id from the canonical mount point,
        |           build overlay_root = "<settings.overlay_root>/<repo-name>/<branch>/<mount-id>",
        |           printf the paths, then:
        v
   src/core_fs.c:gbfs_init(repo, branch, overlay_root)
        |   strdup + git_wrapper_init (libgit2 open + branch resolve +
        |                                peel to commit + get tree)
        |       on failure: free strdups, return NULL
        |   overlay_load_deleted(overlay_root)
        |   mkdir_rec(overlay_root)
        |   synthesise overlay_root/.git/{commondir, HEAD}
        |     -- commondir points at <repo>/.git (or <repo> if bare)
        v
   src/cli.c (cont.)                          -- prepare fuse argv:
        |   fuse_argv = { prog, mount_path, "-s", "-odefault_permissions" }
        v
   src/fuse_ops.c:run_fuse_fs(argc, argv, state)
        |   fuse_main(argc, argv, &gbfs_oper, state)
        v
   On every getattr/readdir/open/read/release callback:
        |
        v
   src/fuse_ops.c                             -- pull state from
        |                                       fuse_get_context()->private_data
        v   src/core_fs.c (or src/core_dir.c for readdir)
        |   gbfs_get_attr   -- deleted? -> ENOENT
        |                       else git tree lookup (or overlay file stat)
        |   gbfs_read_dir   -- delegate to src/core_dir.c (three-source merge)
        |   gbfs_open_file  -- open overlay file fd  OR  wrap git_blob
        |   gbfs_read_file  -- read from fd  OR  copy git_blob_rawcontent
        v
   On every write/create/unlink/rmdir/rename/chmod/chown/truncate/utimens callback:
        |
        v
   src/core_fs.c (gbfs_write_file / _create_file / _unlink_file / _make_dir /
                  _remove_dir / _rename_entry / _chmod_file / _chown_file /
                  _truncate_file / _utimens_file)
        |   writes & mutations land on the overlay directory only —
        |   the git tree is read-only from inside the mount.
        v
   libfuse3 returns control to fuse_main; the loop blocks until unmount.


$ gbfs unmount <mount-point>
        |
        v
   src/cli.c (cont.)                         -- "unmount" branch
        |   realpath the mountpoint (fall back to raw argv if it
        |   disappeared), then:
        v
   system("fusermount3 -u \"<mount-point>\"")
        |   exit 0 on success, 1 otherwise; prints "Unmounted successfully."
        |   or "Error: Failed to unmount <mp>.".
```

The overlay directory is the bridge between plain git content and the
branch-virtualisation behaviour the filesystem promises: every overlay
or deletion is one more entry in `~/.gitbranchfs/<repo-name>/<branch>/<mount-id>/`
(an overlay file body, or a newline-deleted path in the `deleted`
file), and reads are resolved by merging the libgit2 tree walk with the
on-disk overlay contents. Writes land in the overlay directly; the
underlying git tree is never mutated through the mount.
