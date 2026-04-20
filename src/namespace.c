#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "filesystem.h"
#include "namespace.h"
#include "network.h"
#include "resource.h"

typedef struct {
    int   ready;
    int   error_number;
    pid_t namespace_pid;
} NamespaceStatus;

typedef struct {
    const char *hostname;
    const char *rootfs;
    const char *command_line;
    ResourceConfig resource_limits;
    int         status_fd;
    int         continue_fd;
    int         exec_fd;
    int         log_fd;
} NamespaceChildArgs;

static int build_exec_argv(const char *command_line, char *buffer, size_t buffer_size, char **argv, int max_args) {
    char *token = NULL;
    int argc = 0;

    if (command_line == NULL || command_line[0] == '\0' || buffer == NULL || buffer_size == 0 ||
        argv == NULL || max_args < 2) {
        errno = EINVAL;
        return -1;
    }

    if (snprintf(buffer, buffer_size, "%s", command_line) >= (int)buffer_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    token = strtok(buffer, " ");
    while (token != NULL) {
        if (argc >= max_args - 1) {
            errno = E2BIG;
            return -1;
        }

        argv[argc++] = token;
        token = strtok(NULL, " ");
    }

    if (argc == 0) {
        errno = EINVAL;
        return -1;
    }

    argv[argc] = NULL;
    return 0;
}

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
    char command_buffer[256];
    char *argv[32];
    int exec_errno = 0;
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

    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

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

    if (network_setup_loopback() != 0) {
        /* Best-effort for WSL/host restrictions. */
        if (errno != EPERM) {
            status.error_number = errno;
            (void)write(child_args->status_fd, &status, sizeof(status));
            close(child_args->status_fd);
            return 1;
        }
    }

    if (build_exec_argv(child_args->command_line,
                        command_buffer,
                        sizeof(command_buffer),
                        argv,
                        (int)(sizeof(argv) / sizeof(argv[0]))) != 0) {
        status.error_number = errno;
        (void)write(child_args->status_fd, &status, sizeof(status));
        close(child_args->status_fd);
        return 1;
    }

    if (resource_apply_limits(&child_args->resource_limits) != 0) {
        status.error_number = RESOURCE_ERROR_BASE + errno;
        (void)write(child_args->status_fd, &status, sizeof(status));
        close(child_args->status_fd);
        return 1;
    }

    if (setpgid(0, 0) != 0) {
        status.error_number = errno;
        (void)write(child_args->status_fd, &status, sizeof(status));
        close(child_args->status_fd);
        return 1;
    }

    status.ready = 1;
    status.namespace_pid = getpid();
    (void)write(child_args->status_fd, &status, sizeof(status));
    close(child_args->status_fd);

    if (child_args->log_fd >= 0) {
        dup2(child_args->log_fd, STDOUT_FILENO);
        dup2(child_args->log_fd, STDERR_FILENO);
        close(child_args->log_fd);
    }

    execvp(argv[0], argv);
    exec_errno = errno;
    (void)write(child_args->exec_fd, &exec_errno, sizeof(exec_errno));
    close(child_args->exec_fd);
    errno = exec_errno;
    return 127;
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

static int read_exec_status(int fd, int *child_errno) {
    ssize_t bytes_read = 0;

    if (child_errno == NULL) {
        errno = EINVAL;
        return -1;
    }

    while (1) {
        bytes_read = read(fd, child_errno, sizeof(*child_errno));
        if (bytes_read == 0) {
            close(fd);
            return 0;
        }
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if ((size_t)bytes_read == sizeof(*child_errno)) {
            close(fd);
            errno = *child_errno;
            return -1;
        }

        close(fd);
        errno = EPROTO;
        return -1;
    }
}

int namespace_start_container(const NamespaceConfig *config,
                              char *stack,
                              size_t stack_size,
                              NamespaceStartResult *result) {
    NamespaceChildArgs child_args;
    NamespaceStatus status;
    int child_errno = 0;
    int exec_pipe[2];
    int status_pipe[2];
    int continue_pipe[2];
    int clone_flags = CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD;
    pid_t pid;

    if (config == NULL || config->command_line == NULL || config->command_line[0] == '\0' ||
        stack == NULL || stack_size == 0 || result == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (pipe(status_pipe) != 0) {
        return -1;
    }
    if (pipe(exec_pipe) != 0) {
        int saved_errno = errno;

        close(status_pipe[0]);
        close(status_pipe[1]);
        errno = saved_errno;
        return -1;
    }
    if (fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
        int saved_errno = errno;

        close(status_pipe[0]);
        close(status_pipe[1]);
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        errno = saved_errno;
        return -1;
    }
    if (pipe(continue_pipe) != 0) {
        int saved_errno = errno;

        close(status_pipe[0]);
        close(status_pipe[1]);
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        errno = saved_errno;
        return -1;
    }

    memset(&child_args, 0, sizeof(child_args));
    child_args.hostname = config->hostname;
    child_args.rootfs = config->rootfs;
    child_args.command_line = config->command_line;
    child_args.resource_limits = config->resource_limits;
    child_args.status_fd = status_pipe[1];
    child_args.continue_fd = continue_pipe[0];
    child_args.exec_fd = exec_pipe[1];
    child_args.log_fd = config->log_fd;

    pid = clone(namespace_child, stack + stack_size, clone_flags, &child_args);
    if (pid < 0) {
        int saved_errno = errno;

        close(status_pipe[0]);
        close(status_pipe[1]);
        close(exec_pipe[0]);
        close(exec_pipe[1]);
        close(continue_pipe[0]);
        close(continue_pipe[1]);
        if (config->log_fd >= 0) close(config->log_fd);
        errno = saved_errno;
        return -1;
    }

    close(exec_pipe[1]);
    close(status_pipe[1]);
    close(continue_pipe[0]);
    if (config->log_fd >= 0) {
        close(config->log_fd);
    }
    memset(&status, 0, sizeof(status));
    status.namespace_pid = -1;

    if (configure_user_namespace(pid) != 0) {
        int saved_errno = errno;

        close(continue_pipe[1]);
        close(status_pipe[0]);
        close(exec_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        errno = saved_errno;
        return -1;
    }

    if (write(continue_pipe[1], "1", 1) != 1) {
        int saved_errno = errno;

        close(continue_pipe[1]);
        close(status_pipe[0]);
        close(exec_pipe[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        errno = saved_errno;
        return -1;
    }
    close(continue_pipe[1]);

    if (read_status(status_pipe[0], &status) != 0 || status.ready == 0) {
        int setup_errno = status.error_number;

        close(status_pipe[0]);
        close(exec_pipe[0]);
        waitpid(pid, NULL, 0);
        errno = (setup_errno != 0) ? setup_errno : EPROTO;
        return -1;
    }

    close(status_pipe[0]);
    if (read_exec_status(exec_pipe[0], &child_errno) != 0) {
        int saved_errno = errno;

        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        errno = saved_errno;
        return -1;
    }
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
        case RESOURCE_ERROR_BASE + EPERM:
            resource_format_error(EPERM, buffer, buffer_size);
            break;
        case RESOURCE_ERROR_BASE + EINVAL:
            resource_format_error(EINVAL, buffer, buffer_size);
            break;
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
        case E2BIG:
            snprintf(buffer, buffer_size, "container command has too many arguments for the current runtime");
            break;
        case ENOENT:
            snprintf(buffer, buffer_size, "container command was not found inside the isolated rootfs");
            break;
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
