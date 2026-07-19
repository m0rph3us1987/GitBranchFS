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

int gbfs_read_link(gbfs_state_t *state, const char *path, char *buf, size_t size) {
    if (state == NULL || path == NULL || buf == NULL || size == 0) return -EINVAL;
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    gbfs_stat_t st;
    if (lstat(local_path, &st) == 0) {
        if (!S_ISLNK(st.st_mode)) {
            free(local_path);
            return -EINVAL;
        }
        ssize_t ret = readlink(local_path, buf, size - 1);
        free(local_path);
        if (ret < 0) return -errno;
        buf[ret] = '\0';
        return 0;
    }
    free(local_path);

    if (git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time) < 0) {
        return -ENOENT;
    }
    if (!S_ISLNK(st.st_mode)) {
        return -EINVAL;
    }

    git_blob *blob = NULL;
    int err = git_wrapper_get_blob(state->repo, state->tree, path, &blob);
    if (err < 0) return -ENOENT;

    const char *content = git_blob_rawcontent(blob);
    size_t blob_size = (size_t)git_blob_rawsize(blob);

    size_t to_copy = (blob_size < size - 1) ? blob_size : (size - 1);
    memcpy(buf, content, to_copy);
    buf[to_copy] = '\0';

    git_blob_free(blob);
    return 0;
}

int gbfs_make_symlink(gbfs_state_t *state, const char *target, const char *linkpath) {
    if (state == NULL || target == NULL || linkpath == NULL) return -EINVAL;

    gbfs_stat_t st;
    if (gbfs_get_attr(state, linkpath, &st) == 0) {
        return -EEXIST;
    }

    overlay_unmark_deleted(state->overlay_path, state->deleted, linkpath);

    char *local_path = join_paths(state->overlay_path, linkpath);
    if (local_path == NULL) return -ENOMEM;

    char *last_slash = strrchr(local_path, '/');
    if (last_slash != NULL && last_slash != local_path) {
        *last_slash = '\0';
        mkdir_rec(local_path);
        *last_slash = '/';
    }

    int res = symlink(target, local_path);
    free(local_path);
    if (res < 0) return -errno;
    return 0;
}

int gbfs_access_file(gbfs_state_t *state, const char *path, int mask) {
    if (state == NULL || path == NULL) return -EINVAL;
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    gbfs_stat_t st;
    if (lstat(local_path, &st) == 0) {
        int res = access(local_path, mask);
        free(local_path);
        if (res < 0) return -errno;
        return 0;
    }
    free(local_path);

    if (git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time) < 0) {
        return -ENOENT;
    }
    return 0;
}

int gbfs_fsync_file(gbfs_state_t *state, void *fh, int datasync) {
    (void)state;
    gbfs_file_handle_t *handle = (gbfs_file_handle_t *)fh;
    if (handle == NULL) return 0;
    if (handle->is_overlay && handle->fd >= 0) {
#ifdef _WIN32
        (void)datasync;
        FlushFileBuffers((HANDLE)_get_osfhandle(handle->fd));
#else
        int res = datasync ? fdatasync(handle->fd) : fsync(handle->fd);
        if (res < 0) return -errno;
#endif
    }
    return 0;
}

int gbfs_mknod_file(gbfs_state_t *state, const char *path, mode_t mode, dev_t rdev) {
    if (state == NULL || path == NULL) return -EINVAL;

    if (S_ISREG(mode)) {
        void *fh = NULL;
        int res = gbfs_create_file(state, path, mode, &fh);
        if (res == 0) gbfs_close_file(state, fh);
        return res;
    }

    overlay_unmark_deleted(state->overlay_path, state->deleted, path);

    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    char *last_slash = strrchr(local_path, '/');
    if (last_slash != NULL && last_slash != local_path) {
        *last_slash = '\0';
        mkdir_rec(local_path);
        *last_slash = '/';
    }

#ifdef _WIN32
    (void)mode; (void)rdev; free(local_path); return -ENOSYS;
#else
    int res = mknod(local_path, mode, rdev);
    free(local_path);
    if (res < 0) return -errno;
    return 0;
#endif
}

