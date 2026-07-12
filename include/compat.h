#ifndef COMPAT_H
#define COMPAT_H

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_DEPRECATE

#include <windows.h>
#include <io.h>
#include <direct.h>
#include <process.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Disable MSVC warning about deprecated POSIX names
#pragma warning(disable:4996)

#ifndef PATH_MAX
#define PATH_MAX 260
#endif

typedef unsigned short mode_t;
typedef uint32_t uid_t;
typedef uint32_t gid_t;

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
#endif

// Define path separator
#define PATH_SEP '\\'
#define PATH_SEP_STR "\\"

// POSIX macros missing in MSVC
#ifndef S_ISDIR
#define S_ISDIR(mode)  (((mode) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(mode)  (((mode) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(mode)  (((mode) & S_IFMT) == S_IFLNK)
#endif
#ifndef S_IFLNK
#define S_IFLNK 0120000
#endif

// FUSE binary mode mapping
#define O_NOFOLLOW 0
#define O_BINARY _O_BINARY

// POSIX mappings
#define getcwd _getcwd
#define unlink _unlink
#define rmdir _rmdir
#define mkdir(path, mode) _mkdir(path)
#define getuid() 0
#define getgid() 0

// Include WinFSP FUSE3
#include <fuse3/fuse.h>

typedef struct fuse_stat gbfs_stat_t;
typedef struct fuse_timespec gbfs_timespec_t;
typedef fuse_off_t gbfs_off_t;
typedef fuse_mode_t gbfs_mode_t;

static inline void gbfs_stat_set_times(gbfs_stat_t *st, time_t t) {
    st->st_atim.tv_sec = (int64_t)t;
    st->st_mtim.tv_sec = (int64_t)t;
    st->st_ctim.tv_sec = (int64_t)t;
}

// Dirent compatibility layer
struct dirent {
    char d_name[MAX_PATH];
};

typedef struct {
    HANDLE hFind;
    WIN32_FIND_DATAA findData;
    struct dirent entry;
    int first;
} DIR;

static inline DIR *opendir(const char *path) {
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s\\*", path);

    DIR *dir = (DIR *)malloc(sizeof(DIR));
    if (dir == NULL) return NULL;

    dir->hFind = FindFirstFileA(search_path, &dir->findData);
    if (dir->hFind == INVALID_HANDLE_VALUE) {
        free(dir);
        return NULL;
    }
    dir->first = 1;
    return dir;
}

static inline struct dirent *readdir(DIR *dir) {
    if (dir->first) {
        dir->first = 0;
    } else {
        if (!FindNextFileA(dir->hFind, &dir->findData)) {
            return NULL;
        }
    }
    strncpy(dir->entry.d_name, dir->findData.cFileName, sizeof(dir->entry.d_name));
    return &dir->entry;
}

static inline int closedir(DIR *dir) {
    if (dir) {
        FindClose(dir->hFind);
        free(dir);
    }
    return 0;
}

// pread/pwrite compatibility
static inline int pread(int fd, void *buf, size_t count, int64_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFF);
    DWORD bytesRead = 0;
    if (!ReadFile(h, buf, (DWORD)count, &bytesRead, &ov)) {
        DWORD err = GetLastError();
        if (err == ERROR_HANDLE_EOF) {
            return 0;
        }
        switch (err) {
            case ERROR_ACCESS_DENIED: errno = EACCES; break;
            case ERROR_INVALID_PARAMETER: errno = EINVAL; break;
            default: errno = EIO; break;
        }
        return -1;
    }
    return (int)bytesRead;
}

static inline int pwrite(int fd, const void *buf, size_t count, int64_t offset) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov = {0};
    ov.Offset = (DWORD)(offset & 0xFFFFFFFF);
    ov.OffsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFF);
    DWORD bytesWritten = 0;
    if (!WriteFile(h, buf, (DWORD)count, &bytesWritten, &ov)) {
        DWORD err = GetLastError();
        switch (err) {
            case ERROR_ACCESS_DENIED: errno = EACCES; break;
            case ERROR_INVALID_PARAMETER: errno = EINVAL; break;
            default: errno = EIO; break;
        }
        return -1;
    }
    return (int)bytesWritten;
}

// truncate compatibility
static inline int win32_truncate(const char *path, int64_t size) {
    int fd = _open(path, _O_BINARY | _O_RDWR);
    if (fd < 0) return -1;
    int ret = _chsize_s(fd, size);
    _close(fd);
    if (ret != 0) {
        errno = ret;
        return -1;
    }
    return 0;
}
#define truncate(path, size) win32_truncate(path, size)

// utimensat compatibility
static inline int win32_utimens(const char *path, const struct fuse_timespec ts[2]) {
    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) errno = ENOENT;
        else if (err == ERROR_ACCESS_DENIED) errno = EACCES;
        else errno = EIO;
        return -1;
    }

    FILETIME atime, mtime;
    ULARGE_INTEGER li;
    
    li.QuadPart = (ts[0].tv_sec * 10000000ULL) + (ts[0].tv_nsec / 100ULL) + 116444736000000000ULL;
    atime.dwLowDateTime = li.LowPart;
    atime.dwHighDateTime = li.HighPart;

    li.QuadPart = (ts[1].tv_sec * 10000000ULL) + (ts[1].tv_nsec / 100ULL) + 116444736000000000ULL;
    mtime.dwLowDateTime = li.LowPart;
    mtime.dwHighDateTime = li.HighPart;

    BOOL ok = SetFileTime(h, NULL, &atime, &mtime);
    CloseHandle(h);
    if (!ok) {
        errno = EIO;
        return -1;
    }
    return 0;
}
#define utimensat(fd, path, ts, flags) win32_utimens(path, ts)

