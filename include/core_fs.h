#ifndef CORE_FS_H
#define CORE_FS_H

#include "compat.h"
#include <stdint.h>

// Opaque struct representing the file system state.
typedef struct gbfs_state gbfs_state_t;

// FS Lifecycle
gbfs_state_t *gbfs_init(const char *repo_path, const char *branch_name, const char *overlay_path);
void gbfs_destroy(gbfs_state_t *state);
const char *gbfs_get_resolved_branch(gbfs_state_t *state);

// Directory & Metadata
int gbfs_get_attr(gbfs_state_t *state, const char *path, gbfs_stat_t *st);
int gbfs_read_dir(gbfs_state_t *state, const char *path, void *buf, 
                  int (*filler)(void *, const char *, const gbfs_stat_t *, int64_t));

// File Operations
int gbfs_open_file(gbfs_state_t *state, const char *path, int flags, void **out_fh);
int gbfs_read_file(gbfs_state_t *state, void *fh, char *buf, size_t size, int64_t offset);
int gbfs_write_file(gbfs_state_t *state, void *fh, const char *buf, size_t size, int64_t offset);
int gbfs_close_file(gbfs_state_t *state, void *fh);
int gbfs_create_file(gbfs_state_t *state, const char *path, mode_t mode, void **out_fh);
int gbfs_truncate_file(gbfs_state_t *state, const char *path, int64_t size);

// Path Modifications
int gbfs_unlink_file(gbfs_state_t *state, const char *path);
int gbfs_make_dir(gbfs_state_t *state, const char *path, mode_t mode);
int gbfs_remove_dir(gbfs_state_t *state, const char *path);
int gbfs_rename_entry(gbfs_state_t *state, const char *src_path, const char *dst_path);
int gbfs_utimens_file(gbfs_state_t *state, const char *path, const gbfs_timespec_t ts[2]);
int gbfs_chmod_file(gbfs_state_t *state, const char *path, mode_t mode);
int gbfs_chown_file(gbfs_state_t *state, const char *path, uid_t uid, gid_t gid);
int gbfs_read_link(gbfs_state_t *state, const char *path, char *buf, size_t size);
int gbfs_make_symlink(gbfs_state_t *state, const char *target, const char *linkpath);

// Extended File & FUSE Operations
int gbfs_access_file(gbfs_state_t *state, const char *path, int mask);
int gbfs_fsync_file(gbfs_state_t *state, void *fh, int datasync);
int gbfs_mknod_file(gbfs_state_t *state, const char *path, mode_t mode, dev_t rdev);
int gbfs_link_file(gbfs_state_t *state, const char *oldpath, const char *newpath);
int gbfs_get_xattr(gbfs_state_t *state, const char *path, const char *name, char *value, size_t size);
int gbfs_set_xattr(gbfs_state_t *state, const char *path, const char *name, const char *value, size_t size, int flags);
int gbfs_list_xattr(gbfs_state_t *state, const char *path, char *list, size_t size);
int gbfs_remove_xattr(gbfs_state_t *state, const char *path, const char *name);
int gbfs_fallocate_file(gbfs_state_t *state, void *fh, int mode, int64_t offset, int64_t len);
int gbfs_copy_file_range(gbfs_state_t *state, void *fh_in, int64_t off_in, void *fh_out, int64_t off_out, size_t len, unsigned int flags);
int gbfs_flock_file(gbfs_state_t *state, void *fh, int op);
int gbfs_lseek_file(gbfs_state_t *state, void *fh, int64_t off, int whence, int64_t *out_off);

#endif // CORE_FS_H
