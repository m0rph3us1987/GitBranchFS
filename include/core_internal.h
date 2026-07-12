#ifndef CORE_INTERNAL_H
#define CORE_INTERNAL_H

#include "core_fs.h"
#include "git_wrapper.h"
#include "overlay.h"
#include <time.h>

struct gbfs_state {
    char *repo_path;
    char *branch_name;
    char *resolved_branch;
    char *overlay_path;
    git_repository *repo;
    git_tree *tree;
    deleted_list_t *deleted;
    time_t default_time;
};

typedef struct {
    int is_overlay;
    int fd;
    git_blob *blob;
    char *virtual_data;
    size_t virtual_size;
} gbfs_file_handle_t;

#endif // CORE_INTERNAL_H
