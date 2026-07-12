#define _GNU_SOURCE
#include "core_internal.h"
#include "utils.h"
#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct dir_entries {
    char **names;
    size_t count;
    size_t capacity;
};

static void add_dir_entry(struct dir_entries *de, const char *name) {
    for (size_t i = 0; i < de->count; i++) {
        if (strcmp(de->names[i], name) == 0) return;
    }
    if (de->count >= de->capacity) {
        size_t new_cap = de->capacity == 0 ? 16 : de->capacity * 2;
        char **new_names = realloc(de->names, new_cap * sizeof(char *));
        if (new_names == NULL) return;
        de->names = new_names;
        de->capacity = new_cap;
    }
    de->names[de->count] = strdup(name);
    if (de->names[de->count] != NULL) {
        de->count++;
    }
}

static int git_readdir_callback(void *buf, const char *name, const git_oid *oid, unsigned int filemode) {
    (void)oid;
    (void)filemode;
    struct dir_entries *de = (struct dir_entries *)buf;
    add_dir_entry(de, name);
    return 0;
}

int gbfs_read_dir(gbfs_state_t *state, const char *path, void *buf, 
                  int (*filler)(void *, const char *, const gbfs_stat_t *, int64_t)) {
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

    struct dir_entries de = {NULL, 0, 0};

    // 1. Read from Git Tree
    git_wrapper_readdir(state->repo, state->tree, path, &de, git_readdir_callback);

    // 2. Read from Local Overlay
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path != NULL) {
        DIR *dir = opendir(local_path);
        if (dir != NULL) {
            struct dirent *dp;
            while ((dp = readdir(dir)) != NULL) {
                if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0 ||
                    strcmp(dp->d_name, ".gitbranchfs_metadata") == 0 ||
                    strcmp(dp->d_name, "gbfs.pid") == 0) {
                    continue;
                }
                add_dir_entry(&de, dp->d_name);
            }
            closedir(dir);
        }
        free(local_path);
    }

    if (strcmp(path, "/") == 0) {
        add_dir_entry(&de, ".git");
    }

    // Add standard dot entries
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);

    // 3. Process and fill unique non-deleted entries
    for (size_t i = 0; i < de.count; i++) {
        char *entry_path = join_paths(path, de.names[i]);
        if (entry_path != NULL) {
            if (!overlay_is_deleted(state->deleted, entry_path)) {
                gbfs_stat_t st;
                memset(&st, 0, sizeof(gbfs_stat_t));
                gbfs_get_attr(state, entry_path, &st);
                filler(buf, de.names[i], &st, 0);
            }
            free(entry_path);
        }
        free(de.names[i]);
    }
    free(de.names);

    return 0;
}

int gbfs_make_dir(gbfs_state_t *state, const char *path, mode_t mode) {
    (void)mode;
    overlay_unmark_deleted(state->overlay_path, state->deleted, path);

    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    int ret = mkdir_rec(local_path);
    free(local_path);
    if (ret != 0) return -errno;
    return 0;
}

static int check_dir_empty_cb(void *buf, const char *name, const git_oid *oid, unsigned int filemode) {
    (void)name;
    (void)oid;
    (void)filemode;
    int *count = (int *)buf;
    (*count)++;
    return 0;
}

static int is_dir_empty(gbfs_state_t *state, const char *path) {
    int entries_count = 0;
    
    // Check git entries
    git_wrapper_readdir(state->repo, state->tree, path, &entries_count, check_dir_empty_cb);
    if (entries_count > 0) {
        struct dir_entries de = {NULL, 0, 0};
        git_wrapper_readdir(state->repo, state->tree, path, &de, git_readdir_callback);
        int actual_git_count = 0;
        for (size_t i = 0; i < de.count; i++) {
            char *entry_path = join_paths(path, de.names[i]);
            if (entry_path != NULL) {
                if (!overlay_is_deleted(state->deleted, entry_path)) {
                    actual_git_count++;
                }
                free(entry_path);
            }
            free(de.names[i]);
        }
        free(de.names);
        if (actual_git_count > 0) return 0;
    }

    // Check overlay entries
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path != NULL) {
        DIR *dir = opendir(local_path);
        if (dir != NULL) {
            struct dirent *dp;
            int overlay_count = 0;
            while ((dp = readdir(dir)) != NULL) {
                if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0 ||
                    strcmp(dp->d_name, ".gitbranchfs_metadata") == 0 ||
                    strcmp(dp->d_name, "gbfs.pid") == 0) {
                    continue;
                }
                char *entry_path = join_paths(path, dp->d_name);
                if (entry_path != NULL) {
                    if (!overlay_is_deleted(state->deleted, entry_path)) {
                        overlay_count++;
                    }
                    free(entry_path);
                }
            }
            closedir(dir);
            free(local_path);
            if (overlay_count > 0) return 0;
        } else {
            free(local_path);
        }
    }

    return 1;
}

