#define _GNU_SOURCE
#include "compat.h"
#include "overlay.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define METADATA_DIR ".gitbranchfs_metadata"
#define DELETED_FILE ".gitbranchfs_metadata/deleted"

static int save_deleted_list(const char *overlay_root, deleted_list_t *dl) {
    char *meta_dir = join_paths(overlay_root, METADATA_DIR);
    if (meta_dir == NULL) return -ENOMEM;
    
    if (mkdir_rec(meta_dir) != 0) {
        free(meta_dir);
        return -errno;
    }
    free(meta_dir);

    char *filepath = join_paths(overlay_root, DELETED_FILE);
    if (filepath == NULL) return -ENOMEM;

    FILE *f = fopen(filepath, "w");
    free(filepath);
    if (f == NULL) return -errno;

    for (size_t i = 0; i < dl->count; i++) {
        fprintf(f, "%s\n", dl->paths[i]);
    }

    fclose(f);
    return 0;
}

deleted_list_t *overlay_load_deleted(const char *overlay_root) {
    deleted_list_t *dl = malloc(sizeof(deleted_list_t));
    if (dl == NULL) return NULL;

    dl->paths = NULL;
    dl->count = 0;
    dl->capacity = 0;

    char *filepath = join_paths(overlay_root, DELETED_FILE);
    if (filepath == NULL) return dl;

    FILE *f = fopen(filepath, "r");
    free(filepath);
    if (f == NULL) {
        return dl; // File doesn't exist yet, which is fine
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    while ((read = getline(&line, &len, f)) != -1) {
        // Strip newline characters
        while (read > 0 && (line[read - 1] == '\n' || line[read - 1] == '\r')) {
            line[read - 1] = '\0';
            read--;
        }

        if (read == 0) continue;

        if (dl->count >= dl->capacity) {
            size_t new_cap = dl->capacity == 0 ? 8 : dl->capacity * 2;
            char **new_paths = realloc(dl->paths, new_cap * sizeof(char *));
            if (new_paths == NULL) {
                // Out of memory, break early
                break;
            }
            dl->paths = new_paths;
            dl->capacity = new_cap;
        }

        dl->paths[dl->count] = strdup(line);
        if (dl->paths[dl->count] != NULL) {
            dl->count++;
        }
    }

    free(line);
    fclose(f);
    return dl;
}

void overlay_free_deleted(deleted_list_t *dl) {
    if (dl == NULL) return;
    for (size_t i = 0; i < dl->count; i++) {
        free(dl->paths[i]);
    }
    free(dl->paths);
    free(dl);
}

int overlay_is_deleted(deleted_list_t *dl, const char *path) {
    if (dl == NULL || path == NULL) return 0;

    for (size_t i = 0; i < dl->count; i++) {
        const char *dp = dl->paths[i];
        if (strcmp(path, dp) == 0) {
            return 1;
        }

        // Parent directory check: e.g. path is "/a/b/c", dp is "/a/b"
        size_t dp_len = strlen(dp);
        if (strncmp(path, dp, dp_len) == 0 && path[dp_len] == '/') {
            return 1;
        }
    }

    return 0;
}

int overlay_mark_deleted(const char *overlay_root, deleted_list_t *dl, const char *path) {
    if (dl == NULL || path == NULL) return -EINVAL;

    // Check for duplicate
    for (size_t i = 0; i < dl->count; i++) {
        if (strcmp(dl->paths[i], path) == 0) {
            return 0; // Already marked
        }
    }

    if (dl->count >= dl->capacity) {
        size_t new_cap = dl->capacity == 0 ? 8 : dl->capacity * 2;
        char **new_paths = realloc(dl->paths, new_cap * sizeof(char *));
        if (new_paths == NULL) return -ENOMEM;
        dl->paths = new_paths;
        dl->capacity = new_cap;
    }

    dl->paths[dl->count] = strdup(path);
    if (dl->paths[dl->count] == NULL) return -ENOMEM;
    dl->count++;

    return save_deleted_list(overlay_root, dl);
}

int overlay_unmark_deleted(const char *overlay_root, deleted_list_t *dl, const char *path) {
    if (dl == NULL || path == NULL) return -EINVAL;

    int found = 0;
    for (size_t i = 0; i < dl->count; i++) {
        if (strcmp(dl->paths[i], path) == 0) {
            free(dl->paths[i]);
            for (size_t j = i; j < dl->count - 1; j++) {
                dl->paths[j] = dl->paths[j + 1];
            }
            dl->count--;
            found = 1;
            i--; // Adjust index since we shifted elements
        }
    }

    if (found) {
        return save_deleted_list(overlay_root, dl);
    }

    return 0;
}

int overlay_write_blob(const char *overlay_root, const char *path, git_blob *blob, gbfs_mode_t mode) {
    char *dest_path = join_paths(overlay_root, path);
    if (dest_path == NULL) return -ENOMEM;

    // Find parent directory to create it
    char *last_slash = strrchr(dest_path, '/');
    if (last_slash != NULL && last_slash != dest_path) {
        *last_slash = '\0';
        if (mkdir_rec(dest_path) != 0) {
            free(dest_path);
            return -errno;
        }
        *last_slash = '/';
    }

    // Open file with matching permissions
    int fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_BINARY, mode);
    if (fd < 0) {
        int err = -errno;
        free(dest_path);
        return err;
    }

    const void *content = git_blob_rawcontent(blob);
    size_t size = (size_t)git_blob_rawsize(blob);
    
    size_t written = 0;
    while (written < size) {
        ssize_t ret = write(fd, (const char *)content + written, size - written);
        if (ret < 0) {
            if (errno == EINTR) continue;
            int err = -errno;
            close(fd);
            free(dest_path);
            return err;
        }
        written += ret;
    }

    close(fd);
    free(dest_path);
    return 0;
}
