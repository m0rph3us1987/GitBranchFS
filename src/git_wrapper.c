#define _DEFAULT_SOURCE
#include "git_wrapper.h"
#include "compat.h"
#include <stdio.h>
#include <string.h>


int git_wrapper_is_repo(const char *path) {
    git_libgit2_init();

    git_repository *repo = NULL;
    int error = git_repository_open(&repo, path);
    if (error < 0) {
        git_libgit2_shutdown();
        return error;
    }

    git_repository_free(repo);
    git_libgit2_shutdown();
    return 0;
}

int git_wrapper_init(const char *repo_path, const char *branch_name, 
                     git_repository **out_repo, git_tree **out_tree,
                     char **out_resolved_branch) {
    git_libgit2_init();

    git_repository *repo = NULL;
    int error = git_repository_open(&repo, repo_path);
    if (error < 0) {
        git_libgit2_shutdown();
        return error;
    }
    
    git_reference *ref = NULL;
    // Look up branch name locally first
    error = git_branch_lookup(&ref, repo, branch_name, GIT_BRANCH_LOCAL);
    if (error < 0) {
        // Try exact reference lookup
        error = git_reference_lookup(&ref, repo, branch_name);
        if (error < 0) {
            // DWIM lookup
            error = git_reference_dwim(&ref, repo, branch_name);
            if (error < 0) {
                // Branch does not exist anywhere: create a new local branch
                // of this name at the current HEAD commit and mount it, so
                // commits land on the requested branch instead of clobbering
                // the default branch.
                git_reference *head_ref = NULL;
                error = git_repository_head(&head_ref, repo);
                if (error < 0) {
                    // Unborn HEAD / empty repo: nothing to branch from.
                    git_repository_free(repo);
                    git_libgit2_shutdown();
                    return error;
                }

                git_object *head_obj = NULL;
                error = git_reference_peel(&head_obj, head_ref, GIT_OBJECT_COMMIT);
                if (error < 0) {
                    git_reference_free(head_ref);
                    git_repository_free(repo);
                    git_libgit2_shutdown();
                    return error;
                }

                const char *head_short = git_reference_shorthand(head_ref);
                printf("Branch '%s' not found. Creating new local branch '%s' from HEAD (%s).\n",
                       branch_name, branch_name, head_short ? head_short : "HEAD");

                error = git_branch_create(&ref, repo, branch_name,
                                          (const git_commit *)head_obj, 0);
                git_object_free(head_obj);
                git_reference_free(head_ref);
                if (error < 0) {
                    git_repository_free(repo);
                    git_libgit2_shutdown();
                    return error;
                }
            }
        }
    }

    if (out_resolved_branch != NULL) {
        *out_resolved_branch = strdup(git_reference_shorthand(ref));
    }

    git_object *obj = NULL;
    error = git_reference_peel(&obj, ref, GIT_OBJECT_COMMIT);
    git_reference_free(ref);
    if (error < 0) {
        git_repository_free(repo);
        git_libgit2_shutdown();
        return error;
    }

    git_commit *commit = NULL;
    error = git_commit_lookup(&commit, repo, git_object_id(obj));
    git_object_free(obj);
    if (error < 0) {
        git_repository_free(repo);
        git_libgit2_shutdown();
        return error;
    }

    git_tree *tree = NULL;
    error = git_commit_tree(&tree, commit);
    git_commit_free(commit);
    if (error < 0) {
        git_repository_free(repo);
        git_libgit2_shutdown();
        return error;
    }

    *out_repo = repo;
    *out_tree = tree;
    return 0;
}

void git_wrapper_cleanup(git_repository *repo, git_tree *tree) {
    if (tree != NULL) {
        git_tree_free(tree);
    }
    if (repo != NULL) {
        git_repository_free(repo);
    }
    git_libgit2_shutdown();
}

