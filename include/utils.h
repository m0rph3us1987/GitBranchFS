#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

// Resolve paths starting with '~' to the user's home directory.
// Returns a dynamically allocated string that the caller must free.
char *resolve_home_path(const char *path);

// Joins two paths together, ensuring there is exactly one slash between them.
// Returns a dynamically allocated string that the caller must free.
char *join_paths(const char *base, const char *relative);

// Sanitize a FUSE-supplied relative path against a base directory.
// Collapses any internal ".." / "." segments and verifies that the
// resulting path is contained within `base`. On success, writes the
// joined canonical path into `out` (NUL-terminated) and returns 0.
// Returns -1 on invalid input, -EACCES if the path escapes `base`,
// or -ENOMEM on allocation failure.
int sanitize_path(const char *base, const char *path,
                  char *out, size_t out_size);

// Recursively create directories (like mkdir -p).
// Returns 0 on success, or -1 on error.
int mkdir_rec(const char *path);

// Check if a directory exists.
int dir_exists(const char *path);

// Check if a file exists.
int file_exists(const char *path);

// Derive a deterministic, filesystem-safe identifier for a canonical
// mountpoint path. The result combines a sanitized basename of the
// mountpoint with a stable 64-bit hash of the full canonical path, e.g.
// "gbfs-1a2b3c4d5e6f7a8b". The same mountpoint path always yields the same
// identifier, while distinct mountpoints yield distinct identifiers.
// Returns a dynamically allocated string that the caller must free, or NULL
// on invalid input or allocation failure.
char *derive_mount_id(const char *mount_path);

#endif // UTILS_H
