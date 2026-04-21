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

/* ── wire protocol on continue_pipe ────────────────────────────────────
 * Parent writes one ContinueToken after user-namespace mapping and veth
 * setup.  Child reads it, then proceeds with sethostname / pivot_root /
 * network setup.
 */
typedef struct {
    char     ok;          /* '1' = proceed, anything else = abort */
    NetConfig net;        /* filled when bridge networking is active */
} ContinueToken;

typedef struct {
    const char    *hostname;
    const char    *rootfs;
    const char    *command_line;
    ResourceConfig resource_limits;
    int            status_fd;       /* child writes NamespaceStatus here    */
    int            continue_fd;     /* child reads  ContinueToken here      */
    int            exec_fd;         /* child writes errno here if execvp fails (CLOEXEC) */
    int            exec_trigger_fd; /* child reads  one byte before execvp  */
    int            log_fd;
} NamespaceChildArgs;

typedef struct {
    int   ready;
    int   error_number;
    pid_t namespace_pid;
} NamespaceStatus;

/* ── helpers ─────────────────────────────────────────────────────────── */

static int build_exec_argv(const char *command_line,
                           char *buffer, size_t buffer_size,
                           char **argv, int max_args) {
    char *token;
    int   argc = 0;

    if (!command_line || !command_line[0] || !buffer || !buffer_size ||
        !argv || max_args < 2) { errno = EINVAL; return -1; }

    if (snprintf(buffer, buffer_size, "%s", command_line) >= (int)buffer_size) {
        errno = ENAMETOOLONG; return -1;
    }
    token = strtok(buffer, " ");
    while (token) {
        if (argc >= max_args - 1) { errno = E2BIG; return -1; }
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    if (argc == 0) { errno = EINVAL; return -1; }
    argv[argc] = NULL;
    return 0;
}

static int write_text_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    if (fputs(content, f) == EOF) { int e = errno; fclose(f); errno = e; return -1; }
    if (fclose(f) != 0) return -1;
    return 0;
}

static int configure_user_namespace(pid_t pid) {
    char path[128], mapping[128];

    snprintf(path, sizeof(path), "/proc/%d/setgroups", (int)pid);
    if (write_text_file(path, "deny\n") != 0 && errno != ENOENT) return -1;

    snprintf(path, sizeof(path), "/proc/%d/uid_map", (int)pid);
    snprintf(mapping, sizeof(mapping), "0 %d 1\n", (int)geteuid());
    if (write_text_file(path, mapping) != 0) return -1;

    snprintf(path, sizeof(path), "/proc/%d/gid_map", (int)pid);
    snprintf(mapping, sizeof(mapping), "0 %d 1\n", (int)getegid());
    return write_text_file(path, mapping);
}

/* Read exactly sizeof(*status) bytes from fd into status. */
static int read_status(int fd, NamespaceStatus *status) {
    unsigned char *buf = (unsigned char *)status;
    size_t total = 0;

    while (total < sizeof(*status)) {
        ssize_t n = read(fd, buf + total, sizeof(*status) - total);
        if (n == 0) break;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        total += (size_t)n;
    }
    return (total == sizeof(*status)) ? 0 : -1;
}

/* Read exec-error from exec_pipe_rd.  EOF (CLOEXEC after execvp) → success. */
static int read_exec_status(int fd, int *child_errno) {
    ssize_t n;
    while (1) {
        n = read(fd, child_errno, sizeof(*child_errno));
        if (n == 0) { close(fd); return 0; }           /* execvp succeeded */
        if (n < 0)  { if (errno == EINTR) continue;
                      close(fd); return -1; }
        if ((size_t)n == sizeof(*child_errno)) {
            close(fd); errno = *child_errno; return -1; /* execvp failed */
        }
        close(fd); errno = EPROTO; return -1;
    }
}

/* ── child entry point ───────────────────────────────────────────────── */

