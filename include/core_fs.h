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

#endif // CORE_FS_H
