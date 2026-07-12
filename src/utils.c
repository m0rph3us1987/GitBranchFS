#define _GNU_SOURCE
#include "compat.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#ifndef _WIN32
#include <pwd.h>
#endif

char *resolve_home_path(const char *path) {
    if (path == NULL) return NULL;

    if (path[0] == '~') {
        char *home_alloc = NULL;
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (home == NULL) {
            home = getenv("USERPROFILE");
        }
        if (home == NULL) {
            const char *drive = getenv("HOMEDRIVE");
            const char *home_path = getenv("HOMEPATH");
            if (drive != NULL && home_path != NULL) {
                home_alloc = malloc(strlen(drive) + strlen(home_path) + 1);
                if (home_alloc != NULL) {
                    sprintf(home_alloc, "%s%s", drive, home_path);
                    home = home_alloc;
                }
            }
        }
#else
        if (home == NULL) {
            struct passwd *pw = getpwuid(getuid());
            if (pw != NULL) {
                home = pw->pw_dir;
            }
        }
#endif
        if (home == NULL) {
            home = "";
        }

        size_t home_len = strlen(home);
        size_t rest_len = strlen(path) - 1; // skip '~'

        char *resolved = malloc(home_len + rest_len + 1);
        if (resolved != NULL) {
            int n = snprintf(resolved, home_len + rest_len + 1, "%s%s", home, path + 1);
            if (n < 0 || (size_t)n >= home_len + rest_len + 1) {
                free(resolved);
                resolved = NULL;
            }
        }
        if (home_alloc != NULL) {
            free(home_alloc);
        }
        return resolved;
    }

    return strdup(path);
}

char *join_paths(const char *base, const char *relative) {
    if (base == NULL || relative == NULL) return NULL;

    char resolved[4096];
    if (sanitize_path(base, relative, resolved, sizeof(resolved)) == 0) {
        return strdup(resolved);
    }
    return NULL;
}

int sanitize_path(const char *base, const char *path,
                  char *out, size_t out_size) {
    if (base == NULL || path == NULL || out == NULL || out_size == 0) {
        return -EINVAL;
    }

    size_t base_len = strlen(base);
    if (base_len == 0) {
        return -EINVAL;
    }

    // Skip leading slashes from the FUSE-supplied path
    const char *p = path;
    while (*p == '/') {
        p++;
    }

    // Collapse "." / ".." segments in place, copying into a scratch buffer
    // with a maximum length of out_size. The base path is not modified.
    char *collapsed = malloc(out_size);
    if (collapsed == NULL) return -ENOMEM;

    size_t clen = 0;
    const char *seg = p;
    while (*seg) {
        const char *next = seg;
        while (*next && *next != '/') next++;
        size_t seg_len = (size_t)(next - seg);

        if (seg_len == 0 || (seg_len == 1 && seg[0] == '.')) {
            // skip empty / "." segment
        } else if (seg_len == 2 && seg[0] == '.' && seg[1] == '.') {
            // pop last segment if any
            while (clen > 0 && collapsed[clen - 1] != '/') clen--;
            if (clen > 0) clen--; // drop the trailing '/'
        } else {
            // ensure room for '/' + seg + NUL
            if (clen + 1 + seg_len + 1 > out_size) {
                free(collapsed);
                return -ENAMETOOLONG;
            }
            collapsed[clen++] = '/';
            memcpy(collapsed + clen, seg, seg_len);
            clen += seg_len;
        }

        seg = next;
        while (*seg == '/') seg++;
    }
    collapsed[clen] = '\0';

    // Compose "<base><collapsed>" into `out` and verify the prefix.
    int n = snprintf(out, out_size, "%s%s", base, collapsed);
    free(collapsed);
    if (n < 0 || (size_t)n >= out_size) {
        return -ENAMETOOLONG;
    }

    // Verify the result is contained within `base`.
    if (strncmp(out, base, base_len) != 0) {
        return -EACCES;
    }
    if (out[base_len] != '\0' && out[base_len] != '/') {
        return -EACCES;
    }

    return 0;
}
int mkdir_rec(const char *path) {
    if (path == NULL) return -1;

    char *copypath = strdup(path);
    if (copypath == NULL) return -1;

    size_t len = strlen(copypath);
    if (len == 0) {
        free(copypath);
        return 0;
    }

    // Strip trailing slashes
    while (len > 1 && (copypath[len - 1] == '/' || copypath[len - 1] == '\\')) {
        copypath[len - 1] = '\0';
        len--;
    }

    // Skip drive letter (e.g. "C:") on Windows
    size_t start_idx = 1;
#ifdef _WIN32
    if (len >= 2 && copypath[1] == ':') {
        start_idx = 3; // Start after "C:\" or "C:/"
    }
#endif

    for (size_t i = start_idx; i < len; i++) {
        if (copypath[i] == '/' || copypath[i] == '\\') {
            char old = copypath[i];
            copypath[i] = '\0';
            if (copypath[0] != '\0' && strcmp(copypath, ".") != 0) {
                if (mkdir(copypath, 0700) != 0 && errno != EEXIST) {
                    free(copypath);
                    return -1;
                }
            }
            copypath[i] = old;
        }
    }

    if (mkdir(copypath, 0700) != 0 && errno != EEXIST) {
        free(copypath);
        return -1;
    }

    free(copypath);
    return 0;
}
int dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return 0;
}

int file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0);
}

char *derive_mount_id(const char *mount_path) {
    if (mount_path == NULL || mount_path[0] == '\0') return NULL;

    // 64-bit FNV-1a hash of the full canonical mountpoint path. This is the
    // deterministic component: the same mountpoint always hashes identically.
    uint64_t hash = 1469598103934665603ULL; // FNV offset basis
    for (const unsigned char *p = (const unsigned char *)mount_path; *p; p++) {
        hash ^= (uint64_t)(*p);
        hash *= 1099511628211ULL; // FNV prime
    }

    // Extract the basename of the mountpoint (ignoring trailing slashes) for
    // a human-recognizable prefix.
    size_t len = strlen(mount_path);
    while (len > 1 && mount_path[len - 1] == '/') {
        len--;
    }
    size_t start = len;
    while (start > 0 && mount_path[start - 1] != '/') {
        start--;
    }
    size_t base_len = len - start;

    // Sanitize the basename to a filesystem-safe token: keep [A-Za-z0-9._-],
    // replace everything else with '_'. Fall back to "mnt" if empty.
    char *base = malloc(base_len + 1);
    if (base == NULL) return NULL;
    size_t bi = 0;
    for (size_t i = 0; i < base_len; i++) {
        char c = mount_path[start + i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') {
            base[bi++] = c;
        } else {
            base[bi++] = '_';
        }
    }
    base[bi] = '\0';
    const char *base_token = (bi > 0) ? base : "mnt";

    int n = snprintf(NULL, 0, "%s-%016llx", base_token, (unsigned long long)hash);
    if (n < 0) {
        free(base);
        return NULL;
    }
    char *result = malloc((size_t)n + 1);
    if (result == NULL) {
        free(base);
        return NULL;
    }
    snprintf(result, (size_t)n + 1, "%s-%016llx", base_token, (unsigned long long)hash);
    free(base);
    return result;
}
