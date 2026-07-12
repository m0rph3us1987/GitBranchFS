#define _GNU_SOURCE
#include "compat.h"
#include "cli.h"
#include "core_fs.h"
#include "fuse_ops.h"
#include "git_wrapper.h"
#include "settings.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#ifndef _WIN32
#include <unistd.h>
#include <sys/wait.h>
#endif

// Prompt the user whether to create the mount-point directory.
// Default is yes: an empty answer, EOF, or a leading 'y'/'Y' returns 1;
// a leading 'n'/'N' returns 0.
static int prompt_create_dir(const char *path) {
    printf("Mount point '%s' does not exist. Create it? [Y/n] ", path);
    fflush(stdout);

    char buf[16];
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        printf("\n");
        return 1; // EOF -> default yes
    }
    for (const char *p = buf; *p != '\0'; p++) {
        if (*p == ' ' || *p == '\t') continue;
        if (*p == '\n' || *p == '\r') break; // empty line -> default yes
        return (*p != 'n' && *p != 'N');
    }
    return 1;
}

// Return 1 if the directory is empty, 0 if it contains entries other than
// "." / "..", or -1 if it could not be opened.
static int dir_is_empty(const char *path) {
    DIR *d = opendir(path);
    if (d == NULL) return -1;

    int empty = 1;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        empty = 0;
        break;
    }
    closedir(d);
    return empty;
}

// Return 1 if `child` is `parent` itself or a descendant of `parent`.
// Both paths must be canonical absolute paths (no trailing slash except root).
static int path_is_within(const char *child, const char *parent) {
    size_t plen = strlen(parent);
    if (strncmp(child, parent, plen) != 0) return 0;
    if (child[plen] == '\0') return 1;            // child == parent
    if (plen > 0 && parent[plen - 1] == '/') return 1; // parent is root "/"
    if (child[plen] == '/') return 1;             // proper descendant
    return 0;
}

static char *get_repo_name(const char *repo_path) {
    char *path_copy = strdup(repo_path);
    if (path_copy == NULL) return NULL;
    
    size_t len = strlen(path_copy);
    while (len > 1 && (path_copy[len - 1] == '/' || path_copy[len - 1] == '\\')) {
        path_copy[len - 1] = '\0';
        len--;
    }
    
    char *base = strrchr(path_copy, '/');
#ifdef _WIN32
    char *base_win = strrchr(path_copy, '\\');
    if (base_win > base) base = base_win;
#endif
    char *repo_name = NULL;
    if (base != NULL) {
        repo_name = strdup(base + 1);
    } else {
        repo_name = strdup(path_copy);
    }
    free(path_copy);
    return repo_name;
}

static void print_usage(const char *prog) {
    printf("Usage:\n");
    printf("  %s [mount] <branch-name> <mount-point> [-d | --background]\n", prog);
    printf("  %s unmount <mount-point>\n", prog);
    printf("\nThe 'mount' keyword is optional: two positional arguments are treated as a mount.\n");
    printf("The 'mount' command uses the current working directory as the git repository.\n");
    printf("Use -d or --background to run the filesystem process in the background.\n");
}

#define APP_NAME    "GitBranchFS"
#define APP_VERSION "1.0.0"
#define APP_AUTHOR  "m0rph3us1987"