static int namespace_child(void *arg) {
    NamespaceChildArgs *ca = arg;
    FilesystemConfig    fscfg;
    ContinueToken       tok;
    NamespaceStatus     status;
    char                cmd_buf[256];
    char               *argv[32];
    int                 exec_errno = 0;
    unsigned char      *p;
    size_t              total;

    memset(&status, 0, sizeof(status));
    status.namespace_pid = -1;

    /* ── read ContinueToken ── */
    p = (unsigned char *)&tok;
    total = 0;
    while (total < sizeof(tok)) {
        ssize_t n = read(ca->continue_fd, p + total, sizeof(tok) - total);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        total += (size_t)n;
    }
    close(ca->continue_fd);
    if (total < sizeof(tok) || tok.ok != '1') {
        status.error_number = EPROTO;
        (void)write(ca->status_fd, &status, sizeof(status));
        close(ca->status_fd);
        return 1;
    }

    /* ── become uid/gid 0 inside user namespace ── */
    if (setgid(0) != 0 || setuid(0) != 0) {
        status.error_number = errno;
        (void)write(ca->status_fd, &status, sizeof(status));
        close(ca->status_fd); return 1;
    }
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);

    /* ── hostname ── */
    if (ca->hostname && ca->hostname[0]) {
        if (sethostname(ca->hostname, strlen(ca->hostname)) != 0) {
            status.error_number = errno;
            (void)write(ca->status_fd, &status, sizeof(status));
            close(ca->status_fd); return 1;
        }
    }

    /* ── filesystem isolation ── */
    memset(&fscfg, 0, sizeof(fscfg));
    fscfg.rootfs = ca->rootfs;
    if (filesystem_isolate_rootfs(&fscfg) != 0) {
        status.error_number = errno;
        (void)write(ca->status_fd, &status, sizeof(status));
        close(ca->status_fd); return 1;
    }

    if (mount(NULL, "/proc", NULL, MS_REMOUNT | MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) != 0) {
        if (errno != EINVAL) {
            status.error_number = errno;
            (void)write(ca->status_fd, &status, sizeof(status));
            close(ca->status_fd); return 1;
        }
    }

    /* ── loopback ── */
    if (network_setup_loopback() != 0 && errno != EPERM) {
        status.error_number = errno;
        (void)write(ca->status_fd, &status, sizeof(status));
        close(ca->status_fd); return 1;
    }

    /* ── eth0 (bridge networking) ── */
    network_setup_eth0(&tok.net); /* best-effort */

    /* ── build exec argv ── */
    if (build_exec_argv(ca->command_line, cmd_buf, sizeof(cmd_buf),
                        argv, (int)(sizeof(argv)/sizeof(argv[0]))) != 0) {
        status.error_number = errno;
        (void)write(ca->status_fd, &status, sizeof(status));
        close(ca->status_fd); return 1;
    }

    /* ── resource limits ── */
    if (resource_apply_limits(&ca->resource_limits) != 0) {
        status.error_number = RESOURCE_ERROR_BASE + errno;
        (void)write(ca->status_fd, &status, sizeof(status));
        close(ca->status_fd); return 1;
    }

    if (setpgid(0, 0) != 0) {
        status.error_number = errno;
        (void)write(ca->status_fd, &status, sizeof(status));
        close(ca->status_fd); return 1;
    }

    /* ── signal parent: ready ── */
    status.ready         = 1;
    status.namespace_pid = getpid();
    (void)write(ca->status_fd, &status, sizeof(status));
    close(ca->status_fd);

    /* ── wait for parent to print banner, then exec ── */
    {
        char trigger = 0;
        ssize_t n;
        do { n = read(ca->exec_trigger_fd, &trigger, 1); }
        while (n < 0 && errno == EINTR);
        close(ca->exec_trigger_fd);
        if (n != 1 || trigger != '1') return 1;
    }

    /* ── redirect output to log file if set ── */
    if (ca->log_fd >= 0) {
        dup2(ca->log_fd, STDOUT_FILENO);
        dup2(ca->log_fd, STDERR_FILENO);
        close(ca->log_fd);
    }

    execvp(argv[0], argv);
    exec_errno = errno;
    (void)write(ca->exec_fd, &exec_errno, sizeof(exec_errno));
    close(ca->exec_fd);
    errno = exec_errno;
    return 127;
}

/* ── namespace_start_container ────────────────────────────────────────── */