int git_wrapper_stat(git_repository *repo, git_tree *root_tree, const char *path, 
                     gbfs_stat_t *st, time_t default_time) {
    if (path == NULL || st == NULL) return -1;

    const char *git_path = path;
    while (*git_path == '/') {
        git_path++;
    }

    if (*git_path == '\0') {
        memset(st, 0, sizeof(gbfs_stat_t));
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        gbfs_stat_set_times(st, default_time);
        st->st_uid = getuid();
        st->st_gid = getgid();
        return 0;
    }

    git_tree_entry *entry = NULL;
    int error = git_tree_entry_bypath(&entry, root_tree, git_path);
    if (error < 0) {
        return error;
    }

    git_filemode_t filemode = git_tree_entry_filemode(entry);
    memset(st, 0, sizeof(gbfs_stat_t));
    
    gbfs_stat_set_times(st, default_time);
    st->st_uid = getuid();
    st->st_gid = getgid();

    if (filemode == GIT_FILEMODE_TREE) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else if (filemode == GIT_FILEMODE_BLOB || filemode == GIT_FILEMODE_BLOB_EXECUTABLE) {
        st->st_mode = S_IFREG | ((filemode == GIT_FILEMODE_BLOB_EXECUTABLE) ? 0755 : 0644);
        st->st_nlink = 1;
        
        git_blob *blob = NULL;
        error = git_blob_lookup(&blob, repo, git_tree_entry_id(entry));
        if (error == 0) {
            st->st_size = (off_t)git_blob_rawsize(blob);
            git_blob_free(blob);
        } else {
            st->st_size = 0;
        }
    } else if (filemode == GIT_FILEMODE_LINK) {
        st->st_mode = S_IFLNK | 0777;
        st->st_nlink = 1;
        
        git_blob *blob = NULL;
        error = git_blob_lookup(&blob, repo, git_tree_entry_id(entry));
        if (error == 0) {
            st->st_size = (off_t)git_blob_rawsize(blob);
            git_blob_free(blob);
        } else {
            st->st_size = 0;
        }
    } else {
        git_tree_entry_free(entry);
        return -1;
    }

    git_tree_entry_free(entry);
    return 0;
}

int git_wrapper_get_blob(git_repository *repo, git_tree *root_tree, const char *path, 
                         git_blob **out_blob) {
    if (path == NULL || out_blob == NULL) return -1;

    const char *git_path = path;
    while (*git_path == '/') {
        git_path++;
    }

    git_tree_entry *entry = NULL;
    int error = git_tree_entry_bypath(&entry, root_tree, git_path);
    if (error < 0) {
        return error;
    }

    if (git_tree_entry_type(entry) != GIT_OBJECT_BLOB) {
        git_tree_entry_free(entry);
        return -1;
    }

    error = git_blob_lookup(out_blob, repo, git_tree_entry_id(entry));
    git_tree_entry_free(entry);
    return error;
}

int git_wrapper_readdir(git_repository *repo, git_tree *root_tree, const char *dir_path, 
                        void *buf, git_readdir_cb cb) {
    if (dir_path == NULL || cb == NULL) return -1;

    const char *git_path = dir_path;
    while (*git_path == '/') {
        git_path++;
    }

    git_tree *target_tree = NULL;

    if (*git_path == '\0') {
        target_tree = root_tree;
    } else {
        git_tree_entry *entry = NULL;
        int error = git_tree_entry_bypath(&entry, root_tree, git_path);
        if (error < 0) {
            return error;
        }

        if (git_tree_entry_type(entry) != GIT_OBJECT_TREE) {
            git_tree_entry_free(entry);
            return -1;
        }

        error = git_tree_lookup(&target_tree, repo, git_tree_entry_id(entry));
        git_tree_entry_free(entry);
        if (error < 0) {
            return error;
        }
    }

    size_t count = git_tree_entrycount(target_tree);
    for (size_t i = 0; i < count; i++) {
        const git_tree_entry *entry = git_tree_entry_byindex(target_tree, i);
        const char *name = git_tree_entry_name(entry);
        const git_oid *oid = git_tree_entry_id(entry);
        unsigned int filemode = git_tree_entry_filemode(entry);
        
        cb(buf, name, oid, filemode);
    }

    if (target_tree != root_tree) {
        git_tree_free(target_tree);
    }

    return 0;
}
