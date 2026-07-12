# libgit2 wrapper

`include/git_wrapper.h` + `src/git_wrapper.c` is the project's only
direct libgit2 dependency. Every other module deals in `gbfs_state_t`
or generic `git_blob *` handles that the wrapper hands back; the
wrapper owns the libgit2 init/shutdown lifecycle and the branch
lookup-or-create chain.

## Key files

| File                    | Responsibility                                                                                              |
|-------------------------|-------------------------------------------------------------------------------------------------------------|
| `include/git_wrapper.h` | Public prototypes: `git_wrapper_is_repo`, `git_wrapper_init`, `git_wrapper_cleanup`, `git_wrapper_stat`, `git_wrapper_get_blob`, `git_wrapper_readdir`, plus the `git_readdir_cb` callback typedef. Transitively `#include <git2.h>` so the wrapper can deal in `git_repository *` / `git_tree *` / `git_blob *`. |
| `src/git_wrapper.c`     | Implementation. Performs `git_libgit2_init` / `git_libgit2_shutdown` inside `git_wrapper_init` / `_cleanup`. Implements the four-level branch lookup-or-create chain (local → exact ref → DWIM → create new branch off HEAD) inside `git_wrapper_init`. |
| `Makefile`              | Picks up `src/git_wrapper.c` via `wildcard src/*.c`; no explicit rule. Header is reachable from `src/` because `INCLUDES = -Iinclude`. |

## `git_wrapper_is_repo`

```c
int git_wrapper_is_repo(const char *path);
```

Returns 0 if `path` is a valid git repository, negative otherwise.
Wraps `git_libgit2_init` → `git_repository_open` → `git_repository_free`
→ `git_libgit2_shutdown`. Used by `src/cli.c`'s `mount` branch to
verify the current working directory is a git repository before calling
`gbfs_init`.

## Lifecycle: `git_wrapper_init`

```c
int git_wrapper_init(const char *repo_path, const char *branch_name,
                     git_repository **out_repo,
                     git_tree **out_tree,
                     char **out_resolved_branch);
```

Steps, in order:

1. `git_libgit2_init()` (one-shot per process — multi-call is a no-op
   in libgit2 ≥ 1.5).
2. `git_repository_open(&repo, repo_path)`. Failure → `git_libgit2_shutdown()`,
   return the negative error.
3. **Branch lookup / create chain** (first success wins):
   1. `git_branch_lookup(&ref, repo, branch_name, GIT_BRANCH_LOCAL)` —
      common case; matches `refs/heads/<branch_name>`.
   2. `git_reference_lookup(&ref, repo, branch_name)` — covers
      non-branch refs (tags, arbitrary refs).
   3. `git_reference_dwim(&ref, repo, branch_name)` — the standard
      "do-what-I-mean" lookup (accepts unqualified names, `HEAD`,
      short branch names).
   4. **Create-new-branch**: when the name resolves nowhere, resolve
      the current HEAD (`git_repository_head`), peel it to a commit
      (`git_reference_peel`, `GIT_OBJECT_COMMIT`), print
      `"Branch '<requested>' not found. Creating new local branch
      '<requested>' from HEAD (<head-short>)."\n`, and create a real
      local branch at that commit with
      `git_branch_create(&ref, repo, branch_name, head_commit, 0)`.
      The new branch is persistent in the repo and has no upstream. An
      unborn/empty HEAD (nothing to branch from) or a failed
      `git_branch_create` tears down the repo handle and returns the
      libgit2 error.
4. If `out_resolved_branch` is non-NULL, fill it with a `strdup` of
   `git_reference_shorthand(ref)` (the short name the caller can show
   in user-facing output). Note: `gbfs_init` reads this back via
   `gbfs_get_resolved_branch` and prints it when it diverges from the
   requested branch.
5. Peel `ref` to a commit with `git_reference_peel(&obj, ref,
   GIT_OBJECT_COMMIT)`; free `ref` immediately.
6. `git_commit_lookup(&commit, repo, git_object_id(obj))` and free
   the peeled `obj`.
7. `git_commit_tree(&tree, commit)`; free `commit`.
8. Store `repo` into `*out_repo` and `tree` into `*out_tree`. Return 0.

The function never logs to stderr itself (apart from the create-new-branch
notice on stdout); callers decide whether the resolved branch is acceptable.

## `git_wrapper_cleanup`

```c
void git_wrapper_cleanup(git_repository *repo, git_tree *tree);
```

Frees `tree` (if non-NULL), then `repo` (if non-NULL), then calls
`git_libgit2_shutdown()`. Both pointers are set/used by
`git_wrapper_init` only — `gbfs_destroy` calls this on teardown.

## Read helpers

- `int git_wrapper_stat(git_repository *repo, git_tree *root_tree,
  const char *path, struct stat *st, time_t default_time)` —
  `stat`-equivalent against the tree. `default_time` is fed into the
  `mtime`/`ctime` fields because git tree entries do not carry
  per-file timestamps; the project uses `time(NULL)` captured at
  `gbfs_init` time.
- `int git_wrapper_get_blob(git_repository *repo, git_tree *root_tree,
  const char *path, git_blob **out_blob)` — opens a blob by path and
  returns the libgit2 handle. Caller is responsible for `git_blob_free`.
- `int git_wrapper_readdir(git_repository *repo, git_tree *root_tree,
  const char *dir_path, void *buf, git_readdir_cb cb)` — invokes `cb`
  once per direct child of `dir_path`'s tree. `git_readdir_cb` is
  `int (*)(void *buf, const char *name, const git_oid *oid, unsigned
  int filemode)` — `core_dir.c` provides the
  `git_readdir_callback` adapter that translates this into the
  project's own `(void *, const char *, const struct stat *,
  int64_t)` shape.

All three return 0 on success and a negative libgit2 error code on
failure (or `-ENOENT`-style when the path isn't in the tree, depending
on the helper).

## Integration points

- `src/core_fs.c:gbfs_init` — calls `git_wrapper_init` to fill
  `state->repo` and `state->tree`, captures
  `state->resolved_branch` into the struct, and tears them down via
  `git_wrapper_cleanup` inside `gbfs_destroy`.
- `src/core_fs.c:gbfs_get_attr` / `gbfs_open_file` / etc. — call
  `git_wrapper_stat` / `git_wrapper_get_blob` to look up files by path
  against `state->tree`.
- `src/core_dir.c:gbfs_read_dir` — calls `git_wrapper_readdir` and
  threads its callback through the project-local
  `git_readdir_callback` adapter.
- `Makefile` — `wildcard src/*.c` picks `src/git_wrapper.c` up; no
  per-TU `CPPFLAGS` are required.

## Non-goals

- This wrapper does **not** open transactions, build trees, create
  commits, or push refs. The project never writes back into a git
  repo from the mount (writes go to the overlay directory; see
  [components/overlay.md](./overlay.md)), so no commit-side helpers
  exist.
- It does not own a per-call re-resolve; `state->tree` is captured
  once at `gbfs_init` time and is treated as immutable for the
  lifetime of the mount. A user changing the branch on the original
  repo would need to unmount and re-mount to see the change.