int gbfs_link_file(gbfs_state_t *state, const char *oldpath, const char *newpath) {
    if (state == NULL || oldpath == NULL || newpath == NULL) return -EINVAL;

    gbfs_stat_t st;
    if (gbfs_get_attr(state, oldpath, &st) < 0) return -ENOENT;
    if (gbfs_get_attr(state, newpath, &st) == 0) return -EEXIST;

    char *local_old = join_paths(state->overlay_path, oldpath);
    if (local_old == NULL) return -ENOMEM;

    if (!file_exists(local_old) && !dir_exists(local_old)) {
        git_blob *blob = NULL;
        if (git_wrapper_get_blob(state->repo, state->tree, oldpath, &blob) == 0) {
            git_wrapper_stat(state->repo, state->tree, oldpath, &st, state->default_time);
            overlay_write_blob(state->overlay_path, oldpath, blob, st.st_mode);
            git_blob_free(blob);
        }
    }

    overlay_unmark_deleted(state->overlay_path, state->deleted, newpath);

    char *local_new = join_paths(state->overlay_path, newpath);
    if (local_new == NULL) {
        free(local_old);
        return -ENOMEM;
    }

    char *last_slash = strrchr(local_new, '/');
    if (last_slash != NULL && last_slash != local_new) {
        *last_slash = '\0';
        mkdir_rec(local_new);
        *last_slash = '/';
    }

#ifdef _WIN32
    int res = CreateHardLinkA(local_new, local_old, NULL) ? 0 : -1;
#else
    int res = link(local_old, local_new);
#endif
    free(local_old);
    free(local_new);
    if (res < 0) return -errno;
    return 0;
}

int gbfs_get_xattr(gbfs_state_t *state, const char *path, const char *name, char *value, size_t size) {
    (void)name; (void)value; (void)size;
    if (state == NULL || path == NULL) return -EINVAL;
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

#ifndef _WIN32
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    gbfs_stat_t st;
    if (lstat(local_path, &st) == 0) {
        ssize_t ret = lgetxattr(local_path, name, value, size);
        free(local_path);
        if (ret < 0) return -errno;
        return (int)ret;
    }
    free(local_path);
#endif
    return -ENODATA;
}

int gbfs_set_xattr(gbfs_state_t *state, const char *path, const char *name, const char *value, size_t size, int flags) {
    (void)name; (void)value; (void)size; (void)flags;
    if (state == NULL || path == NULL) return -EINVAL;
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

#ifndef _WIN32
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    if (!file_exists(local_path) && !dir_exists(local_path)) {
        git_blob *blob = NULL;
        if (git_wrapper_get_blob(state->repo, state->tree, path, &blob) == 0) {
            gbfs_stat_t st;
            git_wrapper_stat(state->repo, state->tree, path, &st, state->default_time);
            overlay_write_blob(state->overlay_path, path, blob, st.st_mode);
            git_blob_free(blob);
        }
    }

    int ret = lsetxattr(local_path, name, value, size, flags);
    free(local_path);
    if (ret < 0) return -errno;
    return 0;
#else
    return -ENOTSUP;
#endif
}

int gbfs_list_xattr(gbfs_state_t *state, const char *path, char *list, size_t size) {
    (void)list; (void)size;
    if (state == NULL || path == NULL) return -EINVAL;
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

#ifndef _WIN32
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    gbfs_stat_t st;
    if (lstat(local_path, &st) == 0) {
        ssize_t ret = llistxattr(local_path, list, size);
        free(local_path);
        if (ret < 0) return -errno;
        return (int)ret;
    }
    free(local_path);
#endif
    return 0;
}

