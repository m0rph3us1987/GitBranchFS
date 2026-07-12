#define _GNU_SOURCE
#include "compat.h"
#include "core_internal.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

// Format the synthetic "/.git" gitdir pointer file for this state into a
// freshly-allocated NUL-terminated string. On success returns the buffer and,
// if out_len is non-NULL, stores its content length (excluding the NUL).
// Returns NULL on allocation failure. Both gbfs_get_attr() and
// gbfs_open_file() go through here so the reported size and the actual bytes
// can never diverge.
static char *gbfs_format_gitdir(const gbfs_state_t *state, size_t *out_len) {
    int len = snprintf(NULL, 0, "gitdir: %s/.git\n", state->overlay_path);
    if (len < 0) return NULL;

    char *buf = malloc((size_t)len + 1);
    if (buf == NULL) return NULL;

    snprintf(buf, (size_t)len + 1, "gitdir: %s/.git\n", state->overlay_path);
    if (out_len != NULL) *out_len = (size_t)len;
    return buf;
}

gbfs_state_t *gbfs_init(const char *repo_path, const char *branch_name, const char *overlay_path) {
    gbfs_state_t *state = malloc(sizeof(gbfs_state_t));
    if (state == NULL) return NULL;

    state->repo_path = strdup(repo_path);
    state->branch_name = strdup(branch_name);
    state->resolved_branch = NULL;
    state->overlay_path = strdup(overlay_path);
    state->default_time = time(NULL);

    int err = git_wrapper_init(repo_path, branch_name, &state->repo, &state->tree, &state->resolved_branch);
    if (err < 0) {
        free(state->repo_path);
        free(state->branch_name);
        free(state->overlay_path);
        free(state);
        return NULL;
    }

    state->deleted = overlay_load_deleted(overlay_path);
    mkdir_rec(overlay_path);

    // Initialize virtual .git folder structure in the overlay
    char *overlay_git = join_paths(overlay_path, ".git");
    if (overlay_git != NULL) {
        mkdir_rec(overlay_git);

        // 1. commondir
        char *commondir_path = join_paths(overlay_git, "commondir");
        if (commondir_path != NULL) {
            FILE *f = fopen(commondir_path, "w");
            if (f != NULL) {
                int is_bare = git_repository_is_bare(state->repo);
                char *real_git_dir = join_paths(repo_path, is_bare ? "" : ".git");
                if (real_git_dir != NULL) {
                    fprintf(f, "%s\n", real_git_dir);
                    free(real_git_dir);
                }
                fclose(f);
            }
            free(commondir_path);
        }

        // 2. HEAD
        char *head_path = join_paths(overlay_git, "HEAD");
        if (head_path != NULL) {
            FILE *f = fopen(head_path, "w");
            if (f != NULL) {
                fprintf(f, "ref: refs/heads/%s\n", state->resolved_branch ? state->resolved_branch : branch_name);
                fclose(f);
            }
            free(head_path);
        }

        // 3. index
        char *index_path = join_paths(overlay_git, "index");
        if (index_path != NULL) {
            if (!file_exists(index_path)) {
                git_index *idx = NULL;
                if (git_index_open(&idx, index_path) == 0) {
                    git_index_read_tree(idx, state->tree);
                    git_index_write(idx);
                    git_index_free(idx);
                }
            }
            free(index_path);
        }
        free(overlay_git);
    }

    return state;
}

void gbfs_destroy(gbfs_state_t *state) {
    if (state == NULL) return;
    overlay_free_deleted(state->deleted);
    git_wrapper_cleanup(state->repo, state->tree);
    free(state->repo_path);
    free(state->branch_name);
    free(state->resolved_branch);
    free(state->overlay_path);
    free(state);
}

const char *gbfs_get_resolved_branch(gbfs_state_t *state) {
    if (state == NULL) return NULL;
    return state->resolved_branch;
}

int gbfs_get_attr(gbfs_state_t *state, const char *path, gbfs_stat_t *st) {
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

    if (strcmp(path, "/.git") == 0) {
        size_t gitdir_len = 0;
        char *gitdir = gbfs_format_gitdir(state, &gitdir_len);
        if (gitdir == NULL) return -ENOMEM;
        free(gitdir);

        memset(st, 0, sizeof(gbfs_stat_t));
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        st->st_size = (off_t)gitdir_len;
        st->st_uid = getuid();
        st->st_gid = getgid();
        gbfs_stat_set_times(st, state->default_time);
        return 0;
    }

    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    int res = lstat(local_path, st);
    free(local_path);

    if (res == 0) {
        // Exclude the metadata folder
        const char *last_part = strrchr(path, '/');
        if (last_part != NULL && strcmp(last_part + 1, ".gitbranchfs_metadata") == 0) {
            return -ENOENT;
        }
        return 0;
    }

    int git_err = git_wrapper_stat(state->repo, state->tree, path, st, state->default_time);
    if (git_err < 0) return -ENOENT;

    return 0;
}

