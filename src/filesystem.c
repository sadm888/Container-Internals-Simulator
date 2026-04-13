#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "filesystem.h"

#define OLD_ROOT_DIR ".old_root"

static int ensure_directory(const char *path, mode_t mode) {
    struct stat st;

    if (stat(path, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        return 0;
    }

    if (mkdir(path, mode) == 0) {
        return 0;
    }

    if (errno == EEXIST) {
        return ensure_directory(path, mode);
    }

    return -1;
}

static int mkdir_p(const char *path, mode_t mode) {
    char buffer[PATH_MAX];
    size_t length;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (strlen(path) >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    snprintf(buffer, sizeof(buffer), "%s", path);
    length = strlen(buffer);
    if (length > 1 && buffer[length - 1] == '/') {
        buffer[length - 1] = '\0';
    }

    for (char *cursor = buffer + 1; *cursor != '\0'; cursor++) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        if (ensure_directory(buffer, mode) != 0) {
            return -1;
        }
        *cursor = '/';
    }

    return ensure_directory(buffer, mode);
}

static int create_layout(const char *rootfs) {
    char path[PATH_MAX];
    const char *dirs[] = {
        "proc",
        "tmp",
        "dev",
        OLD_ROOT_DIR
    };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        if (snprintf(path, sizeof(path), "%s/%s", rootfs, dirs[i]) >= (int)sizeof(path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (mkdir_p(path, 0755) != 0) {
            return -1;
        }
    }

    return 0;
}

static int path_join(char *buffer, size_t buffer_size, const char *left, const char *right) {
    if (snprintf(buffer, buffer_size, "%s/%s", left, right) >= (int)buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int enter_root_with_pivot_root(const char *rootfs) {
    char old_root_path[PATH_MAX];

    if (mount(rootfs, rootfs, NULL, MS_BIND | MS_REC, NULL) != 0) {
        return -1;
    }

    if (path_join(old_root_path, sizeof(old_root_path), rootfs, OLD_ROOT_DIR) != 0) {
        return -1;
    }

    if (mkdir_p(old_root_path, 0755) != 0) {
        return -1;
    }

    if (chdir(rootfs) != 0) {
        return -1;
    }

    if (syscall(SYS_pivot_root, ".", OLD_ROOT_DIR) != 0) {
        return -1;
    }

    if (chdir("/") != 0) {
        return -1;
    }

    if (umount2("/" OLD_ROOT_DIR, MNT_DETACH) != 0) {
        return -1;
    }

    if (rmdir("/" OLD_ROOT_DIR) != 0) {
        return -1;
    }

    return 0;
}

static int enter_root_with_chroot(const char *rootfs) {
    if (chdir(rootfs) != 0) {
        return -1;
    }

    if (chroot(".") != 0) {
        return -1;
    }

    if (chdir("/") != 0) {
        return -1;
    }

    return 0;
}

int filesystem_prepare_rootfs(const char *requested_rootfs,
                              char *resolved_rootfs,
                              size_t resolved_rootfs_size) {
    char absolute_rootfs[PATH_MAX];

    if (requested_rootfs == NULL || requested_rootfs[0] == '\0' ||
        resolved_rootfs == NULL || resolved_rootfs_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (mkdir_p(requested_rootfs, 0755) != 0) {
        return -1;
    }

    if (realpath(requested_rootfs, absolute_rootfs) == NULL) {
        return -1;
    }

    if (create_layout(absolute_rootfs) != 0) {
        return -1;
    }

    if (snprintf(resolved_rootfs, resolved_rootfs_size, "%s", absolute_rootfs) >=
        (int)resolved_rootfs_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

int filesystem_isolate_rootfs(const FilesystemConfig *config) {
    if (config == NULL || config->rootfs == NULL || config->rootfs[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        if (errno != EPERM) {
            return -1;
        }
    }

    if (enter_root_with_pivot_root(config->rootfs) != 0) {
        if (errno != EPERM && errno != EINVAL) {
            return -1;
        }
        if (enter_root_with_chroot(config->rootfs) != 0) {
            return -1;
        }
    }

    if (mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
        if (errno != EPERM) {
            return -1;
        }
    }

    return 0;
}

const char *filesystem_profile(void) {
    return FILESYSTEM_PROFILE;
}

void filesystem_format_error(int err, char *buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    switch (err) {
        case ENOENT:
            snprintf(buffer, buffer_size, "rootfs path is missing or incomplete");
            break;
        case ENOTDIR:
            snprintf(buffer, buffer_size, "rootfs path exists but is not a directory");
            break;
        case ENAMETOOLONG:
            snprintf(buffer, buffer_size, "rootfs path is too long for the current runtime limits");
            break;
        case EBUSY:
            snprintf(buffer, buffer_size, "pivot_root could not detach the old root cleanly");
            break;
        case EPERM:
            snprintf(buffer, buffer_size, "filesystem isolation needs privileges the current host denied");
            break;
        case EINVAL:
            snprintf(buffer, buffer_size, "filesystem isolation received an invalid rootfs configuration");
            break;
        default:
            snprintf(buffer, buffer_size, "%s", strerror(err));
            break;
    }
}
