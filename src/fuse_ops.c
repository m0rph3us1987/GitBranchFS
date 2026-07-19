#define _GNU_SOURCE
#define FUSE_USE_VERSION 31
#include "compat.h"
#include "core_internal.h"
#include "fuse_ops.h"
#include "utils.h"
#include "overlay.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>

struct readdir_ctx {
    void *buf;
    fuse_fill_dir_t filler;
};

static int filler_adapter(void *ctx_void, const char *name, const gbfs_stat_t *st, int64_t offset) {
    struct readdir_ctx *ctx = (struct readdir_ctx *)ctx_void;
    return ctx->filler(ctx->buf, name, st, (gbfs_off_t)offset, 0);
}

// Collapse "." / ".." segments in a FUSE-supplied path and strip leading
// slashes. The result is safe to pass to gbfs_* helpers, which will
// join_paths() it with the overlay root. Returns 0 on success, in which
// case `out` holds the sanitized relative path (NUL-terminated). On
// failure, returns a negative errno and leaves `out` untouched.
static int sanitize_fuse_path(const char *path, char *out, size_t out_size) {
    if (path == NULL || out == NULL || out_size == 0) {
        return -EINVAL;
    }

    // Skip leading slashes
    const char *p = path;
    while (*p == '/') p++;

    // Empty path is fine: represents the mount root.
    if (*p == '\0') {
        if (out_size < 1) return -ENAMETOOLONG;
        out[0] = '\0';
        return 0;
    }

    size_t olen = 0;
    const char *seg = p;
    while (*seg) {
        const char *next = seg;
        while (*next && *next != '/') next++;
        size_t seg_len = (size_t)(next - seg);

        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) {
            // skip
        } else if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            // pop last segment
            while (olen > 0 && out[olen - 1] != '/') olen--;
            if (olen > 0) olen--; // drop the trailing '/'
        } else {
            if (olen + 1 + seg_len + 1 > out_size) {
                return -ENAMETOOLONG;
            }
            out[olen++] = '/';
            memcpy(out + olen, seg, seg_len);
            olen += seg_len;
        }

        seg = next;
        while (*seg == '/') seg++;
    }

    if (olen + 1 > out_size) return -ENAMETOOLONG;
    out[olen] = '\0';
    return 0;
}

static int gbfs_fuse_getattr(const char *path, gbfs_stat_t *st, struct fuse_file_info *fi) {
    (void)fi;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_get_attr(state, clean, st);
}

static int gbfs_fuse_readlink(const char *path, char *buf, size_t size) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_read_link(state, clean, buf, size);
}

static int gbfs_fuse_symlink(const char *target, const char *linkpath) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean_linkpath[PATH_MAX];
    int err = sanitize_fuse_path(linkpath, clean_linkpath, sizeof(clean_linkpath));
    if (err < 0) return err;
    return gbfs_make_symlink(state, target, clean_linkpath);
}

static int gbfs_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                             gbfs_off_t offset, struct fuse_file_info *fi,
                             enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;

    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;

    struct readdir_ctx ctx = { buf, filler };
    return gbfs_read_dir(state, clean, &ctx, filler_adapter);
}

static int gbfs_fuse_open(const char *path, struct fuse_file_info *fi) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    void *fh = NULL;
    int res = gbfs_open_file(state, clean, fi->flags, &fh);
    if (res < 0) return res;
    fi->fh = (uint64_t)(uintptr_t)fh;
    return 0;
}

static int gbfs_fuse_read(const char *path, char *buf, size_t size, gbfs_off_t offset,
                          struct fuse_file_info *fi) {
    (void)path;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    void *fh = (void *)(uintptr_t)fi->fh;
    return gbfs_read_file(state, fh, buf, size, (int64_t)offset);
}

static int gbfs_fuse_write(const char *path, const char *buf, size_t size, gbfs_off_t offset,
                           struct fuse_file_info *fi) {
    (void)path;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    void *fh = (void *)(uintptr_t)fi->fh;
    return gbfs_write_file(state, fh, buf, size, (int64_t)offset);
}

static int gbfs_fuse_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    void *fh = (void *)(uintptr_t)fi->fh;
    return gbfs_close_file(state, fh);
}

static int gbfs_fuse_create(const char *path, gbfs_mode_t mode, struct fuse_file_info *fi) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    void *fh = NULL;
    int res = gbfs_create_file(state, clean, mode, &fh);
    if (res < 0) return res;
    fi->fh = (uint64_t)(uintptr_t)fh;
    return 0;
}

