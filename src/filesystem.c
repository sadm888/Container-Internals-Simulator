#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "filesystem.h"

#define OLD_ROOT_DIR ".old_root"

static const char *bootstrap_binaries[] = {
    "/bin/sh",
    "/bin/echo",
    "/bin/hostname",
    "/bin/ps",
    "/bin/sleep"
};

static int path_dirname(const char *path, char *buffer, size_t buffer_size);

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
        "bin",
        "lib",
        "lib64",
        "usr",
        "usr/bin",
        "usr/lib",
        "usr/lib64",
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

static int copy_file_contents(int src_fd, int dst_fd) {
    char buffer[8192];

    while (1) {
        ssize_t bytes_read = read(src_fd, buffer, sizeof(buffer));

        if (bytes_read == 0) {
            return 0;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        for (ssize_t offset = 0; offset < bytes_read;) {
            ssize_t bytes_written = write(dst_fd, buffer + offset, (size_t)(bytes_read - offset));

            if (bytes_written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }

            offset += bytes_written;
        }
    }
}

static int copy_regular_file(const char *src, const char *dst) {
    int src_fd = -1;
    int dst_fd = -1;
    struct stat st;
    int saved_errno = 0;

    if (stat(src, &st) != 0) {
        return -1;
    }

    src_fd = open(src, O_RDONLY);
    if (src_fd < 0) {
        return -1;
    }

    dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (dst_fd < 0) {
        saved_errno = errno;
        close(src_fd);
        errno = saved_errno;
        return -1;
    }

    if (copy_file_contents(src_fd, dst_fd) != 0) {
        saved_errno = errno;
        close(src_fd);
        close(dst_fd);
        errno = saved_errno;
        return -1;
    }

    if (close(src_fd) != 0) {
        saved_errno = errno;
        close(dst_fd);
        errno = saved_errno;
        return -1;
    }

    if (close(dst_fd) != 0) {
        return -1;
    }

    return 0;
}

static int copy_symlink(const char *src, const char *dst) {
    char target[PATH_MAX];
    ssize_t length = readlink(src, target, sizeof(target) - 1);

    if (length < 0) {
        return -1;
    }

    target[length] = '\0';

    if (unlink(dst) != 0 && errno != ENOENT) {
        return -1;
    }

    if (symlink(target, dst) != 0) {
        return -1;
    }

    return 0;
}

static int resolve_symlink_target(const char *link_path, const char *target, char *resolved, size_t resolved_size) {
    char parent[PATH_MAX];
    const char *slash = NULL;
    size_t prefix_length = 0;

    if (link_path == NULL || target == NULL || resolved == NULL || resolved_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (target[0] == '/') {
        if (snprintf(resolved, resolved_size, "%s", target) >= (int)resolved_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }

    if (path_dirname(link_path, parent, sizeof(parent)) != 0) {
        return -1;
    }

    slash = (parent[0] == '/' && parent[1] == '\0') ? "" : "/";
    prefix_length = strlen(parent);
    if (snprintf(resolved, resolved_size, "%s%s%s", parent, slash, target) >= (int)resolved_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (prefix_length == 0) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int path_dirname(const char *path, char *buffer, size_t buffer_size) {
    const char *slash = NULL;
    size_t length = 0;

    if (path == NULL || path[0] == '\0' || buffer == NULL || buffer_size == 0) {
        errno = EINVAL;
        return -1;
    }

    slash = strrchr(path, '/');
    if (slash == NULL) {
        errno = EINVAL;
        return -1;
    }

    length = (size_t)(slash - path);
    if (length == 0) {
        length = 1;
    }
    if (length >= buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(buffer, path, length);
    buffer[length] = '\0';
    return 0;
}

static int copy_path_into_rootfs(const char *rootfs, const char *host_path) {
    char destination[PATH_MAX];
    char destination_dir[PATH_MAX];
    struct stat st;

    if (rootfs == NULL || host_path == NULL || host_path[0] != '/') {
        errno = EINVAL;
        return -1;
    }

    if (snprintf(destination, sizeof(destination), "%s%s", rootfs, host_path) >= (int)sizeof(destination)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (path_dirname(destination, destination_dir, sizeof(destination_dir)) != 0) {
        return -1;
    }

    if (mkdir_p(destination_dir, 0755) != 0) {
        return -1;
    }

    if (lstat(host_path, &st) != 0) {
        return -1;
    }

    if (S_ISLNK(st.st_mode)) {
        char link_target[PATH_MAX];
        char resolved_target[PATH_MAX];
        ssize_t length = readlink(host_path, link_target, sizeof(link_target) - 1);

        if (length < 0) {
            return -1;
        }

        link_target[length] = '\0';

        if (copy_symlink(host_path, destination) != 0) {
            return -1;
        }

        if (resolve_symlink_target(host_path, link_target, resolved_target, sizeof(resolved_target)) != 0) {
            return -1;
        }

        return copy_path_into_rootfs(rootfs, resolved_target);
    }

    if (!S_ISREG(st.st_mode)) {
        errno = EINVAL;
        return -1;
    }

    return copy_regular_file(host_path, destination);
}

static int parse_ldd_dependency(const char *line, char *path, size_t path_size) {
    const char *start = NULL;
    const char *end = NULL;
    size_t length = 0;

    if (line == NULL || path == NULL || path_size == 0) {
        errno = EINVAL;
        return -1;
    }

    if (strstr(line, "=> not found") != NULL) {
        errno = ENOENT;
        return -1;
    }

    start = strstr(line, "=> ");
    if (start != NULL) {
        start += 3;
        while (*start == ' ') {
            start++;
        }
    } else {
        start = line;
        while (*start == ' ' || *start == '\t') {
            start++;
        }
    }

    if (*start != '/') {
        errno = EINVAL;
        return -1;
    }

    end = start;
    while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n') {
        end++;
    }

    length = (size_t)(end - start);
    if (length == 0 || length >= path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(path, start, length);
    path[length] = '\0';
    return 0;
}

static int copy_binary_dependencies(const char *rootfs, const char *binary_path) {
    char command[PATH_MAX + 16];
    char line[PATH_MAX * 2];
    FILE *pipe = NULL;
    int status = 0;
    int saved_errno = 0;

    if (snprintf(command, sizeof(command), "ldd %s", binary_path) >= (int)sizeof(command)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    pipe = popen(command, "r");
    if (pipe == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), pipe) != NULL) {
        char dependency[PATH_MAX];

        if (parse_ldd_dependency(line, dependency, sizeof(dependency)) != 0) {
            if (errno == EINVAL) {
                continue;
            }
            saved_errno = errno;
            pclose(pipe);
            errno = saved_errno;
            return -1;
        }

        if (copy_path_into_rootfs(rootfs, dependency) != 0) {
            saved_errno = errno;
            pclose(pipe);
            errno = saved_errno;
            return -1;
        }
    }

    status = pclose(pipe);
    if (status == -1) {
        return -1;
    }

    return 0;
}

static int bootstrap_rootfs_tools(const char *rootfs) {
    for (size_t i = 0; i < sizeof(bootstrap_binaries) / sizeof(bootstrap_binaries[0]); i++) {
        if (copy_path_into_rootfs(rootfs, bootstrap_binaries[i]) != 0) {
            return -1;
        }

        if (copy_binary_dependencies(rootfs, bootstrap_binaries[i]) != 0) {
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

    if (bootstrap_rootfs_tools(absolute_rootfs) != 0) {
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
        case ENOEXEC:
            snprintf(buffer, buffer_size, "rootfs bootstrap could not prepare runnable container tools");
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