int namespace_start_container(const NamespaceConfig *config,
                              char *stack, size_t stack_size,
                              NamespaceStartResult *result) {
    NamespaceChildArgs ca;
    NamespaceStatus    status;
    int  status_pipe[2]      = {-1,-1};
    int  exec_pipe[2]        = {-1,-1};
    int  continue_pipe[2]    = {-1,-1};
    int  exec_trigger_pipe[2]= {-1,-1};
    int  clone_flags = CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWUTS |
                       CLONE_NEWNS   | CLONE_NEWNET | SIGCHLD;
    pid_t pid;
    ContinueToken tok;

    if (!config || !config->command_line || !config->command_line[0] ||
        !stack || !stack_size || !result) { errno = EINVAL; return -1; }

#define OPEN_PIPE(p) do { if (pipe(p) != 0) goto fail_pipes; } while(0)
    OPEN_PIPE(status_pipe);
    OPEN_PIPE(exec_pipe);
    OPEN_PIPE(continue_pipe);
    OPEN_PIPE(exec_trigger_pipe);
#undef OPEN_PIPE

    if (fcntl(exec_pipe[1], F_SETFD, FD_CLOEXEC) != 0) goto fail_pipes;

    memset(&ca, 0, sizeof(ca));
    ca.hostname      = config->hostname;
    ca.rootfs        = config->rootfs;
    ca.command_line  = config->command_line;
    ca.resource_limits = config->resource_limits;
    ca.status_fd       = status_pipe[1];
    ca.continue_fd     = continue_pipe[0];
    ca.exec_fd         = exec_pipe[1];
    ca.exec_trigger_fd = exec_trigger_pipe[0];
    ca.log_fd          = config->log_fd;

    pid = clone(namespace_child, stack + stack_size, clone_flags, &ca);
    if (pid < 0) goto fail_pipes;

    /* parent no longer needs these ends */
    close(exec_pipe[1]);        exec_pipe[1]        = -1;
    close(status_pipe[1]);      status_pipe[1]       = -1;
    close(continue_pipe[0]);    continue_pipe[0]     = -1;
    close(exec_trigger_pipe[0]);exec_trigger_pipe[0] = -1;
    if (config->log_fd >= 0) close(config->log_fd);

    /* configure user namespace uid/gid mapping */
    if (configure_user_namespace(pid) != 0) goto fail_kill;

    /* optionally set up veth — callback fills NetConfig */
    memset(&tok, 0, sizeof(tok));
    tok.ok = '1';
    if (config->net_setup != NULL) {
        if (config->net_setup(pid, &tok.net, config->net_setup_data) != 0) {
            /* veth setup failed: still start container, just no eth0 */
            memset(&tok.net, 0, sizeof(tok.net));
        }
    }

    /* send ContinueToken — unblocks child */
    {
        const unsigned char *p = (const unsigned char *)&tok;
        size_t total = 0;
        while (total < sizeof(tok)) {
            ssize_t n = write(continue_pipe[1], p + total, sizeof(tok) - total);
            if (n <= 0) { if (n < 0 && errno == EINTR) continue; goto fail_kill; }
            total += (size_t)n;
        }
    }
    close(continue_pipe[1]); continue_pipe[1] = -1;

    /* wait for child to finish setup */
    memset(&status, 0, sizeof(status));
    status.namespace_pid = -1;
    if (read_status(status_pipe[0], &status) != 0 || status.ready == 0) {
        int e = status.error_number;
        close(status_pipe[0]);
        /* close exec_trigger so child sees EOF and exits */
        close(exec_trigger_pipe[1]); exec_trigger_pipe[1] = -1;
        close(exec_pipe[0]);
        waitpid(pid, NULL, 0);
        errno = e ? e : EPROTO;
        return -1;
    }
    close(status_pipe[0]); status_pipe[0] = -1;

    result->host_pid       = pid;
    result->namespace_pid  = status.namespace_pid;
    result->exec_trigger_fd= exec_trigger_pipe[1];
    result->exec_pipe_rd   = exec_pipe[0];
    return 0;

fail_kill:
    close(continue_pipe[1]);
    close(exec_trigger_pipe[1]);
    close(exec_pipe[0]);
    close(status_pipe[0]);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;

fail_pipes:
    {
        int fds[] = { status_pipe[0], status_pipe[1],
                      exec_pipe[0],   exec_pipe[1],
                      continue_pipe[0], continue_pipe[1],
                      exec_trigger_pipe[0], exec_trigger_pipe[1] };
        unsigned i;
        for (i = 0; i < sizeof(fds)/sizeof(fds[0]); i++)
            if (fds[i] >= 0) close(fds[i]);
    }
    if (config->log_fd >= 0) close(config->log_fd);
    return -1;
}

/* ── namespace_trigger_exec ──────────────────────────────────────────── */

int namespace_trigger_exec(NamespaceStartResult *result) {
    char trigger = '1';
    int  child_errno = 0;

    if (write(result->exec_trigger_fd, &trigger, 1) != 1) {
        close(result->exec_trigger_fd);
        close(result->exec_pipe_rd);
        result->exec_trigger_fd = -1;
        result->exec_pipe_rd    = -1;
        return -1;
    }
    close(result->exec_trigger_fd);
    result->exec_trigger_fd = -1;

    return read_exec_status(result->exec_pipe_rd, &child_errno);
}

/* ── profile / error formatting ──────────────────────────────────────── */

const char *namespace_profile(void) { return NAMESPACE_PROFILE; }

void namespace_format_start_error(int err, char *buffer, size_t buffer_size) {
    if (!buffer || !buffer_size) return;

    switch (err) {
    case RESOURCE_ERROR_BASE + EPERM:
        resource_format_error(EPERM, buffer, buffer_size); break;
    case RESOURCE_ERROR_BASE + EINVAL:
        resource_format_error(EINVAL, buffer, buffer_size); break;
    case EPERM:
        snprintf(buffer, buffer_size,
                 "permission denied; try sudo or enable unprivileged namespaces"); break;
    case EINVAL:
        snprintf(buffer, buffer_size,
                 "kernel rejected the requested namespace flags"); break;
    case ENOSPC:
        snprintf(buffer, buffer_size, "namespace limit reached"); break;
    case EPROTO:
        snprintf(buffer, buffer_size, "namespace child exited before setup"); break;
    case E2BIG:
        snprintf(buffer, buffer_size, "too many arguments"); break;
    case ENOENT:
        snprintf(buffer, buffer_size, "command not found inside rootfs"); break;
    case ENOTDIR: case ENAMETOOLONG: case EBUSY:
        filesystem_format_error(err, buffer, buffer_size); break;
    default:
        snprintf(buffer, buffer_size, "%s", strerror(err)); break;
    }
}