// lstat compatibility (maps standard local _stat64 to fuse_stat)
static inline int win32_lstat(const char *path, struct fuse_stat *st) {
    struct _stat64 st_win;
    int res = _stat64(path, &st_win);
    if (res != 0) return res;
    memset(st, 0, sizeof(*st));
    st->st_mode = st_win.st_mode;
    st->st_size = st_win.st_size;
    st->st_nlink = st_win.st_nlink;
    st->st_mtim.tv_sec = st_win.st_mtime;
    st->st_atim.tv_sec = st_win.st_atime;
    st->st_ctim.tv_sec = st_win.st_ctime;
    st->st_uid = 0;
    st->st_gid = 0;
    return 0;
}
#define lstat(path, st) win32_lstat(path, st)

// realpath compatibility
static inline char *win32_realpath(const char *rel, char *abs) {
    char temp[PATH_MAX];
    char *res = _fullpath(temp, rel, PATH_MAX);
    if (res == NULL) return NULL;
    
    // Check if the path exists
    DWORD attr = GetFileAttributesA(temp);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        errno = ENOENT;
        return NULL;
    }
    
    if (abs != NULL) {
        strncpy(abs, temp, PATH_MAX);
        abs[PATH_MAX - 1] = '\0';
        return abs;
    } else {
        return _strdup(temp);
    }
}
#define realpath(rel, abs) win32_realpath(rel, abs)

// getline compatibility
static inline ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (lineptr == NULL || n == NULL || stream == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (*lineptr == NULL || *n == 0) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (*lineptr == NULL) return -1;
    }
    size_t pos = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (pos + 1 >= *n) {
            size_t new_n = *n * 2;
            char *new_ptr = (char *)realloc(*lineptr, new_n);
            if (new_ptr == NULL) return -1;
            *lineptr = new_ptr;
            *n = new_n;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == '\n') break;
    }
    if (pos == 0 && c == EOF) return -1;
    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}

#define chmod(path, mode) _chmod(path, mode)
#define lchown(path, uid, gid) (0)

// statvfs compatibility. WinFSP-FUSE3 uses this to answer Windows'
// FileFsSizeInformation queries; without it WinFSP reports zero free bytes
// and every write fails with ERROR_DISK_FULL. The host volume queried here
// is the overlay root, not the virtual FUSE path the kernel supplies.
// On WinFSP the relevant struct is `struct fuse_statvfs` (WinFSP's own
// definition, not the POSIX `struct statvfs`).
typedef struct fuse_statvfs gbfs_statvfs_t;
static inline int gbfs_statvfs(const char *path, gbfs_statvfs_t *st) {
    if (path == NULL || st == NULL) {
        errno = EINVAL;
        return -1;
    }

    ULARGE_INTEGER free_avail, total_bytes, free_total;
    if (!GetDiskFreeSpaceExA(path, &free_avail, &total_bytes, &free_total)) {
        errno = EIO;
        return -1;
    }
    DWORD sectors_per_cluster = 0, bytes_per_sector = 0;
    DWORD free_clusters = 0, total_clusters = 0;
    if (!GetDiskFreeSpaceA(path, &sectors_per_cluster, &bytes_per_sector,
                           &free_clusters, &total_clusters)) {
        errno = EIO;
        return -1;
    }
    unsigned long long cluster_size =
        (unsigned long long)sectors_per_cluster * (unsigned long long)bytes_per_sector;
    if (cluster_size == 0) {
        errno = EIO;
        return -1;
    }

    memset(st, 0, sizeof(*st));
    st->f_bsize = cluster_size;
    st->f_frsize = cluster_size;
    st->f_blocks = (fuse_fsblkcnt_t)total_clusters;
    st->f_bfree = (fuse_fsblkcnt_t)free_clusters;
    st->f_bavail = (fuse_fsblkcnt_t)((unsigned long long)free_avail.QuadPart / cluster_size);
    // Windows has no inode model; cap directory-entry counts so writes are
    // never gated on inode exhaustion.
    st->f_files = (fuse_fsfilcnt_t)0x40000000ULL;
    st->f_ffree = st->f_files;
    st->f_favail = st->f_files;
    st->f_namemax = MAX_PATH;
    return 0;
}

#define rename(src, dst) win32_rename(src, dst)
static inline int win32_rename(const char *src, const char *dst) {
    if (MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        return 0;
    }
    DWORD err = GetLastError();
    switch (err) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            errno = ENOENT;
            break;
        case ERROR_ACCESS_DENIED:
            errno = EACCES;
            break;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            errno = EEXIST;
            break;
        default:
            errno = EIO;
            break;
    }
    return -1;
}

#else // !defined(_WIN32)

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>

#ifndef FUSE_USE_VERSION
#define FUSE_USE_VERSION 31
#endif
#include <fuse.h>

#define PATH_SEP '/'
#define PATH_SEP_STR "/"

#define O_BINARY 0

typedef struct stat gbfs_stat_t;
typedef struct timespec gbfs_timespec_t;
typedef off_t gbfs_off_t;
typedef mode_t gbfs_mode_t;

static inline void gbfs_stat_set_times(gbfs_stat_t *st, time_t t) {
    st->st_atime = t;
    st->st_mtime = t;
    st->st_ctime = t;
}

// statvfs against the overlay host volume. libfuse3 on Linux does not gate
// writes on statfs, but reporting a sensible value keeps `df` honest and is
// harmless.
typedef struct statvfs gbfs_statvfs_t;
static inline int gbfs_statvfs(const char *path, gbfs_statvfs_t *st) {
    return statvfs(path, st);
}

#endif // _WIN32

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

#endif // COMPAT_H