int gbfs_remove_xattr(gbfs_state_t *state, const char *path, const char *name) {
    (void)name;
    if (state == NULL || path == NULL) return -EINVAL;
    if (overlay_is_deleted(state->deleted, path)) return -ENOENT;

#ifndef _WIN32
    char *local_path = join_paths(state->overlay_path, path);
    if (local_path == NULL) return -ENOMEM;

    gbfs_stat_t st;
    if (lstat(local_path, &st) == 0) {
        int ret = lremovexattr(local_path, name);
        free(local_path);
        if (ret < 0) return -errno;
        return 0;
    }
    free(local_path);
#endif
    return -ENODATA;
}

int gbfs_fallocate_file(gbfs_state_t *state, void *fh, int mode, int64_t offset, int64_t len) {
    (void)state;
    gbfs_file_handle_t *handle = (gbfs_file_handle_t *)fh;
    if (handle == NULL || !handle->is_overlay || handle->fd < 0) return -EBADF;

#if defined(__linux__) && defined(_GNU_SOURCE)
    int res = fallocate(handle->fd, mode, (off_t)offset, (off_t)len);
    if (res < 0) return -errno;
    return 0;
#else
    (void)mode; (void)offset; (void)len;
    return -ENOSYS;
#endif
}

int gbfs_copy_file_range(gbfs_state_t *state, void *fh_in, int64_t off_in, void *fh_out, int64_t off_out, size_t len, unsigned int flags) {
    (void)state;
    gbfs_file_handle_t *in = (gbfs_file_handle_t *)fh_in;
    gbfs_file_handle_t *out = (gbfs_file_handle_t *)fh_out;
    if (in == NULL || out == NULL) return -EBADF;

#if defined(__linux__) && defined(_GNU_SOURCE)
    if (in->is_overlay && out->is_overlay && in->fd >= 0 && out->fd >= 0) {
        off_t i = (off_t)off_in;
        off_t o = (off_t)off_out;
        ssize_t ret = copy_file_range(in->fd, &i, out->fd, &o, len, flags);
        if (ret < 0) return -errno;
        return (int)ret;
    }
#else
    (void)off_in; (void)off_out; (void)len; (void)flags;
#endif
    return -EXDEV;
}

int gbfs_flock_file(gbfs_state_t *state, void *fh, int op) {
    (void)state;
    gbfs_file_handle_t *handle = (gbfs_file_handle_t *)fh;
    if (handle == NULL) return -EBADF;
#ifndef _WIN32
    if (handle->is_overlay && handle->fd >= 0) {
        int res = flock(handle->fd, op);
        if (res < 0) return -errno;
    }
#else
    (void)op;
#endif
    return 0;
}

int gbfs_lseek_file(gbfs_state_t *state, void *fh, int64_t off, int whence, int64_t *out_off) {
    (void)state;
    gbfs_file_handle_t *handle = (gbfs_file_handle_t *)fh;
    if (handle == NULL) return -EBADF;

    if (handle->is_overlay && handle->fd >= 0) {
        off_t res = lseek(handle->fd, (off_t)off, whence);
        if (res == (off_t)-1) return -errno;
        if (out_off != NULL) *out_off = (int64_t)res;
        return 0;
    }

    size_t size = 0;
    if (handle->virtual_data != NULL) size = handle->virtual_size;
    else if (handle->blob != NULL) size = (size_t)git_blob_rawsize(handle->blob);

    int64_t new_off = 0;
    if (whence == SEEK_SET) {
        new_off = off;
    } else if (whence == SEEK_CUR || whence == SEEK_END) {
        new_off = (int64_t)size + off;
    } else if (whence == 3 /* SEEK_DATA */) {
        if ((uint64_t)off >= size) return -ENXIO;
        new_off = off;
    } else if (whence == 4 /* SEEK_HOLE */) {
        new_off = (int64_t)size;
    } else {
        return -EINVAL;
    }

    if (new_off < 0) return -EINVAL;
    if (out_off != NULL) *out_off = new_off;
    return 0;
}