static int gbfs_fuse_unlink(const char *path) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_unlink_file(state, clean);
}

static int gbfs_fuse_mkdir(const char *path, gbfs_mode_t mode) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_make_dir(state, clean, mode);
}

static int gbfs_fuse_rmdir(const char *path) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_remove_dir(state, clean);
}

static int gbfs_fuse_rename(const char *src, const char *dst, unsigned int flags) {
    if (flags & ~RENAME_NOREPLACE) return -EINVAL;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean_src[PATH_MAX];
    char clean_dst[PATH_MAX];
    int err = sanitize_fuse_path(src, clean_src, sizeof(clean_src));
    if (err < 0) return err;
    err = sanitize_fuse_path(dst, clean_dst, sizeof(clean_dst));
    if (err < 0) return err;

    if (flags & RENAME_NOREPLACE) {
        char *local_dst = join_paths(state->overlay_path, clean_dst);
        if (local_dst != NULL) {
            int exists = file_exists(local_dst) || dir_exists(local_dst);
            free(local_dst);
            if (exists) return -EEXIST;
        }
        const char *git_dst_path = clean_dst;
        while (*git_dst_path == '/') git_dst_path++;
        if (*git_dst_path != '\0') {
            git_tree_entry *entry = NULL;
            if (git_tree_entry_bypath(&entry, state->tree, git_dst_path) == 0) {
                git_tree_entry_free(entry);
                if (!overlay_is_deleted(state->deleted, clean_dst)) {
                    return -EEXIST;
                }
            }
        }
    }

    return gbfs_rename_entry(state, clean_src, clean_dst);
}

static int gbfs_fuse_truncate(const char *path, gbfs_off_t size, struct fuse_file_info *fi) {
    (void)fi;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_truncate_file(state, clean, (int64_t)size);
}

static int gbfs_fuse_utimens(const char *path, const gbfs_timespec_t ts[2], struct fuse_file_info *fi) {
    (void)fi;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_utimens_file(state, clean, ts);
}

static int gbfs_fuse_chmod(const char *path, gbfs_mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_chmod_file(state, clean, mode);
}

static int gbfs_fuse_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi) {
    (void)fi;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_chown_file(state, clean, uid, gid);
}

static int gbfs_fuse_statfs(const char *path, gbfs_statvfs_t *st) {
    (void)path;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    if (state == NULL || state->overlay_path == NULL || st == NULL) {
        return -EIO;
    }
    if (gbfs_statvfs(state->overlay_path, st) == 0) {
        return 0;
    }
    // Never report "0 bytes free" to WinFSP — that is exactly what makes
    // Windows surface "disk full" errors on every write. Fill in a safe,
    // generous fallback so at least writes are not pre-emptively rejected.
    memset(st, 0, sizeof(*st));
    st->f_bsize = 4096;
    st->f_frsize = 4096;
    st->f_blocks = 0x40000000UL;
    st->f_bfree = st->f_blocks;
    st->f_bavail = st->f_blocks;
    st->f_files = 0x40000000UL;
    st->f_ffree = st->f_files;
    st->f_favail = st->f_files;
    st->f_namemax = 260;
    return 0;
}

static int gbfs_fuse_access(const char *path, int mask) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_access_file(state, clean, mask);
}

static int gbfs_fuse_flush(const char *path, struct fuse_file_info *fi) {
    (void)path;
    (void)fi;
    return 0;
}

static int gbfs_fuse_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    void *fh = (void *)(uintptr_t)fi->fh;
    return gbfs_fsync_file(state, fh, datasync);
}

static int gbfs_fuse_opendir(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    gbfs_stat_t st;
    err = gbfs_get_attr(state, clean, &st);
    if (err < 0) return err;
    if (!S_ISDIR(st.st_mode)) return -ENOTDIR;
    return 0;
}

static int gbfs_fuse_releasedir(const char *path, struct fuse_file_info *fi) {
    (void)path;
    (void)fi;
    return 0;
}

static int gbfs_fuse_fsyncdir(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path;
    (void)datasync;
    (void)fi;
    return 0;
}

static int gbfs_fuse_mknod(const char *path, gbfs_mode_t mode, dev_t rdev) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_mknod_file(state, clean, mode, rdev);
}

static int gbfs_fuse_link(const char *oldpath, const char *newpath) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean_old[PATH_MAX];
    char clean_new[PATH_MAX];
    int err = sanitize_fuse_path(oldpath, clean_old, sizeof(clean_old));
    if (err < 0) return err;
    err = sanitize_fuse_path(newpath, clean_new, sizeof(clean_new));
    if (err < 0) return err;
    return gbfs_link_file(state, clean_old, clean_new);
}