#ifdef _WIN32
static int daemonize_windows(int argc, char *argv[]) {
    char cmdline[32768] = {0};
    size_t offset = 0;
    
    char exepath[PATH_MAX];
    GetModuleFileNameA(NULL, exepath, PATH_MAX);
    
    offset += snprintf(cmdline + offset, sizeof(cmdline) - offset, "\"%s\"", exepath);
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--background") == 0) {
            continue;
        }
        offset += snprintf(cmdline + offset, sizeof(cmdline) - offset, " \"%s\"", argv[i]);
    }
    
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    
    HANDLE hLog = CreateFileA(
        "gbfs_daemon.log",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    
    if (hLog != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hLog;
        si.hStdError = hLog;
        si.hStdInput = INVALID_HANDLE_VALUE;
    }
    
    BOOL success = CreateProcessA(
        NULL,
        cmdline,
        NULL,
        NULL,
        TRUE, // Inherit hLog handle
        CREATE_NO_WINDOW | DETACHED_PROCESS,
        NULL,
        NULL,
        &si,
        &pi
    );
    
    if (hLog != INVALID_HANDLE_VALUE) {
        CloseHandle(hLog);
    }
    
    if (!success) {
        fprintf(stderr, "Error: Failed to spawn background process (Error code: %lu)\n", GetLastError());
        return -1;
    }
    
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

static int find_pid_file(const char *overlay_root, const char *mount_id,
                        char *out_pid_path, size_t out_size) {
    const char *root_rel = (overlay_root != NULL) ? overlay_root : "~/.gitbranchfs";
    char *home_abs = resolve_home_path(root_rel);
    if (home_abs == NULL) return -1;
    
    DIR *dir_repo = opendir(home_abs);
    if (dir_repo == NULL) {
        free(home_abs);
        return -1;
    }
    
    int found = 0;
    struct dirent *ent_repo;
    while ((ent_repo = readdir(dir_repo)) != NULL) {
        if (strcmp(ent_repo->d_name, ".") == 0 || strcmp(ent_repo->d_name, "..") == 0) {
            continue;
        }
        
        char path_repo[PATH_MAX];
        snprintf(path_repo, sizeof(path_repo), "%s/%s", home_abs, ent_repo->d_name);
        
        DIR *dir_branch = opendir(path_repo);
        if (dir_branch == NULL) continue;
        
        struct dirent *ent_branch;
        while ((ent_branch = readdir(dir_branch)) != NULL) {
            if (strcmp(ent_branch->d_name, ".") == 0 || strcmp(ent_branch->d_name, "..") == 0) {
                continue;
            }
            
            char path_branch[PATH_MAX];
            snprintf(path_branch, sizeof(path_branch), "%s/%s", path_repo, ent_branch->d_name);
            
            DIR *dir_mount = opendir(path_branch);
            if (dir_mount == NULL) continue;
            
            struct dirent *ent_mount;
            while ((ent_mount = readdir(dir_mount)) != NULL) {
                if (strcmp(ent_mount->d_name, mount_id) == 0) {
                    char pid_file[PATH_MAX];
                    snprintf(pid_file, sizeof(pid_file), "%s/%s/gbfs.pid", path_branch, ent_mount->d_name);
                    
                    struct stat st;
                    if (stat(pid_file, &st) == 0) {
                        snprintf(out_pid_path, out_size, "%s", pid_file);
                        found = 1;
                        break;
                    }
                }
            }
            closedir(dir_mount);
            if (found) break;
        }
        closedir(dir_branch);
        if (found) break;
    }
    closedir(dir_repo);
    free(home_abs);
    return found ? 0 : -1;
}
#endif

int cli_run(int argc, char *argv[]) {
    int background = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--background") == 0) {
            background = 1;
            break;
        }
    }
    (void)background;

#ifdef _WIN32
    if (background) {
        printf("%s version %s\n", APP_NAME, APP_VERSION);
        printf("Author: %s\n", APP_AUTHOR);
        printf("Starting GitBranchFS in the background...\n");
        if (daemonize_windows(argc, argv) == 0) {
            return 0;
        } else {
            return 1;
        }
    }
