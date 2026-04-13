#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "filesystem.h"
#include "namespace.h"

typedef struct {
    int   ready;
    int   error_number;
    pid_t namespace_pid;
} NamespaceStatus;

typedef struct {
    const char *hostname;
    const char *rootfs;
    int         status_fd;
    int         continue_fd;
} NamespaceChildArgs;

static int write_text_file(const char *path, const char *content) {
    FILE *file = fopen(path, "w");

    if (file == NULL) {
        return -1;
    }

    if (fputs(content, file) == EOF) {
        int saved_errno = errno;

        fclose(file);
        errno = saved_errno;
        return -1;
    }

    if (fclose(file) != 0) {
        return -1;
    }

    return 0;
}

static int configure_user_namespace(pid_t pid) {
    char path[128];
    char mapping[128];

    snprintf(path, sizeof(path), "/proc/%d/setgroups", (int)pid);
    if (write_text_file(path, "deny\n") != 0 && errno != ENOENT) {
        return -1;
    }

    snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)pid);
    snprintf(mapping, sizeof(mapping), "0 %d 1\n", (int)geteuid());
    if (write_text_file(path, mapping) != 0) {
        return -1;
    }

    snprintf(path, sizeof(path), "/proc/%d/gid_map", (int)pid);
    snprintf(mapping, sizeof(mapping), "0 %d 1\n", (int)getegid());
    if (write_text_file(path, mapping) != 0) {
        return -1;
    }

    return 0;
}

static int wait_for_parent_ready(int fd) {
    char token = '\0';

    while (1) {
        ssize_t bytes_read = read(fd, &token, 1);

        if (bytes_read > 0) {
            close(fd);
            return (token == '1') ? 0 : -1;
        }
        if (bytes_read == 0) {
            close(fd);
            errno = EPROTO;
            return -1;
        }
        if (errno != EINTR) {
            close(fd);
            return -1;
        }
    }
}

static int namespace_child(void *arg) {
    FilesystemConfig filesystem_config;
    NamespaceChildArgs *child_args = arg;
    NamespaceStatus status;

    memset(&status, 0, sizeof(status));
    status.namespace_pid = -1;

    if (wait_for_parent_ready(child_args->continue_fd) != 0) {
        status.error_number = errno;
        (void)write(child_args->status_fd, &status, sizeof(status));
        close(child_args->status_fd);
        return 1;
    }

    if (setgid(0) != 0 || setuid(0) != 0) {
        status.error_number = errno;
        (void)write(child_args->status_fd, &status, sizeof(status));
        close(child_args->status_fd);
        return 1;
    }

    if (child_args->hostname != NULL && child_args->hostname[0] != '\0') {
        if (sethostname(child_args->hostname, strlen(child_args->hostname)) != 0) {
            status.error_number = errno;
            (void)write(child_args->status_fd, &status, sizeof(status));
            close(child_args->status_fd);
            return 1;
        }
    }

    memset(&filesystem_config, 0, sizeof(filesystem_config));
    filesystem_config.rootfs = child_args->rootfs;
    if (filesystem_isolate_rootfs(&filesystem_config) != 0) {
        status.error_number = errno;
        (void)write(child_args->status_fd, &status, sizeof(status));
        close(child_args->status_fd);
        return 1;
    }

    if (mount(NULL, "/proc", NULL, MS_REMOUNT | MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
        if (errno != EINVAL) {
            status.error_number = errno;
            (void)write(child_args->status_fd, &status, sizeof(status));
            close(child_args->status_fd);
            return 1;
        }
    }

    status.ready = 1;
    status.namespace_pid = getpid();
    (void)write(child_args->status_fd, &status, sizeof(status));
    close(child_args->status_fd);

    while (1) {
        pause();
    }

    return 0;
}

static int read_status(int fd, NamespaceStatus *status) {
    unsigned char *buffer = (unsigned char *)status;
    size_t total_read = 0;

    while (total_read < sizeof(*status)) {
        ssize_t bytes_read = read(fd, buffer + total_read, sizeof(*status) - total_read);

        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        total_read += (size_t)bytes_read;
    }

    return (total_read == sizeof(*status)) ? 0 : -1;
}

int namespace_start_container(const NamespaceConfig *config,
                              char *stack,
                              size_t stack_size,
                              NamespaceStartResult *result) {
    NamespaceChildArgs child_args;
    NamespaceStatus status;
    int status_pipe[2];
    int continue_pipe[2];
    int clone_flags = CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD;
    pid_t pid;

    if (config == NULL || stack == NULL || stack_size == 0 || result == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (pipe(status_pipe) != 0) {
        return -1;
    }
    if (pipe(continue_pipe) != 0) {
        int saved_errno = errno;

        close(status_pipe[0]);
        close(status_pipe[1]);
        errno = saved_errno;
        return -1;
    }

    memset(&child_args, 0, sizeof(child_args));
    child_args.hostname = config->hostname;
    child_args.rootfs = config->rootfs;
    child_args.status_fd = status_pipe[1];
    child_args.continue_fd = continue_pipe[0];

    pid = clone(namespace_child, stack + stack_size, clone_flags, &child_args);
    if (pid < 0) {
        int saved_errno = errno;

        close(status_pipe[0]);
        close(status_pipe[1]);
        close(continue_pipe[0]);
        close(continue_pipe[1]);
        errno = saved_errno;
        return -1;
    }

    close(status_pipe[1]);
    close(continue_pipe[0]);
    memset(&status, 0, sizeof(status));
    status.namespace_pid = -1;

    if (configure_user_namespace(pid) != 0) {
        int saved_errno = errno;

        close(continue_pipe[1]);
        close(status_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        errno = saved_errno;
        return -1;
    }

    if (write(continue_pipe[1], "1", 1) != 1) {
        int saved_errno = errno;

        close(continue_pipe[1]);
        close(status_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        errno = saved_errno;
        return -1;
    }
    close(continue_pipe[1]);

    if (read_status(status_pipe[0], &status) != 0 || status.ready == 0) {
        int child_errno = status.error_number;

        close(status_pipe[0]);
        waitpid(pid, NULL, 0);
        errno = (child_errno != 0) ? child_errno : EPROTO;
        return -1;
    }

    close(status_pipe[0]);
    result->host_pid = pid;
    result->namespace_pid = status.namespace_pid;
    return 0;
}

const char *namespace_profile(void) {
    return NAMESPACE_PROFILE;
}

void namespace_format_start_error(int err, char *buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    switch (err) {
        case EPERM:
            snprintf(buffer,
                     buffer_size,
                     "permission denied while creating namespaces; try sudo or enable unprivileged namespaces");
            break;
        case EINVAL:
            snprintf(buffer,
                     buffer_size,
                     "kernel rejected the requested namespace flags on this host");
            break;
        case ENOSPC:
            snprintf(buffer,
                     buffer_size,
                     "namespace limit reached on this host");
            break;
        case EPROTO:
            snprintf(buffer,
                     buffer_size,
                     "namespace child exited before finishing setup");
            break;
        case ENOENT:
        case ENOTDIR:
        case ENAMETOOLONG:
        case EBUSY:
            filesystem_format_error(err, buffer, buffer_size);
            break;
        default:
            snprintf(buffer, buffer_size, "%s", strerror(err));
            break;
    }
}