int gbfs_remove_dir(gbfs_state_t *state, const char *path) {
    if (!is_dir_empty(state, path)) return -ENOTEMPTY;

    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    int in_overlay = dir_exists(local_path);
    int in_git = 0;

    const char *git_path = path;
    while (*git_path == '/') git_path++;
    if (*git_path != '\0') {
        git_tree_entry *entry = NULL;
        if (git_tree_entry_bypath(&entry, state->tree, git_path) == 0) {
            if (git_tree_entry_type(entry) == GIT_OBJECT_TREE) {
                in_git = 1;
            }
            git_tree_entry_free(entry);
        }
    }

    if (!in_overlay && !in_git) {
        free(local_path);
        return -ENOENT;
    }

    int err = 0;
    if (in_overlay) {
        if (rmdir(local_path) != 0) err = -errno;
    }
    free(local_path);

    if (err == 0 && in_git) {
        err = overlay_mark_deleted(state->overlay_path, state->deleted, path);
    }

    return err;
}

#ifdef _WIN32
static int copy_overlay_dir_rec(gbfs_state_t *state, const char *src, const char *dst) {
    char *local_src = join_paths(state->overlay_path, src);
    if (local_src == NULL) return -ENOMEM;

    DIR *dirp = opendir(local_src);
    if (dirp == NULL) {
        free(local_src);
        return -ENOENT;
    }

    int err = 0;
    struct dirent *de;
    while ((de = readdir(dirp)) != NULL && err == 0) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;

        char *sub_src = join_paths(src, de->d_name);
        char *sub_dst = join_paths(dst, de->d_name);
        if (sub_src == NULL || sub_dst == NULL) {
            err = -ENOMEM;
            free(sub_src);
            free(sub_dst);
            continue;
        }
        if (overlay_is_deleted(state->deleted, sub_src)) {
            free(sub_src);
            free(sub_dst);
            continue;
        }

        char *local_sub_src = join_paths(state->overlay_path, sub_src);
        char *local_sub_dst = join_paths(state->overlay_path, sub_dst);
        if (local_sub_src == NULL || local_sub_dst == NULL) {
            err = -ENOMEM;
            free(sub_src);
            free(sub_dst);
            free(local_sub_src);
            free(local_sub_dst);
            continue;
        }

        if (dir_exists(local_sub_src)) {
            mkdir_rec(local_sub_dst);
            err = copy_overlay_dir_rec(state, sub_src, sub_dst);
        } else if (file_exists(local_sub_src)) {
            if (file_exists(local_sub_dst)) {
                unlink(local_sub_dst);
            }
            if (!CopyFileA(local_sub_src, local_sub_dst, FALSE)) {
                DWORD win_err = GetLastError();
                if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND) err = -ENOENT;
                else if (win_err == ERROR_ACCESS_DENIED) err = -EACCES;
                else if (win_err == ERROR_SHARING_VIOLATION) err = -EBUSY;
                else err = -EIO;
            }
        }

        free(sub_src);
        free(sub_dst);
        free(local_sub_src);
        free(local_sub_dst);
    }
    closedir(dirp);
    free(local_src);
    return err;
}
#endif


static int copy_git_dir_rec(gbfs_state_t *state, const char *src, const char *dst) {
    struct dir_entries de = {NULL, 0, 0};
    git_wrapper_readdir(state->repo, state->tree, src, &de, git_readdir_callback);

    int err = 0;
    for (size_t i = 0; i < de.count; i++) {
        if (err == 0) {
            char *sub_src = join_paths(src, de.names[i]);
            char *sub_dst = join_paths(dst, de.names[i]);
            
            if (sub_src != NULL && sub_dst != NULL && !overlay_is_deleted(state->deleted, sub_src)) {
                gbfs_stat_t st;
                if (git_wrapper_stat(state->repo, state->tree, sub_src, &st, state->default_time) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        char *local_sub_dst = join_paths(state->overlay_path, sub_dst);
                        if (local_sub_dst != NULL) {
                            mkdir_rec(local_sub_dst);
                            free(local_sub_dst);
                        }
                        err = copy_git_dir_rec(state, sub_src, sub_dst);
                    } else if (S_ISREG(st.st_mode)) {
                        git_blob *blob = NULL;
                        if (git_wrapper_get_blob(state->repo, state->tree, sub_src, &blob) == 0) {
                            err = overlay_write_blob(state->overlay_path, sub_dst, blob, st.st_mode);
                            git_blob_free(blob);
                        }
                    }
                }
            }
            free(sub_src);
            free(sub_dst);
        }
        free(de.names[i]);
    }
    free(de.names);
    return err;
}

