#ifndef GIT_WRAPPER_H
#define GIT_WRAPPER_H

#include "compat.h"
#include <git2.h>

// Initialize git2 library, open the repository, and find the commit/tree of the branch.
// Returns 0 on success, negative error code on failure.
int git_wrapper_init(const char *repo_path, const char *branch_name, 
                     git_repository **out_repo, git_tree **out_tree,
                     char **out_resolved_branch);

// Check whether the given path is a valid git repository.
// Returns 0 if it is a git repository, negative otherwise.
int git_wrapper_is_repo(const char *path);

// Clean up git structures.
void git_wrapper_cleanup(git_repository *repo, git_tree *tree);

// Stat a file or directory directly from the git tree.
// Returns 0 on success, negative on error (e.g. not found).
int git_wrapper_stat(git_repository *repo, git_tree *root_tree, const char *path, 
                     gbfs_stat_t *st, time_t default_time);

// Open a blob from the git tree by path and return the libgit2 git_blob pointer.
// The caller must free it when done.
// Returns 0 on success, negative on error.
int git_wrapper_get_blob(git_repository *repo, git_tree *root_tree, const char *path, 
                         git_blob **out_blob);

// Callback function type for listing git tree entries.
typedef int (*git_readdir_cb)(void *buf, const char *name, const git_oid *oid, unsigned int filemode);

// Walk the git tree for a given directory path and invoke callback for each direct child.
// Returns 0 on success, negative on error.
int git_wrapper_readdir(git_repository *repo, git_tree *root_tree, const char *dir_path, 
                        void *buf, git_readdir_cb cb);

#endif // GIT_WRAPPER_H