int gbfs_open_file(gbfs_state_t *state, const char *path, int flags, void **out_fh) {
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

    gbfs_file_handle_t *fh = malloc(sizeof(gbfs_file_handle_t));
    if (fh == NULL) return -ENOMEM;

    fh->virtual_data = NULL;
    fh->virtual_size = 0;

    if (strcmp(path, "/.git") == 0) {
        int write_mode = (flags & (O_WRONLY | O_RDWR | O_TRUNC | O_CREAT));
        if (write_mode) {
            free(fh);
            return -EACCES;
        }
        size_t gitdir_len = 0;
        fh->virtual_data = gbfs_format_gitdir(state, &gitdir_len);
        if (fh->virtual_data == NULL) {
            free(fh);
            return -ENOMEM;
        }
        fh->virtual_size = gitdir_len;
        fh->is_overlay = 0;
        fh->fd = -1;
        fh->blob = NULL;
        *out_fh = fh;
        return 0;
    }

char *local_path = join_paths(state->overlay_path, path);
    if (local_path != NULL && file_exists(local_path)) {
        int fd = open(local_path, flags | O_NOFOLLOW | O_BINARY);
        free(local_path);
        if (fd < 0) {
            free(fh);
            return -errno;
        }
        fh->is_overlay = 1;
        fh->fd = fd;
        fh->blob = NULL;
        *out_fh = fh;
        return 0;
    }
    free(local_path);

    git_blob *blob = NULL;
    int err = git_wrapper_get_blob(state->repo, state->tree, path, &blob);
    if (err < 0) {
        free(fh);
        return -ENOENT;
    }

    // Only real write intents (O_WRONLY/O_RDWR/O_TRUNC) should promote
    // a tracked file into the overlay. Pure O_CREAT must not nuke an
    // existing tree-tracked file.
    int write_mode = (flags & (O_WRONLY | O_RDWR | O_TRUNC));
    if (write_mode) {
        gbfs_stat_t st;
        git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time);
        // Refuse to promote symlink tree entries: open() with a symlink
        // filemode is EINVAL on Linux.
        if (S_ISLNK(st.st_mode)) {
            git_blob_free(blob);
            free(fh);
            return -EACCES;
        }
        err = overlay_write_blob(state->overlay_path, path, blob, st.st_mode);
        git_blob_free(blob);
        if (err < 0) {
            free(fh);
            return err;
        }

        local_path = join_paths(state->overlay_path, path);
        int fd = open(local_path, flags | O_NOFOLLOW | O_BINARY);
        free(local_path);
        if (fd < 0) {
            free(fh);
            return -errno;
        }
        fh->is_overlay = 1;
        fh->fd = fd;
        fh->blob = NULL;
    } else {
        fh->is_overlay = 0;
        fh->fd = -1;
        fh->blob = blob;
    }

    *out_fh = fh;
    return 0;
}

int gbfs_read_file(gbfs_state_t *state, void *fh, char *buf, size_t size, int64_t offset) {
    (void)state;
    if (offset < 0) return -EINVAL;
    if (size > 0x7ffff000) {
        size = 0x7ffff000;
    }
    gbfs_file_handle_t *handle = (gbfs_file_handle_t *)fh;
    if (handle->virtual_data != NULL) {
        if ((uint64_t)offset >= handle->virtual_size) return 0;
        // Safe: offset < virtual_size, so the subtraction cannot underflow
        // and avoids computing offset + size (which could overflow).
        size_t remaining = handle->virtual_size - (size_t)offset;
        size_t to_read = size < remaining ? size : remaining;
        memcpy(buf, handle->virtual_data + offset, to_read);
        return (int)to_read;
    }

    if (handle->is_overlay) {
        ssize_t ret = pread(handle->fd, buf, size, offset);
        if (ret < 0) return -errno;
        return (int)ret;
    } else {
        const char *content = git_blob_rawcontent(handle->blob);
        size_t blob_size = (size_t)git_blob_rawsize(handle->blob);
        if ((uint64_t)offset >= blob_size) return 0;
        // Safe: offset < blob_size, so the subtraction cannot underflow
        // and avoids computing offset + size (which could overflow).
        size_t remaining = blob_size - (size_t)offset;
        size_t to_read = size < remaining ? size : remaining;
        memcpy(buf, content + offset, to_read);
        return (int)to_read;
    }
}

int gbfs_write_file(gbfs_state_t *state, void *fh, const char *buf, size_t size, int64_t offset) {
    (void)state;
    if (offset < 0) return -EINVAL;
    if (size > 0x7ffff000) {
        size = 0x7ffff000;
    }
    gbfs_file_handle_t *handle = (gbfs_file_handle_t *)fh;
    if (!handle->is_overlay) return -EBADF;

    ssize_t ret = pwrite(handle->fd, buf, size, offset);
    if (ret < 0) return -errno;
    return (int)ret;
}