int gbfs_rename_entry(gbfs_state_t *state, const char *src, const char *dst) {
    if (overlay_is_deleted(state->deleted, src)) return -ENOENT;

    char *local_src = join_paths(state->overlay_path, src);
    char *local_dst = join_paths(state->overlay_path, dst);
    if (local_src == NULL || local_dst == NULL) {
        free(local_src);
        free(local_dst);
        return -ENOMEM;
    }

    int src_in_overlay = file_exists(local_src) || dir_exists(local_src);
    int src_in_git = 0;
    const char *git_src_path = src;
    while (*git_src_path == '/') git_src_path++;
    if (*git_src_path != '\0') {
        git_tree_entry *entry = NULL;
        if (git_tree_entry_bypath(&entry, state->tree, git_src_path) == 0) {
            src_in_git = 1;
            git_tree_entry_free(entry);
        }
    }

    if (!src_in_overlay && !src_in_git) {
        free(local_src);
        free(local_dst);
        return -ENOENT;
    }

    char *last_slash = strrchr(local_dst, '/');
    if (last_slash != NULL && last_slash != local_dst) {
        *last_slash = '\0';
        mkdir_rec(local_dst);
        *last_slash = '/';
    }

    int err = 0;
    if (src_in_overlay) {
#ifdef _WIN32
        // Files/dirs in the overlay must be renamed by copying content to dst
        // and marking src as deleted. We cannot use MoveFileExA here because the
        // WinFsp FSD holds the parent directory open with FILE_WRITE_DATA but
        // without FILE_SHARE_DELETE during the rename, which makes MoveFileExA
        // fail with ERROR_IO_DEVICE (and leaves the rename unable to proceed).
        // The "deleted" list hides src from readdir/getattr/open, so leaving
        // the original file on disk as an orphan is safe for correctness.
        int src_is_dir = dir_exists(local_src);

        if (src_is_dir) {
            char *local_dst_dir = join_paths(state->overlay_path, dst);
            if (local_dst_dir == NULL) {
                err = -ENOMEM;
            } else {
                mkdir_rec(local_dst_dir);
                free(local_dst_dir);
                err = copy_overlay_dir_rec(state, src, dst);
            }
        } else {
            // Replace existing dst file in overlay (if any)
            if (file_exists(local_dst)) {
                unlink(local_dst);
            }
            if (!CopyFileA(local_src, local_dst, FALSE)) {
                DWORD win_err = GetLastError();
                if (win_err == ERROR_FILE_NOT_FOUND || win_err == ERROR_PATH_NOT_FOUND) err = -ENOENT;
                else if (win_err == ERROR_ACCESS_DENIED) err = -EACCES;
                else if (win_err == ERROR_SHARING_VIOLATION) err = -EBUSY;
                else err = -EIO;
            }
        }
#else
        if (rename(local_src, local_dst) != 0) err = -errno;
#endif
    } else {
        gbfs_stat_t st;
        git_wrapper_stat(state->repo, state->tree, src, &st, state->default_time);
        if (S_ISREG(st.st_mode)) {
            git_blob *blob = NULL;
            err = git_wrapper_get_blob(state->repo, state->tree, src, &blob);
            if (err == 0) {
                err = overlay_write_blob(state->overlay_path, dst, blob, st.st_mode);
                git_blob_free(blob);
            }
        } else if (S_ISDIR(st.st_mode)) {
            char *local_sub_dst = join_paths(state->overlay_path, dst);
            if (local_sub_dst != NULL) {
                mkdir_rec(local_sub_dst);
                free(local_sub_dst);
            }
            err = copy_git_dir_rec(state, src, dst);
        }
    }

    if (err == 0) {
#ifdef _WIN32
        // Always hide src via the deleted list (works for both overlay-only
        // and git-sourced sources). dst must be un-marked in case a prior
        // rename had hidden it.
        overlay_mark_deleted(state->overlay_path, state->deleted, src);
        overlay_unmark_deleted(state->overlay_path, state->deleted, dst);
        (void)src_in_git;
#else
        if (src_in_git) overlay_mark_deleted(state->overlay_path, state->deleted, src);
        overlay_unmark_deleted(state->overlay_path, state->deleted, dst);
#endif
    }

    free(local_src);
    free(local_dst);
    return err;
}

int gbfs_unlink_file(gbfs_state_t *state, const char *path) {
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    int in_overlay = file_exists(local_path);
    int in_git = 0;
    
    const char *git_path = path;
    while (*git_path == '/') git_path++;
    if (*git_path != '\0') {
        git_tree_entry *entry = NULL;
        if (git_tree_entry_bypath(&entry, state->tree, git_path) == 0) {
            in_git = 1;
            git_tree_entry_free(entry);
        }
    }

    if (!in_overlay && !in_git) {
        free(local_path);
        return -ENOENT;
    }

    int err = 0;
    if (in_overlay) {
#ifdef _WIN32
        // We cannot unlink the file from disk here: the WinFsp FSD holds the
        // parent directory open with FILE_WRITE_DATA but no FILE_SHARE_DELETE
        // (because of the in-progress Create with SL_OPEN_TARGET_DIRECTORY that
        // accompanied the unlink request), so a raw _unlink on the overlay
        // file fails with EACCES. We mark the path as deleted instead so the
        // overlay layer hides it from readdir/getattr/open. The orphan file
        // is left on disk and would need GC to clean up.
        // (Note: the Windows kernel unlink request will be acknowledged
        //  successfully as far as the caller is concerned.)
        (void)0;
#else
        if (unlink(local_path) != 0) err = -errno;
#endif
    }
    free(local_path);

    if (err == 0) {
#ifdef _WIN32
        err = overlay_mark_deleted(state->overlay_path, state->deleted, path);
#else
        if (in_git) {
            err = overlay_mark_deleted(state->overlay_path, state->deleted, path);
        }
#endif
    }

    return err;
}
