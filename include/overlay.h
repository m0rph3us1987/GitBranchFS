#ifndef OVERLAY_H
#define OVERLAY_H

#include "compat.h"
#include <git2.h>
#include <stddef.h>

// Structure to hold the set of deleted file paths.
typedef struct {
    char **paths;
    size_t count;
    size_t capacity;
} deleted_list_t;

// Loads the list of deleted files from ~/.gitbranchfs/<reponame>/<branch>/.gitbranchfs_metadata/deleted
deleted_list_t *overlay_load_deleted(const char *overlay_root);

// Frees the deleted list structure.
void overlay_free_deleted(deleted_list_t *dl);

// Checks if the given path (or any of its parent directories) is marked as deleted.
// Returns 1 if deleted, 0 otherwise.
int overlay_is_deleted(deleted_list_t *dl, const char *path);

// Marks a path as deleted and saves the list back to disk.
// Returns 0 on success, non-zero on failure.
int overlay_mark_deleted(const char *overlay_root, deleted_list_t *dl, const char *path);

// Unmarks a path as deleted (e.g., when a file is created or a directory is remade) and saves the list.
// Returns 0 on success, non-zero on failure.
int overlay_unmark_deleted(const char *overlay_root, deleted_list_t *dl, const char *path);

// Writes a Git blob to a file in the overlay directory at the corresponding path.
// Creates any intermediate folders in the overlay.
// Returns 0 on success, negative error code on failure.
int overlay_write_blob(const char *overlay_root, const char *path, git_blob *blob, gbfs_mode_t mode);

#endif // OVERLAY_H