static int gbfs_fuse_getxattr(const char *path, const char *name, char *value, size_t size) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_get_xattr(state, clean, name, value, size);
}

static int gbfs_fuse_setxattr(const char *path, const char *name, const char *value, size_t size, int flags) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_set_xattr(state, clean, name, value, size, flags);
}

static int gbfs_fuse_listxattr(const char *path, char *list, size_t size) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_list_xattr(state, clean, list, size);
}

static int gbfs_fuse_removexattr(const char *path, const char *name) {
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    char clean[PATH_MAX];
    int err = sanitize_fuse_path(path, clean, sizeof(clean));
    if (err < 0) return err;
    return gbfs_remove_xattr(state, clean, name);
}

static int gbfs_fuse_fallocate(const char *path, int mode, gbfs_off_t offset, gbfs_off_t len, struct fuse_file_info *fi) {
    (void)path;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    void *fh = (void *)(uintptr_t)fi->fh;
    return gbfs_fallocate_file(state, fh, mode, (int64_t)offset, (int64_t)len);
}

static ssize_t gbfs_fuse_copy_file_range(const char *path_in, struct fuse_file_info *fi_in, gbfs_off_t off_in,
                                          const char *path_out, struct fuse_file_info *fi_out, gbfs_off_t off_out,
                                          size_t len, int flags) {
    (void)path_in;
    (void)path_out;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    void *fh_in = (void *)(uintptr_t)fi_in->fh;
    void *fh_out = (void *)(uintptr_t)fi_out->fh;
    return (ssize_t)gbfs_copy_file_range(state, fh_in, (int64_t)off_in, fh_out, (int64_t)off_out, len, (unsigned int)flags);
}

static int gbfs_fuse_flock(const char *path, struct fuse_file_info *fi, int op) {
    (void)path;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    void *fh = (void *)(uintptr_t)fi->fh;
    return gbfs_flock_file(state, fh, op);
}

static gbfs_off_t gbfs_fuse_lseek(const char *path, gbfs_off_t off, int whence, struct fuse_file_info *fi) {
    (void)path;
    gbfs_state_t *state = (gbfs_state_t *)fuse_get_context()->private_data;
    void *fh = (void *)(uintptr_t)fi->fh;
    int64_t out_off = 0;
    int err = gbfs_lseek_file(state, fh, (int64_t)off, whence, &out_off);
    if (err < 0) return (gbfs_off_t)err;
    return (gbfs_off_t)out_off;
}

static const struct fuse_operations gbfs_oper = {
    .getattr         = gbfs_fuse_getattr,
    .readlink        = gbfs_fuse_readlink,
    .mknod           = gbfs_fuse_mknod,
    .mkdir           = gbfs_fuse_mkdir,
    .unlink          = gbfs_fuse_unlink,
    .rmdir           = gbfs_fuse_rmdir,
    .symlink         = gbfs_fuse_symlink,
    .rename          = gbfs_fuse_rename,
    .link            = gbfs_fuse_link,
    .chmod           = gbfs_fuse_chmod,
    .chown           = gbfs_fuse_chown,
    .truncate        = gbfs_fuse_truncate,
    .open            = gbfs_fuse_open,
    .read            = gbfs_fuse_read,
    .write           = gbfs_fuse_write,
    .statfs          = gbfs_fuse_statfs,
    .flush           = gbfs_fuse_flush,
    .release         = gbfs_fuse_release,
    .fsync           = gbfs_fuse_fsync,
    .setxattr        = gbfs_fuse_setxattr,
    .getxattr        = gbfs_fuse_getxattr,
    .listxattr       = gbfs_fuse_listxattr,
    .removexattr     = gbfs_fuse_removexattr,
    .opendir         = gbfs_fuse_opendir,
    .readdir         = gbfs_fuse_readdir,
    .releasedir      = gbfs_fuse_releasedir,
    .fsyncdir        = gbfs_fuse_fsyncdir,
    .access          = gbfs_fuse_access,
    .create          = gbfs_fuse_create,
    .utimens         = gbfs_fuse_utimens,
    .fallocate       = gbfs_fuse_fallocate,
    .copy_file_range = gbfs_fuse_copy_file_range,
    .lseek           = gbfs_fuse_lseek,
    .flock           = gbfs_fuse_flock,
};

int run_fuse_fs(int argc, char *argv[], gbfs_state_t *state) {
    return fuse_main(argc, argv, &gbfs_oper, state);
}