int gbfs_close_file(gbfs_state_t *state, void *fh) {
    (void)state;
    gbfs_file_handle_t *handle = (gbfs_file_handle_t *)fh;
    if (handle->virtual_data != NULL) {
        free(handle->virtual_data);
    } else if (handle->is_overlay) {
        if (handle->fd >= 0) close(handle->fd);
    } else {
        if (handle->blob != NULL) git_blob_free(handle->blob);
    }
    free(handle);
    return 0;
}

int gbfs_create_file(gbfs_state_t *state, const char *path, mode_t mode, void **out_fh) {
    overlay_unmark_deleted(state->overlay_path, state->deleted, path);

    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    char *last_slash = strrchr(local_path, '/');
    if (last_slash != NULL && last_slash != local_path) {
        *last_slash = '\0';
        mkdir_rec(local_path);
        *last_slash = '/';
    }

    int fd = open(local_path, O_CREAT | O_RDWR | O_TRUNC | O_NOFOLLOW | O_BINARY, mode);
    free(local_path);
    if (fd < 0) return -errno;

    gbfs_file_handle_t *fh = malloc(sizeof(gbfs_file_handle_t));
    if (fh == NULL) {
        close(fd);
        return -ENOMEM;
    }
    fh->is_overlay = 1;
    fh->fd = fd;
    fh->blob = NULL;
    fh->virtual_data = NULL;
    fh->virtual_size = 0;
    *out_fh = fh;
    return 0;
}

int gbfs_truncate_file(gbfs_state_t *state, const char *path, int64_t size) {
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    if (!file_exists(local_path)) {
        git_blob *blob = NULL;
        int err = git_wrapper_get_blob(state->repo, state->tree, path, &blob);
        if (err == 0) {
            gbfs_stat_t st;
            git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time);
            err = overlay_write_blob(state->overlay_path, path, blob, st.st_mode);
            git_blob_free(blob);
            if (err < 0) {
                free(local_path);
                return err;
            }
        } else {
            free(local_path);
            return -ENOENT;
        }
    }

    int ret = truncate(local_path, size);
    free(local_path);
    if (ret < 0) return -errno;
    return 0;
}

int gbfs_utimens_file(gbfs_state_t *state, const char *path, const gbfs_timespec_t ts[2]) {
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    if (!file_exists(local_path) && !dir_exists(local_path)) {
        git_blob *blob = NULL;
        int err = git_wrapper_get_blob(state->repo, state->tree, path, &blob);
        if (err == 0) {
            gbfs_stat_t st;
            git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time);
            err = overlay_write_blob(state->overlay_path, path, blob, st.st_mode);
            git_blob_free(blob);
            if (err < 0) {
                free(local_path);
                return err;
            }
        } else {
            gbfs_stat_t st;
            if (git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time) == 0 && S_ISDIR(st.st_mode)) {
                mkdir_rec(local_path);
            } else {
                free(local_path);
                return -ENOENT;
            }
        }
    }

    int ret = utimensat(AT_FDCWD, local_path, ts, AT_SYMLINK_NOFOLLOW);
    free(local_path);
    if (ret < 0) return -errno;
    return 0;
}

int gbfs_chmod_file(gbfs_state_t *state, const char *path, mode_t mode) {
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    if (!file_exists(local_path) && !dir_exists(local_path)) {
        git_blob *blob = NULL;
        int err = git_wrapper_get_blob(state->repo, state->tree, path, &blob);
        if (err == 0) {
            err = overlay_write_blob(state->overlay_path, path, blob, mode);
            git_blob_free(blob);
            if (err < 0) {
                free(local_path);
                return err;
            }
        } else {
            gbfs_stat_t st;
            if (git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time) == 0 && S_ISDIR(st.st_mode)) {
                mkdir_rec(local_path);
            } else {
                free(local_path);
                return -ENOENT;
            }
        }
    }

    int ret = chmod(local_path, mode);
    free(local_path);
    if (ret < 0) return -errno;
    return 0;
}

int gbfs_chown_file(gbfs_state_t *state, const char *path, uid_t uid, gid_t gid) {
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    if (!file_exists(local_path) && !dir_exists(local_path)) {
        git_blob *blob = NULL;
        int err = git_wrapper_get_blob(state->repo, state->tree, path, &blob);
        if (err == 0) {
            gbfs_stat_t st;
            git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time);
            err = overlay_write_blob(state->overlay_path, path, blob, st.st_mode);
            git_blob_free(blob);
            if (err < 0) {
                free(local_path);
                return err;
            }
        } else {
            gbfs_stat_t st;
            if (git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time) == 0 && S_ISDIR(st.st_mode)) {
                mkdir_rec(local_path);
            } else {
                free(local_path);
                return -ENOENT;
            }
        }
    }

    int ret = lchown(local_path, uid, gid);
    free(local_path);
    if (ret < 0) return -errno;
    return 0;
}