#endif

    char *clean_argv[64];
    int clean_argc = 0;
    for (int i = 0; i < argc && clean_argc < 60; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--background") == 0) {
            continue;
        }
        clean_argv[clean_argc++] = argv[i];
    }
    clean_argv[clean_argc] = NULL;

    printf("%s version %s\n", APP_NAME, APP_VERSION);
    printf("Author: %s\n", APP_AUTHOR);

    gbfs_settings_t settings;
    gbfs_settings_load(&settings);
    if (settings.overlay_root != NULL) {
        printf("Overlay root (from settings): %s\n", settings.overlay_root);
    } else {
        printf("Overlay root (default):       ~/.gitbranchfs\n");
    }
    fflush(stdout);

    if (clean_argc < 2) {
        print_usage(clean_argv[0]);
        gbfs_settings_free(&settings);
        return 1;
    }

    const char *command = clean_argv[1];

    int implicit_mount = (strcmp(command, "mount") != 0 &&
                          strcmp(command, "unmount") != 0 &&
                          clean_argc == 3);

    int ret = 1;
    do {
    if (strcmp(command, "mount") == 0 || implicit_mount) {
        const char *branch;
        const char *mount_raw;
        if (implicit_mount) {
            branch = clean_argv[1];
            mount_raw = clean_argv[2];
        } else {
            if (clean_argc != 4) {
                fprintf(stderr, "Error: 'mount' requires exactly 2 arguments.\n");
                print_usage(clean_argv[0]);
                ret = 2; break;
            }
            branch = clean_argv[2];
            mount_raw = clean_argv[3];
        }

        if (branch[0] == '\0') {
            fprintf(stderr, "Error: Branch name must not be empty.\n");
            ret = 1; break;
        }
        if (strchr(branch, '/') != NULL) {
            fprintf(stderr, "Error: Branch name must not contain '/'.\n");
            ret = 1; break;
        }
        if (strstr(branch, "..") != NULL) {
            fprintf(stderr, "Error: Branch name must not contain '..'.\n");
            ret = 1; break;
        }

        char repo_path[PATH_MAX];
        if (getcwd(repo_path, sizeof(repo_path)) == NULL) {
            fprintf(stderr, "Error: Failed to determine the current working directory.\n");
            ret = 1; break;
        }

        if (git_wrapper_is_repo(repo_path) != 0) {
            fprintf(stderr, "Error: Current directory '%s' is not a git repository.\n", repo_path);
            ret = 1; break;
        }

        char mount_path[PATH_MAX];
#ifdef _WIN32
        if (realpath(mount_raw, mount_path) == NULL) {
            char *parent = strdup(mount_raw);
            if (parent != NULL) {
                size_t plen = strlen(parent);
                while (plen > 1 && (parent[plen - 1] == '/' || parent[plen - 1] == '\\')) {
                    parent[plen - 1] = '\0';
                    plen--;
                }
                char *last_slash = strrchr(parent, '\\');
                if (last_slash == NULL) last_slash = strrchr(parent, '/');
                if (last_slash != NULL) {
                    *last_slash = '\0';
                    size_t parent_len = strlen(parent);
                    int is_root = (parent_len == 0 || 
                                   (parent_len == 1 && (parent[0] == '/' || parent[0] == '\\')) ||
                                   (parent_len == 2 && parent[1] == ':') ||
                                   (parent_len == 3 && parent[1] == ':' && (parent[2] == '/' || parent[2] == '\\')));
                    if (!is_root && !dir_exists(parent)) {
                        if (!prompt_create_dir(parent)) {
                            fprintf(stderr, "Aborted: Parent directory '%s' was not created.\n", parent);
                            free(parent);
                            ret = 1; break;
                        }
                        if (mkdir_rec(parent) != 0) {
                            fprintf(stderr, "Error: Parent directory '%s' could not be created.\n", parent);
                            free(parent);
                            ret = 1; break;
                        }
                    }
                }
                free(parent);
            }
            if (_fullpath(mount_path, mount_raw, PATH_MAX) == NULL) {
                fprintf(stderr, "Error: Failed to resolve mount point '%s'.\n", mount_raw);
                ret = 1; break;
            }
        } else {
            size_t mlen = strlen(mount_path);
            int is_drive = (mlen >= 2 && mount_path[1] == ':' && (mount_path[2] == '\0' || (mount_path[2] == '\\' && mount_path[3] == '\0')));
            if (!is_drive) {
                if (!dir_exists(mount_path)) {
                    fprintf(stderr, "Error: Mount point '%s' exists but is not a directory.\n", mount_path);
                    ret = 1; break;
                }
                int empty = dir_is_empty(mount_path);
                if (empty == 0) {
                    fprintf(stderr, "Error: Mount point '%s' is not empty.\n", mount_path);
                    ret = 1; break;
                } else if (empty < 0) {
                    fprintf(stderr, "Error: Mount point '%s' could not be read.\n", mount_path);
                    ret = 1; break;
                }
                if (rmdir(mount_path) != 0) {
                    fprintf(stderr, "Error: Mount point '%s' exists and is empty, but could not be removed.\n", mount_path);
                    ret = 1; break;
                }
            }
        }
#else
        int mount_created = 0;
        if (realpath(mount_raw, mount_path) == NULL) {
            if (!prompt_create_dir(mount_raw)) {
                fprintf(stderr, "Aborted: mount point '%s' was not created.\n", mount_raw);
                ret = 1; break;
            }
            if (mkdir_rec(mount_raw) != 0) {
                fprintf(stderr, "Error: Mount point '%s' could not be created.\n", mount_raw);
                ret = 1; break;
            }
            mount_created = 1;
            if (realpath(mount_raw, mount_path) == NULL) {
                fprintf(stderr, "Error: Failed to resolve mount point '%s'.\n", mount_raw);
                ret = 1; break;
            }
        } else if (!dir_exists(mount_path)) {
            fprintf(stderr, "Error: Mount point '%s' exists but is not a directory.\n", mount_path);
            ret = 1; break;
        }

        if (!mount_created) {
            int empty = dir_is_empty(mount_path);
            if (empty == 0) {
                fprintf(stderr, "Error: Mount point '%s' is not empty.\n", mount_path);
                ret = 1; break;
            } else if (empty < 0) {
                fprintf(stderr, "Error: Mount point '%s' could not be read.\n", mount_path);
                ret = 1; break;
            }
        }
#endif

        if (path_is_within(mount_path, repo_path)) {
            fprintf(stderr, "Error: Mount point '%s' must not be inside the git repository '%s'.\n",
                    mount_path, repo_path);
#ifndef _WIN32
            if (mount_created) rmdir(mount_path);
#endif
            ret = 1; break;
        }

        char *repo_name = get_repo_name(repo_path);
        if (repo_name == NULL) {
            fprintf(stderr, "Error: Failed to parse repository name.\n");
            ret = 1; break;
        }

        char *mount_id = derive_mount_id(mount_path);
        if (mount_id == NULL) {
            free(repo_name);
            fprintf(stderr, "Error: Failed to derive mount identifier.\n");
            ret = 1; break;
        }

char overlay_rel[PATH_MAX];
        const char *root_prefix = (settings.overlay_root != NULL)
                                      ? settings.overlay_root
                                      : "~/.gitbranchfs";
        int n = snprintf(overlay_rel, sizeof(overlay_rel), "%s/%s/%s/%s",
                         root_prefix, repo_name, branch, mount_id);
        free(repo_name);
        free(mount_id);
        if (n < 0 || (size_t)n >= sizeof(overlay_rel)) {
            fprintf(stderr, "Error: Overlay path is too long.\n");
            ret = 1; break;
        }

        char *overlay_path = resolve_home_path(overlay_rel);
        if (overlay_path == NULL) {
            fprintf(stderr, "Error: Failed to resolve overlay home directory path.\n");
            ret = 1; break;
        }

        printf("Initializing GitBranchFS:\n");
        printf("  Git Repository: %s\n", repo_path);
        printf("  Branch:         %s\n", branch);
        printf("  Mount Point:    %s\n", mount_path);
        printf("  Overlay Path:   %s\n", overlay_path);
        fflush(stdout);

        gbfs_state_t *state = gbfs_init(repo_path, branch, overlay_path);
        if (state == NULL) {
            fprintf(stderr, "Error: Failed to initialize GitBranchFS state. Check repo path and branch name.\n");
            fflush(stderr);
            ret = 1; break;
        }

        char pid_file[PATH_MAX];
        snprintf(pid_file, sizeof(pid_file), "%s/gbfs.pid", overlay_path);
        FILE *fpid = fopen(pid_file, "w");
        if (fpid != NULL) {
#ifdef _WIN32
            fprintf(fpid, "%lu\n", GetCurrentProcessId());
#else
            fprintf(fpid, "%d\n", getpid());
#endif
            fclose(fpid);
        }

        free(overlay_path);


        const char *resolved = gbfs_get_resolved_branch(state);
        if (resolved != NULL && strcmp(resolved, branch) != 0) {
            printf("  -> NOTE: Resolved to branch: %s (fallback)\n", resolved);
            fflush(stdout);
        }

#ifdef _WIN32
        char *fuse_argv[6];
        fuse_argv[0] = clean_argv[0];
        fuse_argv[1] = mount_path;
        fuse_argv[2] = "-s";
        fuse_argv[3] = "-ouid=-1";
        fuse_argv[4] = "-ogid=-1";
        int fuse_argc = 5;
#else
        char *fuse_argv[5];
        fuse_argv[0] = clean_argv[0];
        fuse_argv[1] = mount_path;
        fuse_argv[2] = "-s";
        fuse_argv[3] = "-odefault_permissions";
        int fuse_argc = 4;
#endif

        printf("Mounting filesystem...\n");
        fflush(stdout);
        ret = run_fuse_fs(fuse_argc, fuse_argv, state);

        gbfs_destroy(state);
        unlink(pid_file);
        break;

    } else if (strcmp(command, "unmount") == 0) {
        if (clean_argc != 3) {
            fprintf(stderr, "Error: 'unmount' requires exactly 1 argument.\n");
            print_usage(clean_argv[0]);
            ret = 2; break;
        }

        const char *mount_raw = clean_argv[2];
        char mount_path[PATH_MAX];
        if (realpath(mount_raw, mount_path) == NULL) {
            size_t raw_len = strlen(mount_raw);
            if (raw_len >= PATH_MAX) {
                fprintf(stderr, "Error: Mount point path is too long.\n");
                ret = 1; break;
            }
            memcpy(mount_path, mount_raw, raw_len + 1);
        }

        printf("Unmounting %s...\n", mount_path);
#ifdef _WIN32
        size_t len = strlen(mount_path);
        int is_drive = (len == 2 && mount_path[1] == ':') ||
                       (len == 3 && mount_path[1] == ':' && (mount_path[2] == '/' || mount_path[2] == '\\'));
        if (is_drive) {
            char drive[3] = { mount_path[0], ':', '\0' };
            printf("Attempting to unmount drive %s via 'net use'...\n", drive);
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "net use %s /delete", drive);
            int system_ret = system(cmd);
            if (system_ret == 0) {
                printf("Unmounted successfully.\n");
                ret = 0; break;
            }
            ret = 1; break;
        }

        char *mount_id = derive_mount_id(mount_path);
        if (mount_id != NULL) {
            char pid_file[PATH_MAX];
            if (find_pid_file(settings.overlay_root, mount_id, pid_file, sizeof(pid_file)) == 0) {
                FILE *fpid = fopen(pid_file, "r");
                if (fpid != NULL) {
                    DWORD pid = 0;
                    if (fscanf(fpid, "%lu", &pid) == 1 && pid != 0) {
                        fclose(fpid);
                        printf("Found running gbfs process with PID %lu. Terminating...\n", pid);
                        HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                        if (hProc != NULL) {
                            if (TerminateProcess(hProc, 0)) {
                                CloseHandle(hProc);
                                printf("Unmounted successfully.\n");
                                unlink(pid_file);
                                free(mount_id);
                                ret = 0; break;
                            } else {
                                DWORD err = GetLastError();
                                CloseHandle(hProc);
                                fprintf(stderr, "Error: Failed to terminate process %lu (Error: %lu)\n", pid, err);
                            }
                        } else {
                            DWORD err = GetLastError();
                            if (err == ERROR_INVALID_PARAMETER) {
                                printf("Process %lu is not running. Cleaning up stale metadata...\n", pid);
                                unlink(pid_file);
                                free(mount_id);
                                ret = 0; break;
                            } else {
                                fprintf(stderr, "Error: Failed to open process %lu (Error: %lu)\n", pid, err);
                            }
                        }
                    } else {
                        fclose(fpid);
                    }
                }
            }
            free(mount_id);
        }
        fprintf(stderr, "Error: Could not find or terminate the running gbfs process for '%s'.\n", mount_path);
        ret = 1; break;
#else
        pid_t pid = fork();
        if (pid < 0) {
            perror("Error: fork failed");
            ret = 1; break;
        }
        if (pid == 0) {
            char *const args[] = {
                (char *)"fusermount3",
                (char *)"-u",
                mount_path,
                NULL,
            };
            execv("/usr/bin/fusermount3", args);
            perror("Error: execv failed");
            _exit(127);
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            perror("Error: waitpid failed");
            ret = 1; break;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("Unmounted successfully.\n");
            ret = 0; break;
        } else {
            fprintf(stderr, "Error: Failed to unmount %s.\n", mount_path);
            ret = 1; break;
        }
#endif
    } else {
        fprintf(stderr, "Error: Unknown command '%s'.\n", command);
        print_usage(clean_argv[0]);
        ret = 1; break;
    }
    } while (0);
    gbfs_settings_free(&settings);
    return ret;
}
