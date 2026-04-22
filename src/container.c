#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "bridge.h"
#include "container.h"
#include "eventbus.h"
#include "filesystem.h"
#include "image.h"
#include "logger.h"
#include "metrics.h"
#include "monitor.h"
#include "network.h"
#include "namespace.h"
#include "resource.h"
#include "scheduler.h"

#define STACK_SIZE (1024 * 1024)
#define METADATA_FILE "containers.meta"
#define METADATA_FILE_TMP "containers.meta.tmp"
#define DEFAULT_CONTAINER_COMMAND "/bin/sh"

static Container *head = NULL;
static Container *tail = NULL;
static int next_sequence = 1;
static volatile sig_atomic_t g_interrupt_requested = 0;

void container_request_interrupt(void) {
    g_interrupt_requested = 1;
}

int container_consume_interrupt(void) {
    if (g_interrupt_requested) {
        g_interrupt_requested = 0;
        return 1;
    }
    return 0;
}

static const char *state_to_string(ContainerState state) {
    switch (state) {
        case STATE_CREATED:
            return "CREATED";
        case STATE_RUNNING:
            return "RUNNING";
        case STATE_PAUSED:
            return "PAUSED";
        case STATE_STOPPED:
            return "STOPPED";
        default:
            return "UNKNOWN";
    }
}

static ContainerState string_to_state(const char *value) {
    if (strcmp(value, "RUNNING") == 0) {
        return STATE_RUNNING;
    }
    if (strcmp(value, "PAUSED") == 0) {
        return STATE_PAUSED;
    }
    if (strcmp(value, "STOPPED") == 0) {
        return STATE_STOPPED;
    }
    return STATE_CREATED;
}

static void copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, dst_size, "%s", src);
}

static void trim_newline(char *line) {
    if (line == NULL) {
        return;
    }
    line[strcspn(line, "\r\n")] = '\0';
}

static void append_container(Container *container) {
    container->next = NULL;
    if (tail == NULL) {
        head = tail = container;
        return;
    }
    tail->next = container;
    tail = container;
}

static void remove_container(Container *container) {
    Container *prev = NULL;

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        if (cursor != container) {
            prev = cursor;
            continue;
        }

        if (prev == NULL) {
            head = cursor->next;
        } else {
            prev->next = cursor->next;
        }

        if (tail == cursor) {
            tail = prev;
        }
        cursor->next = NULL;
        return;
    }
}

static Container *find_container(const char *id) {
    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        if (strcmp(cursor->id, id) == 0) {
            return cursor;
        }
    }
    return NULL;
}

static void free_container_stack(Container *container) {
    if (container->stack != NULL) {
        free(container->stack);
        container->stack = NULL;
    }
}

static int save_metadata(void) {
    FILE *file = fopen(METADATA_FILE_TMP, "w");
    if (file == NULL) {
        printf("[error] failed to write container metadata\n\n");
        log_event("metadata save failed: could not open temp file");
        return -1;
    }

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        char port_maps_buf[128];
        bridge_serialize_port_maps(cursor->port_maps, cursor->port_map_count,
                                   port_maps_buf, sizeof(port_maps_buf));
        fprintf(file,
                "%s\t%s\t%d\t%s\t%s\t%s\t%s\t%u\t%u\t%u\t%ld\t%ld\t%d\t%s\t%s\t%s\t%s\t%s"
                "\t%d\t%d\t%d\t%" PRIx64 "\n",
                cursor->id,
                cursor->name,
                (int)cursor->pid,
                cursor->hostname,
                cursor->rootfs,
                state_to_string(cursor->state),
                cursor->command_line,
                cursor->resource_limits.cpu_seconds,
                cursor->resource_limits.memory_mb,
                cursor->resource_limits.max_processes,
                (long)cursor->started_at,
                (long)cursor->stopped_at,
                cursor->exit_code,
                cursor->log_path,
                cursor->image_ref,
                cursor->ip_address,
                cursor->veth_host,
                port_maps_buf,
                /* fields 18-21: security */
                cursor->security.privileged,
                cursor->security.seccomp_enabled,
                cursor->security.readonly_rootfs,
                cursor->security.cap_keep);
    }

    if (fclose(file) != 0) {
        printf("[error] failed to flush container metadata\n\n");
        log_event("metadata save failed: fclose");
        return -1;
    }

    if (rename(METADATA_FILE_TMP, METADATA_FILE) != 0) {
        /* On Windows/DrvFs, rename over an open file fails with EACCES.
           Unlink the stale target and retry once. */
        unlink(METADATA_FILE);
        if (rename(METADATA_FILE_TMP, METADATA_FILE) != 0) {
            printf("[error] failed to finalize container metadata\n\n");
            log_event("metadata save failed: rename");
            return -1;
        }
    }

    return 0;
}

static void update_next_sequence(const char *id) {
    int value = 0;

    if (sscanf(id, "container-%d", &value) == 1 && value >= next_sequence) {
        next_sequence = value + 1;
    }
}

static int is_pid_alive(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }

    if (kill(pid, 0) == 0) {
        return 1;
    }

    return errno == EPERM;
}

static void format_wait_status(int status, char *buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    if (WIFEXITED(status)) {
        snprintf(buffer, buffer_size, "exited code=%d", WEXITSTATUS(status));
        return;
    }
    if (WIFSIGNALED(status)) {
        snprintf(buffer, buffer_size, "killed signal=%d", WTERMSIG(status));
        return;
    }

    snprintf(buffer, buffer_size, "status=%d", status);
}

/* Tear down veth and iptables rules for a container that is exiting or being stopped.
 * Safe to call even when the bridge is not active or fields are empty. */
static void cleanup_container_network(Container *container) {
    if (container->ip_address[0] != '\0') {
        int i;
        for (i = 0; i < container->port_map_count; i++) {
            bridge_del_port_forward(&container->port_maps[i], container->ip_address);
        }
    }
    if (container->veth_host[0] != '\0') {
        bridge_teardown_veth(container->veth_host);
        container->veth_host[0]  = '\0';
        container->ip_address[0] = '\0';
    }
}

static int sync_container_state(Container *container) {
    int state_changed = 0;
    int status = 0;
    pid_t result = 0;

    if ((container->state != STATE_RUNNING && container->state != STATE_PAUSED) ||
        container->pid <= 0) {
        return 0;
    }

    /* Don't try to reap a paused process — it isn't done yet. */
    if (container->state == STATE_PAUSED) {
        return 0;
    }

    result = waitpid(container->pid, &status, WNOHANG);
    if (result > 0) {
        char reason[64];
        pid_t old_pid = container->pid;

        container->stopped_at = time(NULL);
        container->exit_code  = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
        container->state = STATE_STOPPED;
        container->pid   = -1;
        free_container_stack(container);
        container_scheduler_on_stopped(old_pid);
        resource_cleanup_cgroup(container->id);
        cleanup_container_network(container);
        format_wait_status(status, reason, sizeof(reason));
        log_event("%s exited and was reaped by manager (%s)", container->id, reason);
        eventbus_emit(EVENT_CONTAINER_STOPPED, container->id, reason,
                      (long)container->exit_code);
        return 1;
    }

    if (result == 0) {
        return 0;
    }

    if (errno == ECHILD && !is_pid_alive(container->pid)) {
        container->state = STATE_STOPPED;
        container->pid = -1;
        free_container_stack(container);
        resource_cleanup_cgroup(container->id);
        cleanup_container_network(container);
        log_event("%s marked stopped after recovery check", container->id);
        eventbus_emit(EVENT_CONTAINER_STOPPED, container->id, "recovery-check", -1);
        state_changed = 1;
    }

    return state_changed;
}

static long read_oom_kill_count(const char *container_id); /* forward decl */

static void poll_states(void) {
    int changed = 0;

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        /* Detect new OOM kills before reaping — container may still be alive. */
        if (cursor->state == STATE_RUNNING && cursor->pid > 0) {
            long oom = read_oom_kill_count(cursor->id);
            if (oom > 0 && oom > cursor->oom_kill_count) {
                MonitorStats mst = {0};
                long rss_mb = 0;
                if (monitor_read(cursor->pid, &mst) == 0)
                    rss_mb = (long)(mst.rss_bytes / (1024 * 1024));
                eventbus_emit(EVENT_OOM_KILL, cursor->id, NULL, rss_mb);
                cursor->oom_kill_count = oom;
            }
        }
        changed |= sync_container_state(cursor);
    }

    if (changed) {
        save_metadata();
    }
}

static int normalize_resource_limits(ResourceConfig *limits) {
    if (limits == NULL) {
        return -1;
    }

    return 0;
}

static int move_terminal_foreground(pid_t pgrp) {
    void (*previous_sigttou)(int) = SIG_ERR;
    void (*previous_sigttin)(int) = SIG_ERR;
    void (*previous_sigtstp)(int) = SIG_ERR;

    if (!isatty(STDIN_FILENO)) {
        return 0;
    }

    previous_sigttou = signal(SIGTTOU, SIG_IGN);
    previous_sigttin = signal(SIGTTIN, SIG_IGN);
    previous_sigtstp = signal(SIGTSTP, SIG_IGN);

    if (tcsetpgrp(STDIN_FILENO, pgrp) != 0) {
        int saved_errno = errno;

        if (previous_sigttou != SIG_ERR) {
            signal(SIGTTOU, previous_sigttou);
        }
        if (previous_sigttin != SIG_ERR) {
            signal(SIGTTIN, previous_sigttin);
        }
        if (previous_sigtstp != SIG_ERR) {
            signal(SIGTSTP, previous_sigtstp);
        }
        errno = saved_errno;
        return -1;
    }

    if (previous_sigttou != SIG_ERR) {
        signal(SIGTTOU, previous_sigttou);
    }
    if (previous_sigttin != SIG_ERR) {
        signal(SIGTTIN, previous_sigttin);
    }
    if (previous_sigtstp != SIG_ERR) {
        signal(SIGTSTP, previous_sigtstp);
    }

    return 0;
}

static int wait_for_container_process(Container *container, int quiet) {
    int status = 0;
    char reason[64];
    pid_t shell_pgrp = getpgrp();
    int terminal_moved = 0;

    if (container == NULL || container->state != STATE_RUNNING || container->pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    if (move_terminal_foreground(container->pid) == 0) {
        terminal_moved = 1;
    }

    if (waitpid(container->pid, &status, 0) < 0) {
        if (terminal_moved) {
            (void)move_terminal_foreground(shell_pgrp);
        }
        if (errno == ECHILD && !is_pid_alive(container->pid)) {
            status = 0;
        } else {
            return -1;
        }
    }

    if (terminal_moved) {
        (void)move_terminal_foreground(shell_pgrp);
    }

    format_wait_status(status, reason, sizeof(reason));
    log_event("%s finished (%s)", container->id, reason);
    container->stopped_at = time(NULL);
    container->exit_code  = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    container->pid   = -1;
    container->state = STATE_STOPPED;
    eventbus_emit(EVENT_CONTAINER_STOPPED, container->id, reason,
                  (long)container->exit_code);
    free_container_stack(container);
    resource_cleanup_cgroup(container->id);
    cleanup_container_network(container);

    if (save_metadata() != 0) {
        return -1;
    }

    if (!quiet) {
        printf("[manager] %s finished\n\n", container->id);
    }

    return 0;
}

#define STOP_TIMEOUT_S 10   /* default SIGTERM grace period — mirrors docker stop */

static int stop_container_process(Container *container, int quiet, int timeout_s) {
    int reaped = 0;
    int wstatus = 0;
    int effective_timeout = (timeout_s > 0) ? timeout_s : STOP_TIMEOUT_S;

    if ((container->state != STATE_RUNNING && container->state != STATE_PAUSED) ||
        container->pid <= 0) {
        if (!quiet) {
            printf("[manager] %s is not running\n\n", container->id);
        }
        return -1;
    }

    /* Resume a paused container so signals reach it. */
    if (container->state == STATE_PAUSED) {
        kill(container->pid, SIGCONT);
    }

    /* Phase 1: SIGTERM + grace period (mirrors `docker stop`). */
    if (!quiet) {
        printf("[manager] stopping %s (SIGTERM, timeout %ds)...\n",
               container->id, effective_timeout);
    }
    if (kill(container->pid, SIGTERM) == 0) {
        struct timespec ts = {0, 100000000}; /* 100 ms */
        int ticks = 0;
        while (ticks < effective_timeout * 10) {
            pid_t r = waitpid(container->pid, &wstatus, WNOHANG);
            if (r > 0) { reaped = 1; break; }
            if (r < 0 && errno != EINTR) break;
            nanosleep(&ts, NULL);
            ticks++;
        }
    }

    /* Phase 2: SIGKILL if container is still alive. */
    if (!reaped) {
        if (!quiet) {
            printf("[manager] %s did not exit gracefully — sending SIGKILL\n",
                   container->id);
        }
        if (kill(container->pid, SIGKILL) != 0 && errno != ESRCH) {
            if (!quiet) {
                printf("[error] failed to stop %s\n\n", container->id);
            }
            log_event("failed to stop %s (pid %d)", container->id, container->pid);
            return -1;
        }
        if (waitpid(container->pid, &wstatus, 0) < 0 && errno != ECHILD) {
            if (!quiet) {
                printf("[error] failed to reap %s\n\n", container->id);
            }
            log_event("failed to reap %s (pid %d)", container->id, container->pid);
            return -1;
        }
    }

    log_event("%s STOPPED (pid %d)", container->id, container->pid);
    container_scheduler_on_stopped(container->pid);
    resource_cleanup_cgroup(container->id);
    cleanup_container_network(container);
    container->stopped_at = time(NULL);
    container->exit_code  = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus)
                          : 128 + WTERMSIG(wstatus);
    container->pid   = -1;
    container->state = STATE_STOPPED;
    eventbus_emit(EVENT_CONTAINER_STOPPED, container->id, "killed",
                  (long)container->exit_code);
    free_container_stack(container);
    save_metadata();

    if (!quiet) {
        printf("[manager] %s stopped\n\n", container->id);
    }

    return 0;
}

/* ── Unicode box constants (total line width = 62 chars + newline) ── */
#define BOX_DASHES \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80" \
    "\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80"

/* row: │  <label:10>  <value:46>│  (2+10+2+46+1 = 61 inner + 1 border = 62) */
static void box_row(const char *label, const char *value) {
    printf("\xe2\x94\x82  %-10s  %-46.46s\xe2\x94\x82\n", label, value);
}

static void print_container_banner(const Container *container, pid_t namespace_pid) {
    char pid_text[24];
    char nspid_text[24];
    char limits_text[128];

    if (container->pid > 0)
        snprintf(pid_text, sizeof(pid_text), "%d", (int)container->pid);
    else
        copy_string(pid_text, sizeof(pid_text), "-");

    if (namespace_pid > 0)
        snprintf(nspid_text, sizeof(nspid_text), "%d", (int)namespace_pid);
    else
        nspid_text[0] = '\0';

    resource_format_limits(&container->resource_limits, limits_text, sizeof(limits_text));

    const char *state = state_to_string(container->state);

    /* ── top border ── */
    printf("\n\xe2\x95\xad" BOX_DASHES "\xe2\x95\xae\n");

    /* ── id + state header ── */
    /* inner = 60 chars: "  <id:36>  <state:18>  " but we do: 2+36+gap+state+pad */
    printf("\xe2\x94\x82  %-36.36s%20.20s  \xe2\x94\x82\n", container->id, state);

    /* ── separator ── */
    printf("\xe2\x94\x9c" BOX_DASHES "\xe2\x94\xa4\n");

    /* ── info rows ── */
    box_row("name",    container->name);
    box_row("hostname", container->hostname);

    /* pid + ns-pid on one row */
    if (nspid_text[0] != '\0') {
        char pid_combo[64];
        snprintf(pid_combo, sizeof(pid_combo), "%-18s  ns pid  %s", pid_text, nspid_text);
        box_row("pid", pid_combo);
    } else {
        box_row("pid", pid_text);
    }

    box_row("command",  container->command_line);
    box_row("rootfs",   container->rootfs);
    box_row("image",    container->image_ref[0] ? container->image_ref : "-");
    box_row("limits",   limits_text);
    box_row("isolate",  namespace_profile());
    box_row("fs",       filesystem_profile());
    box_row("net",      network_profile());
    box_row("res",      resource_profile());

    if (container->ip_address[0] != '\0') {
        char net_info[64];
        snprintf(net_info, sizeof(net_info), "%s  veth %s",
                 container->ip_address, container->veth_host);
        box_row("ip / veth", net_info);
    }

    if (container->port_map_count > 0) {
        char ports_buf[128];
        bridge_serialize_port_maps(container->port_maps, container->port_map_count,
                                   ports_buf, sizeof(ports_buf));
        box_row("ports", ports_buf);
    }

    /* ── bottom border ── */
    printf("\xe2\x95\xb0" BOX_DASHES "\xe2\x95\xaf\n\n");
}

static void print_start_error(const Container *container, int error_number) {
    char message[256];

    namespace_format_start_error(error_number, message, sizeof(message));
    printf("[error] failed to start %s with isolation setup\n", container->id);
    printf("[hint] rootfs: %s\n", container->rootfs);
    printf("[hint] %s\n\n", message);
}

static void format_start_error_message(int error_number, char *buffer, size_t buffer_size) {
    namespace_format_start_error(error_number, buffer, buffer_size);
}

int container_manager_init(void) {
    FILE *file = fopen(METADATA_FILE, "r");
    char line[1024];
    int restored = 0;

    if (file == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *fields[22];
        Container *container = NULL;
        int index = 0;

        trim_newline(line);
        /* Use a tab-aware splitter instead of strtok so empty fields
           (e.g. empty log_path or image_ref) are not silently skipped. */
        {
            char *p = line;
            while (index < 22) {
                fields[index++] = p;
                char *tab = strchr(p, '\t');
                if (tab == NULL) break;
                *tab = '\0';
                p = tab + 1;
            }
        }

        if (index < 6) {
            continue;
        }

        container = calloc(1, sizeof(*container));
        if (container == NULL) {
            fclose(file);
            printf("[error] out of memory while restoring containers\n\n");
            return -1;
        }

        copy_string(container->id, sizeof(container->id), fields[0]);
        copy_string(container->name, sizeof(container->name), fields[1]);
        container->pid = (pid_t)atoi(fields[2]);
        copy_string(container->hostname, sizeof(container->hostname), fields[3]);
        copy_string(container->rootfs, sizeof(container->rootfs), fields[4]);
        container->state = string_to_state(fields[5]);

        copy_string(container->command_line, sizeof(container->command_line),
                    (index >= 7) ? fields[6] : DEFAULT_CONTAINER_COMMAND);

        if (index >= 10) {
            container->resource_limits.cpu_seconds  = (unsigned int)strtoul(fields[7], NULL, 10);
            container->resource_limits.memory_mb    = (unsigned int)strtoul(fields[8], NULL, 10);
            container->resource_limits.max_processes = (unsigned int)strtoul(fields[9], NULL, 10);
        } else {
            memset(&container->resource_limits, 0, sizeof(container->resource_limits));
        }

        if (index >= 14) {
            container->started_at  = (time_t)atol(fields[10]);
            container->stopped_at  = (time_t)atol(fields[11]);
            container->exit_code   = atoi(fields[12]);
            copy_string(container->log_path, sizeof(container->log_path), fields[13]);
        } else {
            container->started_at = 0;
            container->stopped_at = 0;
            container->exit_code  = -1;
            container->log_path[0] = '\0';
        }

        copy_string(container->image_ref, sizeof(container->image_ref),
                    (index >= 15) ? fields[14] : "");

        copy_string(container->ip_address, sizeof(container->ip_address),
                    (index >= 16) ? fields[15] : "");
        copy_string(container->veth_host, sizeof(container->veth_host),
                    (index >= 17) ? fields[16] : "");
        if (index >= 18 && fields[17][0] != '\0') {
            container->port_map_count =
                bridge_parse_port_maps(fields[17], container->port_maps, MAX_PORT_MAPS);
        }

        /* Security fields (added in module 10; absent in older metadata → defaults). */
        container->security = security_config_default();
        if (index >= 22) {
            container->security.privileged      = atoi(fields[18]);
            container->security.seccomp_enabled  = atoi(fields[19]);
            container->security.readonly_rootfs  = atoi(fields[20]);
            container->security.cap_keep         = (uint64_t)strtoull(fields[21], NULL, 16);
        }

        bridge_mark_ip_used(container->ip_address);

        (void)normalize_resource_limits(&container->resource_limits);
        container->stack = NULL;

        if (container->state == STATE_RUNNING && !is_pid_alive(container->pid)) {
            container->state = STATE_STOPPED;
            container->pid = -1;
        }

        append_container(container);
        update_next_sequence(container->id);
        restored++;
    }

    fclose(file);
    poll_states();

    if (restored > 0) {
        printf("[manager] restored %d container record(s) from %s\n\n",
               restored,
               METADATA_FILE);
        log_event("restored %d container record(s)", restored);
    }

    return 0;
}

int container_create(const ContainerSpec *spec, char *out_id, size_t out_id_size) {
    Container *container = calloc(1, sizeof(*container));
    char requested_rootfs[CONTAINER_ROOTFS_LEN];
    int saved_errno = 0;

    if (container == NULL) {
        printf("[error] out of memory\n\n");
        return -1;
    }

    snprintf(container->id, sizeof(container->id), "container-%04d", next_sequence++);

    if (spec != NULL && spec->name != NULL && spec->name[0] != '\0') {
        copy_string(container->name, sizeof(container->name), spec->name);
    } else {
        copy_string(container->name, sizeof(container->name), container->id);
    }

    if (spec != NULL && spec->hostname != NULL && spec->hostname[0] != '\0') {
        copy_string(container->hostname, sizeof(container->hostname), spec->hostname);
    } else {
        copy_string(container->hostname, sizeof(container->hostname), container->name);
    }

    container->image_ref[0] = '\0';
    if (spec != NULL && spec->rootfs != NULL && spec->rootfs[0] != '\0') {
        /* Try to resolve as an image name first; fall back to treating as a path. */
        char resolved_image[IMAGE_PATH_LEN];
        if (image_resolve(spec->rootfs, resolved_image, sizeof(resolved_image)) == 0) {
            copy_string(requested_rootfs, sizeof(requested_rootfs), resolved_image);
            /* Store the image ref ("name:tag") for later display in inspect. */
            snprintf(container->image_ref, sizeof(container->image_ref), "%s", spec->rootfs);
        } else {
            copy_string(requested_rootfs, sizeof(requested_rootfs), spec->rootfs);
        }
    } else {
        snprintf(requested_rootfs, sizeof(requested_rootfs), "./rootfs/%s", container->id);
    }

    if (spec != NULL && spec->command_line != NULL && spec->command_line[0] != '\0') {
        copy_string(container->command_line, sizeof(container->command_line), spec->command_line);
    } else {
        copy_string(container->command_line, sizeof(container->command_line), DEFAULT_CONTAINER_COMMAND);
    }

    memset(&container->resource_limits, 0, sizeof(container->resource_limits));
    container->ip_address[0] = '\0';
    container->veth_host[0]  = '\0';
    container->port_map_count = 0;
    container->security = security_config_default();
    if (spec != NULL) {
        container->resource_limits = spec->resource_limits;
        (void)normalize_resource_limits(&container->resource_limits);
        if (spec->port_map_count > 0) {
            int n = spec->port_map_count < MAX_PORT_MAPS ? spec->port_map_count : MAX_PORT_MAPS;
            memcpy(container->port_maps, spec->port_maps, (size_t)n * sizeof(PortMapping));
            container->port_map_count = n;
        }
        container->security = spec->security;
    }

    if (filesystem_prepare_rootfs(requested_rootfs, container->rootfs, sizeof(container->rootfs)) != 0) {
        saved_errno = errno;
        printf("[error] failed to prepare rootfs for %s\n", container->id);
        printf("[hint] %s\n\n", strerror(saved_errno));
        free(container);
        return -1;
    }

    container->pid        = -1;
    container->started_at = 0;
    container->stopped_at = 0;
    container->exit_code  = -1;
    container->log_path[0] = '\0';
    container->state = STATE_CREATED;
    append_container(container);

    if (save_metadata() != 0) {
        remove_container(container);
        free(container);
        return -1;
    }

    if (out_id != NULL && out_id_size > 0) {
        copy_string(out_id, out_id_size, container->id);
    }

    printf("[manager] created %s\n", container->id);
    print_container_banner(container, -1);
    log_event("%s CREATED (name=%s hostname=%s rootfs=%s command=%s cpu=%us mem=%uMB nproc=%u)",
              container->id,
              container->name,
              container->hostname,
              container->rootfs,
              container->command_line,
              container->resource_limits.cpu_seconds,
              container->resource_limits.memory_mb,
              container->resource_limits.max_processes);
    eventbus_emit(EVENT_CONTAINER_CREATED, container->id, container->rootfs, 0);
    return 0;
}

void container_scheduler_on_started(pid_t pid) {
    (void)scheduler_add_target(pid);
}

void container_scheduler_on_stopped(pid_t pid) {
    (void)scheduler_remove_target(pid);
}

void container_scheduler_refresh_targets(void) {
    pid_t first = -1;
    int enabled = scheduler_is_enabled();

    scheduler_clear_targets();
    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        if (cursor->state == STATE_RUNNING && cursor->pid > 0) {
            if (first < 0) {
                first = cursor->pid;
            }
            (void)scheduler_add_target(cursor->pid);
        }
    }

    if (enabled && first > 0) {
        /* Start RR from a stable point: stop others and resume the first. */
        for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
            if (cursor->state == STATE_RUNNING && cursor->pid > 0 && cursor->pid != first) {
                (void)kill(cursor->pid, SIGSTOP);
            }
        }
        (void)kill(first, SIGCONT);
    } else if (!enabled) {
        /* Make sure no container is left paused when scheduling is off. */
        for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
            if (cursor->state == STATE_RUNNING && cursor->pid > 0) {
                (void)kill(cursor->pid, SIGCONT);
            }
        }
    }
}

/* Called by namespace_start_container between clone() and writing ContinueToken.
 * Sets up veth pair if the bridge is up, fills cfg for the child. */
static int container_net_setup(pid_t container_pid, NetConfig *cfg, void *userdata) {
    Container *c = (Container *)userdata;
    char host_veth[16], peer_veth[16];
    char ip[16];

    if (!bridge_is_up()) return 0;

    if (bridge_alloc_ip(ip, sizeof(ip)) != 0) {
        log_event("%s bridge IP allocation failed", c->id);
        return 0; /* non-fatal: start without bridge */
    }

    if (bridge_setup_veth(c->id, container_pid, ip,
                          host_veth, sizeof(host_veth),
                          peer_veth, sizeof(peer_veth)) != 0) {
        log_event("%s veth setup failed", c->id);
        return 0; /* non-fatal */
    }

    snprintf(cfg->peer_veth, sizeof(cfg->peer_veth), "%s", peer_veth);
    snprintf(cfg->ip,        sizeof(cfg->ip),        "%s", ip);
    snprintf(cfg->gateway,   sizeof(cfg->gateway),   "%s", BRIDGE_IP);

    snprintf(c->ip_address, sizeof(c->ip_address), "%s", ip);
    snprintf(c->veth_host,  sizeof(c->veth_host),  "%s", host_veth);
    return 0;
}

static int start_container_by_id(const char *id, int schedule_target) {
    Container *container = NULL;
    NamespaceConfig namespace_config;
    NamespaceStartResult start_result;
    int saved_errno = 0;
    struct timespec t_start = {0}, t_end = {0};

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: start <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->state == STATE_RUNNING) {
        printf("[manager] %s is already running\n\n", container->id);
        return -1;
    }

    container->stack = malloc(STACK_SIZE);
    if (container->stack == NULL) {
        printf("[error] out of memory\n\n");
        return -1;
    }

    memset(&namespace_config, 0, sizeof(namespace_config));
    memset(&start_result, 0, sizeof(start_result));
    namespace_config.hostname        = container->hostname;
    namespace_config.rootfs          = container->rootfs;
    namespace_config.command_line    = container->command_line;
    namespace_config.resource_limits = container->resource_limits;
    namespace_config.security        = container->security;
    namespace_config.log_fd          = -1;
    namespace_config.net_setup       = container_net_setup;
    namespace_config.net_setup_data  = container;

    if (container->log_path[0] != '\0') {
        namespace_config.log_fd = open(container->log_path,
                                       O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (namespace_config.log_fd < 0) {
            log_event("%s warning: could not open log file %s",
                      container->id, container->log_path);
        }
    }

    /* Reset networking fields — will be filled by net_setup callback if bridge is up */
    container->ip_address[0] = '\0';
    container->veth_host[0]  = '\0';

    clock_gettime(CLOCK_MONOTONIC, &t_start);
    if (namespace_start_container(&namespace_config,
                                  container->stack,
                                  STACK_SIZE,
                                  &start_result) != 0) {
        char message[256];

        saved_errno = errno;
        print_start_error(container, saved_errno);
        format_start_error_message(saved_errno, message, sizeof(message));
        log_event("startup isolation failed for %s: %s", container->id, message);
        free_container_stack(container);
        return -1;
    }

    container->pid        = start_result.host_pid;
    container->state      = STATE_RUNNING;
    container->started_at = time(NULL);
    container->exit_code  = -1;

    /* Raise OOM kill priority so the kernel reclaims container memory first
     * under host memory pressure — mirrors Docker's oom_score_adj handling. */
    {
        char oom_path[64];
        FILE *oom_f;
        snprintf(oom_path, sizeof(oom_path),
                 "/proc/%d/oom_score_adj", (int)container->pid);
        oom_f = fopen(oom_path, "w");
        if (oom_f) { fputs("500\n", oom_f); fclose(oom_f); }
    }

    /* Apply port forwards now that we have the container IP */
    if (container->ip_address[0] != '\0') {
        int i;
        for (i = 0; i < container->port_map_count; i++) {
            bridge_add_port_forward(&container->port_maps[i], container->ip_address);
        }
    }

    if (resource_try_cgroup(container->id, &container->resource_limits, container->pid) == 0) {
        log_event("%s cgroup applied (mem=%uMB nproc=%u)",
                  container->id,
                  container->resource_limits.memory_mb,
                  container->resource_limits.max_processes);
    } else {
        log_event("%s cgroup unavailable, rlimits active", container->id);
    }

    if (save_metadata() != 0) {
        stop_container_process(container, 1, 0);
        return -1;
    }

    printf("[manager] started %s\n", container->id);
    print_container_banner(container, start_result.namespace_pid);

    /* Signal child to execvp — must happen AFTER banner is printed */
    if (namespace_trigger_exec(&start_result) != 0) {
        saved_errno = errno;
        log_event("%s execvp failed inside container: %s", container->id, strerror(saved_errno));
        printf("[error] command failed inside container: %s\n\n", strerror(saved_errno));
        stop_container_process(container, 1, 0);
        return -1;
    }

    log_event("%s STARTED (pid %d, ns_pid %d, ip=%s, isolation=%s, command=%s, cpu=%us mem=%uMB nproc=%u)",
              container->id,
              container->pid,
              start_result.namespace_pid,
              container->ip_address[0] ? container->ip_address : "none",
              namespace_profile(),
              container->command_line,
              container->resource_limits.cpu_seconds,
              container->resource_limits.memory_mb,
              container->resource_limits.max_processes);

    {
        unsigned long elapsed_ms;
        int i, cap_drop_count = 0;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        elapsed_ms = (unsigned long)(t_end.tv_sec - t_start.tv_sec) * 1000UL
                   + (unsigned long)(t_end.tv_nsec - t_start.tv_nsec) / 1000000UL;
        metrics_record_startup_ms(elapsed_ms);
        eventbus_emit(EVENT_CONTAINER_STARTED, container->id,
                      container->command_line, (long)elapsed_ms);
        for (i = 0; i <= 37; i++)
            if (!(container->security.cap_keep & (1ULL << i)))
                cap_drop_count++;
        eventbus_emit(EVENT_SECURITY_PROFILE_APPLIED, container->id, NULL,
                      (long)cap_drop_count);
    }

    if (schedule_target) {
        container_scheduler_on_started(container->pid);
    }

    return 0;
}

int container_run(const ContainerSpec *spec, char *out_id, size_t out_id_size) {
    char container_id[CONTAINER_ID_LEN];
    Container *container = NULL;

    if (container_create(spec, container_id, sizeof(container_id)) != 0) {
        return -1;
    }

    if (start_container_by_id(container_id, 0) != 0) {
        return -1;
    }

    if (out_id != NULL && out_id_size > 0) {
        copy_string(out_id, out_id_size, container_id);
    }

    container = find_container(container_id);
    if (container == NULL) {
        errno = ESRCH;
        return -1;
    }

    return wait_for_container_process(container, 0);
}

int container_run_background(const ContainerSpec *spec, char *out_id, size_t out_id_size) {
    char container_id[CONTAINER_ID_LEN];
    Container *container = NULL;

    if (container_create(spec, container_id, sizeof(container_id)) != 0) {
        return -1;
    }

    container = find_container(container_id);
    if (container != NULL) {
        mkdir("logs", 0755);
        snprintf(container->log_path, sizeof(container->log_path),
                 "logs/%s.log", container_id);
        save_metadata();
    }

    if (start_container_by_id(container_id, 1) != 0) {
        return -1;
    }

    if (out_id != NULL && out_id_size > 0) {
        copy_string(out_id, out_id_size, container_id);
    }

    return 0;
}

int container_start(const char *id) {
    return start_container_by_id(id, 1);
}

int container_stop(const char *id, int timeout_s) {
    Container *container = NULL;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: stop <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    return stop_container_process(container, 0, timeout_s);
}

int container_delete(const char *id) {
    Container *container = NULL;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: delete <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->state == STATE_RUNNING) {
        printf("[error] stop %s before deleting it\n\n", container->id);
        return -1;
    }

    if (container->state == STATE_PAUSED) {
        printf("[error] unpause %s before deleting it\n\n", container->id);
        return -1;
    }

    remove_container(container);
    if (save_metadata() != 0) {
        append_container(container);
        return -1;
    }

    log_event("%s DELETED", container->id);
    eventbus_emit(EVENT_CONTAINER_DELETED, container->id, NULL, 0);
    printf("[manager] deleted %s\n\n", container->id);
    free_container_stack(container);
    free(container);
    return 0;
}

static void format_uptime(time_t seconds, char *buf, size_t size) {
    if (seconds <= 0) {
        snprintf(buf, size, "-");
    } else if (seconds < 60) {
        snprintf(buf, size, "%lds", (long)seconds);
    } else if (seconds < 3600) {
        snprintf(buf, size, "%ldm%lds", (long)seconds / 60, (long)seconds % 60);
    } else {
        snprintf(buf, size, "%ldh%ldm", (long)seconds / 3600, ((long)seconds % 3600) / 60);
    }
}

int container_list(void) {
    int count = 0;
    time_t now = time(NULL);

    poll_states();

    printf("\n");
    printf("Isolation : %s\n", namespace_profile());
    printf("Filesystem: %s\n", filesystem_profile());
    printf("%-16s %-16s %-8s %-22s %-16s %-20s %-20s\n",
           "ID", "NAME", "PID", "STATUS", "IP", "PORTS", "COMMAND");
    printf("----------------------------------------------------------------------------------------------------------\n");

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        char pid_text[16];
        char status_text[48];
        char ports_text[64];
        char uptime_buf[24];
        const char *ip_text;

        if (cursor->pid > 0) {
            snprintf(pid_text, sizeof(pid_text), "%d", (int)cursor->pid);
        } else {
            copy_string(pid_text, sizeof(pid_text), "-");
        }

        /* Docker-style STATUS: "Up 2m30s" / "Exited (0)" / "Paused" / "Created" */
        switch (cursor->state) {
        case STATE_RUNNING:
            if (cursor->started_at > 0) {
                format_uptime(now - cursor->started_at, uptime_buf, sizeof(uptime_buf));
                snprintf(status_text, sizeof(status_text), "Up %s", uptime_buf);
            } else {
                copy_string(status_text, sizeof(status_text), "Up");
            }
            break;
        case STATE_PAUSED:
            if (cursor->started_at > 0) {
                format_uptime(now - cursor->started_at, uptime_buf, sizeof(uptime_buf));
                snprintf(status_text, sizeof(status_text), "Paused (%s)", uptime_buf);
            } else {
                copy_string(status_text, sizeof(status_text), "Paused");
            }
            break;
        case STATE_STOPPED:
            if (cursor->exit_code >= 0)
                snprintf(status_text, sizeof(status_text), "Exited (%d)", cursor->exit_code);
            else
                copy_string(status_text, sizeof(status_text), "Exited");
            break;
        default:
            copy_string(status_text, sizeof(status_text), "Created");
            break;
        }

        ip_text = cursor->ip_address[0] ? cursor->ip_address : "-";

        if (cursor->port_map_count > 0) {
            bridge_serialize_port_maps(cursor->port_maps, cursor->port_map_count,
                                       ports_text, sizeof(ports_text));
        } else {
            copy_string(ports_text, sizeof(ports_text), "-");
        }

        printf("%-16s %-16s %-8s %-22s %-16s %-20s %-20s\n",
               cursor->id,
               cursor->name,
               pid_text,
               status_text,
               ip_text,
               ports_text,
               cursor->command_line);
        count++;
    }

    if (count == 0) {
        printf("no containers found\n");
    }

    printf("\n");
    return 0;
}

/* ── CPU-delta helpers (shared by stats, stats_all, and stats --watch) ──── */

typedef struct {
    pid_t              pid;
    double             cpu_seconds;
    unsigned long long wall_ns;
} CpuSample;

static unsigned long long monotonic_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL
         + (unsigned long long)ts.tv_nsec;
}

static int find_sample(CpuSample *samples, size_t count, pid_t pid) {
    for (size_t i = 0; i < count; i++)
        if (samples[i].pid == pid) return (int)i;
    return -1;
}

static void print_stats_header(void) {
    printf("\n");
    printf("Monitor profile: %s\n", monitor_profile());
    printf("%-16s %-8s %-6s %-10s %-8s %-10s %-8s %-10s %-6s %-24s\n",
           "ID",
           "PID",
           "STATE",
           "CPU(s)",
           "CPU(%)",
           "RSS(MB)",
           "MEM%",
           "VSZ(MB)",
           "THR",
           "COMMAND");
    printf("-----------------------------------------------------------------------------------------------------------------------\n");
}

static void print_stats_row(const Container *container, const MonitorStats *stats, int has_cpu_pct, double cpu_pct) {
    double rss_mb = 0.0;
    double vsize_mb = 0.0;
    char state_text[8];
    char cpu_pct_text[16];
    char mem_pct_text[16];

    if (container == NULL || stats == NULL) {
        return;
    }

    rss_mb = (double)stats->rss_bytes / (1024.0 * 1024.0);
    vsize_mb = (double)stats->vsize_bytes / (1024.0 * 1024.0);
    snprintf(state_text, sizeof(state_text), "%s", state_to_string(container->state));

    if (!has_cpu_pct) {
        snprintf(cpu_pct_text, sizeof(cpu_pct_text), "-");
    } else {
        snprintf(cpu_pct_text, sizeof(cpu_pct_text), "%.1f", cpu_pct);
    }

    if (container->resource_limits.memory_mb > 0 && stats->rss_bytes > 0) {
        double limit_bytes = (double)container->resource_limits.memory_mb * 1024.0 * 1024.0;
        snprintf(mem_pct_text, sizeof(mem_pct_text), "%.1f%%", (stats->rss_bytes / limit_bytes) * 100.0);
    } else {
        snprintf(mem_pct_text, sizeof(mem_pct_text), "-");
    }

    printf("%-16s %-8d %-6s %-10.2f %-8s %-10.1f %-8s %-10.1f %-6ld %-24s\n",
           container->id,
           (int)stats->pid,
           state_text,
           stats->cpu_seconds,
           cpu_pct_text,
           rss_mb,
           mem_pct_text,
           vsize_mb,
           stats->threads,
           container->command_line);
}

int container_stats(const char *id) {
    Container *container = NULL;
    MonitorStats s1, s2;
    unsigned long long t1, t2;
    double cpu_pct = 0.0;
    int has_cpu_pct = 0;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: stats <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if ((container->state != STATE_RUNNING && container->state != STATE_PAUSED) ||
        container->pid <= 0) {
        printf("[manager] %s is not running\n\n", container->id);
        return -1;
    }

    /* Take two samples 300 ms apart to compute instantaneous CPU%. */
    if (monitor_read(container->pid, &s1) != 0) {
        printf("[error] failed to read stats for %s\n\n", container->id);
        return -1;
    }
    t1 = monotonic_ns();
    struct timespec delta_sleep = {0, 300000000L};
    nanosleep(&delta_sleep, NULL);
    if (monitor_read(container->pid, &s2) == 0) {
        t2 = monotonic_ns();
        double delta_cpu  = s2.cpu_seconds - s1.cpu_seconds;
        double delta_wall = (double)(t2 - t1) / 1e9;
        if (delta_wall > 0.0 && delta_cpu >= 0.0) {
            cpu_pct = (delta_cpu / delta_wall) * 100.0;
            has_cpu_pct = 1;
        }
    } else {
        s2 = s1;
    }

    print_stats_header();
    print_stats_row(container, &s2, has_cpu_pct, cpu_pct);
    printf("\n");
    return 0;
}

int container_stats_all(void) {
    /* Collect first samples from all running containers, sleep 300 ms,
     * then collect second samples to compute per-container CPU%. */
    CpuSample first[64];
    unsigned long long t1_ns, t2_ns;
    int fc = 0;
    int any = 0;

    poll_states();

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        MonitorStats s;
        if ((cursor->state != STATE_RUNNING && cursor->state != STATE_PAUSED) ||
            cursor->pid <= 0) continue;
        if (monitor_read(cursor->pid, &s) != 0) continue;
        if (fc < 64) {
            first[fc].pid         = cursor->pid;
            first[fc].cpu_seconds = s.cpu_seconds;
            first[fc].wall_ns     = 0;
            fc++;
        }
    }

    t1_ns = monotonic_ns();
    struct timespec delta_sleep = {0, 300000000L};
    nanosleep(&delta_sleep, NULL);
    t2_ns = monotonic_ns();

    print_stats_header();
    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        MonitorStats stats;

        if ((cursor->state != STATE_RUNNING && cursor->state != STATE_PAUSED) ||
            cursor->pid <= 0) {
            continue;
        }

        if (monitor_read(cursor->pid, &stats) != 0) {
            continue;
        }

        metrics_update_mem_highwater(
            (unsigned long)(stats.rss_bytes / (1024 * 1024)));

        double cpu_pct = 0.0;
        int has_cpu_pct = 0;
        int fi = find_sample(first, (size_t)fc, cursor->pid);
        if (fi >= 0) {
            double delta_cpu  = stats.cpu_seconds - first[fi].cpu_seconds;
            double delta_wall = (double)(t2_ns - t1_ns) / 1e9;
            if (delta_wall > 0.0 && delta_cpu >= 0.0) {
                cpu_pct = (delta_cpu / delta_wall) * 100.0;
                has_cpu_pct = 1;
            }
        }

        print_stats_row(cursor, &stats, has_cpu_pct, cpu_pct);
        any = 1;
    }

    if (!any) {
        printf("no running containers\n");
    }

    printf("\n");
    return 0;
}

static void sleep_interval(unsigned int interval_sec) {
    struct timespec ts;
    ts.tv_sec = (time_t)interval_sec;
    ts.tv_nsec = 0;
    while (nanosleep(&ts, &ts) != 0) {
        if (errno == EINTR) {
            return;
        }
        return;
    }
}

static void clear_screen_if_tty(void) {
    if (!isatty(STDOUT_FILENO)) {
        return;
    }

    /* ANSI clear screen + cursor home. */
    fputs("\033[H\033[J", stdout);
}

int container_stats_watch(const char *id, unsigned int interval_sec) {
    CpuSample prev = {0};
    int has_prev = 0;

    if (interval_sec == 0) {
        printf("[error] usage: stats --watch <sec> [id]\n\n");
        return -1;
    }

    printf("[hint] watching every %us; press Ctrl+C to stop\n", interval_sec);

    while (1) {
        if (container_consume_interrupt()) {
            printf("\n");
            return 0;
        }

        Container *container = NULL;
        MonitorStats stats;
        unsigned long long now = 0;
        double cpu_pct = 0.0;
        int has_cpu_pct = 0;

        poll_states();
        container = find_container(id);
        if (container == NULL) {
            printf("\n[error] container %s not found\n\n", id);
            return -1;
        }

        if (container->state != STATE_RUNNING || container->pid <= 0) {
            printf("\n[manager] %s is not running\n\n", container->id);
            return -1;
        }

        if (monitor_read(container->pid, &stats) != 0) {
            printf("\n[error] failed to read stats for %s (pid %d)\n\n", container->id, (int)container->pid);
            return -1;
        }

        now = monotonic_ns();
        if (has_prev && prev.pid == stats.pid) {
            double delta_cpu = stats.cpu_seconds - prev.cpu_seconds;
            double delta_wall = (double)(now - prev.wall_ns) / 1e9;
            if (delta_wall > 0.0 && delta_cpu >= 0.0) {
                cpu_pct = (delta_cpu / delta_wall) * 100.0;
                has_cpu_pct = 1;
            }
        }

        clear_screen_if_tty();
        printf("[watch] interval=%us (Ctrl+C to stop)\n", interval_sec);
        print_stats_header();
        print_stats_row(container, &stats, has_cpu_pct, cpu_pct);
        printf("\n");

        prev.pid = stats.pid;
        prev.cpu_seconds = stats.cpu_seconds;
        prev.wall_ns = now;
        has_prev = 1;

        sleep_interval(interval_sec);
    }
}

int container_stats_all_watch(unsigned int interval_sec) {
    CpuSample *prev = NULL;
    size_t prev_count = 0;
    size_t prev_capacity = 0;

    if (interval_sec == 0) {
        printf("[error] usage: stats --watch <sec> [id]\n\n");
        return -1;
    }

    printf("[hint] watching every %us; press Ctrl+C to stop\n", interval_sec);

    while (1) {
        if (container_consume_interrupt()) {
            printf("\n");
            free(prev);
            return 0;
        }

        unsigned long long now = monotonic_ns();
        int any = 0;

        poll_states();
        clear_screen_if_tty();
        printf("[watch] interval=%us (Ctrl+C to stop)\n", interval_sec);
        print_stats_header();

        for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
            MonitorStats stats;
            double cpu_pct = 0.0;
            int has_cpu_pct = 0;
            int index = -1;

            if (cursor->state != STATE_RUNNING || cursor->pid <= 0) {
                continue;
            }

            if (monitor_read(cursor->pid, &stats) != 0) {
                continue;
            }

            metrics_update_mem_highwater(
                (unsigned long)(stats.rss_bytes / (1024 * 1024)));

            index = find_sample(prev, prev_count, stats.pid);
            if (index >= 0) {
                double delta_cpu = stats.cpu_seconds - prev[index].cpu_seconds;
                double delta_wall = (double)(now - prev[index].wall_ns) / 1e9;
                if (delta_wall > 0.0 && delta_cpu >= 0.0) {
                    cpu_pct = (delta_cpu / delta_wall) * 100.0;
                    has_cpu_pct = 1;
                }

                prev[index].cpu_seconds = stats.cpu_seconds;
                prev[index].wall_ns = now;
            } else {
                if (prev_count == prev_capacity) {
                    size_t next_capacity = (prev_capacity == 0) ? 8 : prev_capacity * 2;
                    CpuSample *next = realloc(prev, next_capacity * sizeof(*next));
                    if (next == NULL) {
                        free(prev);
                        printf("\n[error] out of memory\n\n");
                        return -1;
                    }
                    prev = next;
                    prev_capacity = next_capacity;
                }

                prev[prev_count].pid = stats.pid;
                prev[prev_count].cpu_seconds = stats.cpu_seconds;
                prev[prev_count].wall_ns = now;
                prev_count++;
            }

            print_stats_row(cursor, &stats, has_cpu_pct, cpu_pct);
            any = 1;
        }

        if (!any) {
            printf("no running containers\n");
        }

        printf("\n");
        sleep_interval(interval_sec);
    }
}

static void print_proc_tree(pid_t root_pid) {
    pid_t children[128];
    int child_count = 0;
    DIR *proc_dir;
    struct dirent *entry;

    proc_dir = opendir("/proc");
    if (proc_dir == NULL) {
        return;
    }

    while ((entry = readdir(proc_dir)) != NULL && child_count < 128) {
        pid_t pid;
        char status_path[64];
        FILE *f;
        char line[128];

        pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0) {
            continue;
        }

        snprintf(status_path, sizeof(status_path), "/proc/%d/status", (int)pid);
        f = fopen(status_path, "r");
        if (f == NULL) {
            continue;
        }

        while (fgets(line, sizeof(line), f) != NULL) {
            if (strncmp(line, "PPid:", 5) == 0) {
                if ((pid_t)atoi(line + 5) == root_pid) {
                    children[child_count++] = pid;
                }
                break;
            }
        }
        fclose(f);
    }
    closedir(proc_dir);

    {
        char comm[64] = "?";
        char comm_path[64];
        FILE *f;

        snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)root_pid);
        f = fopen(comm_path, "r");
        if (f != NULL) {
            if (fgets(comm, sizeof(comm), f) != NULL) {
                comm[strcspn(comm, "\n")] = '\0';
            }
            fclose(f);
        }

        printf("  \"ProcessTree\" : [\n");
        printf("    \"%d (%s)\"", (int)root_pid, comm);

        for (int i = 0; i < child_count; i++) {
            char child_comm[64] = "?";
            snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", (int)children[i]);
            f = fopen(comm_path, "r");
            if (f != NULL) {
                if (fgets(child_comm, sizeof(child_comm), f) != NULL) {
                    child_comm[strcspn(child_comm, "\n")] = '\0';
                }
                fclose(f);
            }
            const char *branch = (i == child_count - 1) ? "└── " : "├── ";
            printf(",\n    \"%s%d (%s)\"", branch, (int)children[i], child_comm);
        }
        printf("\n  ]");
    }
}

static long read_oom_kill_count(const char *container_id) {
    char path[256];
    FILE *f;
    char line[128];

    snprintf(path, sizeof(path), "/sys/fs/cgroup/%s/memory.events", container_id);
    f = fopen(path, "r");
    if (f == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "oom_kill ", 9) == 0) {
            long count = atol(line + 9);
            fclose(f);
            return count;
        }
    }
    fclose(f);
    return 0;
}

int container_inspect(const char *id) {
    Container *container = NULL;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: inspect <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    {
        char started_buf[32] = "-";
        char stopped_buf[32] = "-";
        char uptime_buf[24]  = "-";
        char exit_buf[16];
        time_t now = time(NULL);

        if (container->started_at > 0) {
            strftime(started_buf, sizeof(started_buf), "%Y-%m-%dT%H:%M:%S",
                     localtime(&container->started_at));
        }
        if (container->stopped_at > 0) {
            strftime(stopped_buf, sizeof(stopped_buf), "%Y-%m-%dT%H:%M:%S",
                     localtime(&container->stopped_at));
        }
        if (container->state == STATE_RUNNING && container->started_at > 0) {
            format_uptime(now - container->started_at, uptime_buf, sizeof(uptime_buf));
        } else if (container->started_at > 0 && container->stopped_at > 0) {
            format_uptime(container->stopped_at - container->started_at,
                          uptime_buf, sizeof(uptime_buf));
        }
        if (container->state == STATE_CREATED || container->state == STATE_RUNNING ||
            container->state == STATE_PAUSED) {
            snprintf(exit_buf, sizeof(exit_buf), "-");
        } else if (container->exit_code == -2) {
            snprintf(exit_buf, sizeof(exit_buf), "killed");
        } else {
            snprintf(exit_buf, sizeof(exit_buf), "%d", container->exit_code);
        }

        printf("{\n");
        printf("  \"Id\"         : \"%s\",\n", container->id);
        printf("  \"Name\"       : \"%s\",\n", container->name);
        printf("  \"Image\"      : \"%s\",\n",
               container->image_ref[0] ? container->image_ref : "(none)");
        printf("  \"Pid\"        : %d,\n", (int)container->pid);
        printf("  \"State\"      : \"%s\",\n", state_to_string(container->state));
        printf("  \"ExitCode\"   : \"%s\",\n", exit_buf);
        printf("  \"StartedAt\"  : \"%s\",\n", started_buf);
        printf("  \"StoppedAt\"  : \"%s\",\n", stopped_buf);
        printf("  \"Uptime\"     : \"%s\",\n", uptime_buf);
        printf("  \"Hostname\"   : \"%s\",\n", container->hostname);
        printf("  \"Rootfs\"     : \"%s\",\n", container->rootfs);
        printf("  \"Command\"    : \"%s\",\n", container->command_line);
        printf("  \"LogPath\"    : \"%s\",\n",
               container->log_path[0] ? container->log_path : "(none)");
        printf("  \"Limits\"     : {\n");
        printf("    \"CpuSeconds\"   : %u,\n", container->resource_limits.cpu_seconds);
        printf("    \"MemoryMB\"     : %u,\n", container->resource_limits.memory_mb);
        printf("    \"MaxProcesses\" : %u\n",  container->resource_limits.max_processes);
        printf("  },\n");
        printf("  \"Isolation\"  : \"%s\",\n", namespace_profile());
        printf("  \"Filesystem\" : \"%s\",\n", filesystem_profile());
        printf("  \"Resources\"  : \"%s\",\n", resource_profile());
        {
            char sec_buf[640];
            security_format_inspect(&container->security, sec_buf, sizeof(sec_buf));
            printf("  \"SecurityProfile\" : %s,\n", sec_buf);
        }
        printf("  \"NetworkSettings\" : {\n");
        printf("    \"Profile\"   : \"%s\",\n", network_profile());
        printf("    \"Bridge\"    : \"%s\",\n",
               container->veth_host[0] ? BRIDGE_NAME : "");
        printf("    \"IPAddress\" : \"%s\",\n",
               container->ip_address[0] ? container->ip_address : "");
        printf("    \"Gateway\"   : \"%s\",\n",
               container->ip_address[0] ? BRIDGE_IP : "");
        printf("    \"VethHost\"  : \"%s\",\n",
               container->veth_host[0] ? container->veth_host : "");
        printf("    \"Ports\"     : {");
        if (container->port_map_count > 0) {
            int pi;
            printf("\n");
            for (pi = 0; pi < container->port_map_count; pi++) {
                const PortMapping *pm = &container->port_maps[pi];
                printf("      \"%d/%s\" : [{\"HostIp\": \"0.0.0.0\", \"HostPort\": \"%d\"}]%s\n",
                       (int)pm->container_port, pm->proto, (int)pm->host_port,
                       (pi < container->port_map_count - 1) ? "," : "");
            }
            printf("    }");
        } else {
            printf("}");
        }
        printf("\n  }");

        if ((container->state == STATE_RUNNING || container->state == STATE_PAUSED) &&
            container->pid > 0) {
            long oom_kills = read_oom_kill_count(container->id);
            printf(",\n  \"OomKills\"   : %ld", (oom_kills >= 0) ? oom_kills : 0);
            printf(",\n");
            print_proc_tree(container->pid);
            printf("\n");
        } else {
            printf("\n");
        }
        printf("}\n\n");
    }

    log_event("%s INSPECTED", container->id);
    return 0;
}

int container_exec(const char *id, const char *command_line) {
    Container *container = NULL;
    char cmd_buf[256];
    char *argv[32];
    char *token;
    int argc = 0;
    /* uts, net, mnt — entered in this order; mnt is best-effort */
    const char *ns_names[] = {"uts", "net", "mnt"};
    int ns_types[]          = {CLONE_NEWUTS, CLONE_NEWNET, CLONE_NEWNS};
    int ns_fds[3]           = {-1, -1, -1};
    char ns_path[128];
    int entered_mnt = 0;
    pid_t child;
    int status;
    int i;

    if (id == NULL || command_line == NULL || command_line[0] == '\0') {
        printf("[error] usage: exec <id> <command> [args...]\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }
    if (container->state != STATE_RUNNING || container->pid <= 0) {
        printf("[error] %s is not running (state: %s)\n\n",
               id, state_to_string(container->state));
        return -1;
    }

    if (snprintf(cmd_buf, sizeof(cmd_buf), "%s", command_line) >= (int)sizeof(cmd_buf)) {
        printf("[error] command too long\n\n");
        return -1;
    }

    token = strtok(cmd_buf, " ");
    while (token != NULL && argc < (int)(sizeof(argv) / sizeof(argv[0])) - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    if (argc == 0) {
        printf("[error] empty command\n\n");
        return -1;
    }
    argv[argc] = NULL;

    for (i = 0; i < 3; i++) {
        snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/%s",
                 (int)container->pid, ns_names[i]);
        ns_fds[i] = open(ns_path, O_RDONLY);
    }

    child = fork();
    if (child < 0) {
        printf("[error] fork failed: %s\n\n", strerror(errno));
        for (i = 0; i < 3; i++) if (ns_fds[i] >= 0) close(ns_fds[i]);
        return -1;
    }

    if (child == 0) {
        /* Enter UTS and NET namespaces — required for hostname/network isolation. */
        for (i = 0; i < 2; i++) {
            if (ns_fds[i] >= 0) {
                if (setns(ns_fds[i], ns_types[i]) != 0) {
                    /* Best-effort on WSL/unprivileged — silently skip. */
                }
                close(ns_fds[i]);
            }
        }

        /* Enter mount namespace — gives the container's full filesystem view.
         * Best-effort: on WSL or with restricted permissions this may fail, in
         * which case fall back to the /proc/<pid>/root bind trick below. */
        if (ns_fds[2] >= 0) {
            if (setns(ns_fds[2], CLONE_NEWNS) == 0) {
                entered_mnt = 1;
            }
            close(ns_fds[2]);
        }

        if (entered_mnt) {
            /* We're in the container's mount namespace; / is already its rootfs. */
            chdir("/");
        } else {
            /* Fallback: bind the container rootfs via /proc/<pid>/root. */
            char root_path[128];
            snprintf(root_path, sizeof(root_path), "/proc/%d/root", (int)container->pid);
            if (chdir(root_path) == 0) {
                chroot(".");
                chdir("/");
            }
        }

        execvp(argv[0], argv);
        fprintf(stderr, "[exec] %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    for (i = 0; i < 3; i++) if (ns_fds[i] >= 0) close(ns_fds[i]);

    waitpid(child, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
        printf("[exec] command not found in container: %s\n\n", argv[0]);
        return -1;
    }

    log_event("%s EXEC \"%s\" (exit=%d)", container->id, command_line,
              WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    eventbus_emit(EVENT_EXEC_LAUNCHED, container->id, command_line,
                  (long)(WIFEXITED(status) ? WEXITSTATUS(status) : -1));
    return 0;
}

/* ── orchestrator helpers ────────────────────────────────────────────────── */

int container_get_info(const char *id, ContainerState *state,
                       int *exit_code, pid_t *pid) {
    poll_states();
    Container *c = find_container(id);
    if (!c) return -1;
    if (state)     *state     = c->state;
    if (exit_code) *exit_code = c->exit_code;
    if (pid)       *pid       = c->pid;
    return 0;
}

int container_exec_quiet(const char *id, const char *command_line) {
    Container *container = NULL;
    char cmd_buf[256];
    char *argv[32];
    char *token;
    int argc = 0;
    const char *ns_names[] = {"uts", "net", "mnt"};
    int ns_types[]          = {CLONE_NEWUTS, CLONE_NEWNET, CLONE_NEWNS};
    int ns_fds[3]           = {-1, -1, -1};
    char ns_path[128];
    pid_t child;
    int status, i;

    poll_states();
    container = find_container(id);
    if (!container || container->state != STATE_RUNNING || container->pid <= 0)
        return -1;

    if (snprintf(cmd_buf, sizeof(cmd_buf), "%s", command_line) >= (int)sizeof(cmd_buf))
        return -1;

    token = strtok(cmd_buf, " ");
    while (token && argc < (int)(sizeof(argv)/sizeof(argv[0])) - 1) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    if (argc == 0) return -1;
    argv[argc] = NULL;

    for (i = 0; i < 3; i++) {
        snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/%s",
                 (int)container->pid, ns_names[i]);
        ns_fds[i] = open(ns_path, O_RDONLY);
    }

    child = fork();
    if (child < 0) {
        for (i = 0; i < 3; i++) if (ns_fds[i] >= 0) close(ns_fds[i]);
        return -1;
    }

    if (child == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }

        for (i = 0; i < 2; i++) {
            if (ns_fds[i] >= 0) { setns(ns_fds[i], ns_types[i]); close(ns_fds[i]); }
        }
        if (ns_fds[2] >= 0) {
            if (setns(ns_fds[2], CLONE_NEWNS) == 0) chdir("/");
            else {
                char rp[128];
                snprintf(rp, sizeof(rp), "/proc/%d/root", (int)container->pid);
                if (chdir(rp) == 0) { chroot("."); chdir("/"); }
            }
            close(ns_fds[2]);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    for (i = 0; i < 3; i++) if (ns_fds[i] >= 0) close(ns_fds[i]);
    waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int container_pause(const char *id) {
    Container *container = NULL;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: pause <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->state != STATE_RUNNING || container->pid <= 0) {
        printf("[error] %s is not running\n\n", container->id);
        return -1;
    }

    if (kill(container->pid, SIGSTOP) != 0) {
        printf("[error] failed to pause %s: %s\n\n", container->id, strerror(errno));
        return -1;
    }

    container->state = STATE_PAUSED;
    save_metadata();
    printf("[manager] %s paused\n\n", container->id);
    log_event("%s PAUSED (pid %d)", container->id, (int)container->pid);
    eventbus_emit(EVENT_CONTAINER_PAUSED, container->id, NULL, 0);
    return 0;
}

int container_unpause(const char *id) {
    Container *container = NULL;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: unpause <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->state != STATE_PAUSED || container->pid <= 0) {
        printf("[error] %s is not paused\n\n", container->id);
        return -1;
    }

    if (kill(container->pid, SIGCONT) != 0) {
        printf("[error] failed to unpause %s: %s\n\n", container->id, strerror(errno));
        return -1;
    }

    container->state = STATE_RUNNING;
    save_metadata();
    printf("[manager] %s unpaused\n\n", container->id);
    log_event("%s UNPAUSED (pid %d)", container->id, (int)container->pid);
    eventbus_emit(EVENT_CONTAINER_UNPAUSED, container->id, NULL, 0);
    return 0;
}

int container_logs(const char *id) {
    Container *container = NULL;
    FILE *f = NULL;
    char line[1024];

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: logs <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->log_path[0] == '\0') {
        printf("[manager] %s has no log file (use runbg to capture output)\n\n", id);
        return 0;
    }

    f = fopen(container->log_path, "r");
    if (f == NULL) {
        printf("[manager] log file not yet created for %s\n\n", id);
        return 0;
    }

    printf("\n--- logs: %s ---\n", container->log_path);
    while (fgets(line, sizeof(line), f) != NULL) {
        printf("%s", line);
    }
    printf("--- end ---\n\n");
    fclose(f);
    return 0;
}

int container_logs_tail(const char *id, int n) {
    Container *container = NULL;
    FILE *f = NULL;
    char **ring = NULL;
    int head_idx = 0, count = 0;
    char line[1024];

    if (id == NULL || id[0] == '\0' || n <= 0) {
        printf("[error] usage: logs -n N <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->log_path[0] == '\0') {
        printf("[manager] %s has no log file (use runbg to capture output)\n\n", id);
        return 0;
    }

    f = fopen(container->log_path, "r");
    if (f == NULL) {
        printf("[manager] log file not yet created for %s\n\n", id);
        return 0;
    }

    ring = calloc((size_t)n, sizeof(char *));
    if (ring == NULL) { fclose(f); return -1; }

    while (fgets(line, sizeof(line), f) != NULL) {
        free(ring[head_idx]);
        ring[head_idx] = strdup(line);
        head_idx = (head_idx + 1) % n;
        if (count < n) count++;
    }
    fclose(f);

    printf("\n--- logs (last %d): %s ---\n", count, container->log_path);
    int start = (count < n) ? 0 : head_idx;
    for (int i = 0; i < count; i++) {
        int idx = (start + i) % n;
        if (ring[idx]) printf("%s", ring[idx]);
    }
    printf("--- end ---\n\n");

    for (int i = 0; i < n; i++) free(ring[i]);
    free(ring);
    return 0;
}

int container_logs_follow(const char *id) {
    Container *container = NULL;
    FILE *f = NULL;
    char buf[4096];
    size_t n;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: logs -f <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    if (container->log_path[0] == '\0') {
        printf("[manager] %s has no log file (use runbg to capture output)\n\n", id);
        return 0;
    }

    f = fopen(container->log_path, "r");
    if (f == NULL) {
        printf("[manager] log file not yet created for %s\n\n", id);
        return 0;
    }

    if (isatty(STDOUT_FILENO)) {
        printf("[hint] following logs for %s — press Ctrl+C to stop\n", id);
    }
    fflush(stdout);

    while (1) {
        /* Drain all available bytes from the file */
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            fwrite(buf, 1, n, stdout);
            fflush(stdout);
        }
        clearerr(f);

        if (container_consume_interrupt()) {
            printf("\n");
            break;
        }

        /* Re-poll container state so we know when it's done */
        poll_states();
        container = find_container(id);
        if (container == NULL || container->state == STATE_STOPPED) {
            /* One final drain before exiting */
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                fwrite(buf, 1, n, stdout);
                fflush(stdout);
            }
            break;
        }

        /* Wait 50ms before next poll */
        {
            struct timespec ts = {0, 50000000};
            nanosleep(&ts, NULL);
        }
    }

    fclose(f);
    return 0;
}

int container_security_show(const char *id) {
    Container *container = NULL;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: security <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    printf("\nSecurity profile: %s\n", container->id);
    security_print_detail(&container->security);
    printf("\n");

    log_event("%s SECURITY INSPECTED", container->id);
    return 0;
}

int container_net_summary(void) {
    int any_networked = 0;
    char ports_buf[64];

    poll_states();

    printf("\n");
    printf("  Bridge    : %-12s  %s\n", BRIDGE_NAME,
           bridge_is_up() ? "UP" : "DOWN (run: net init)");
    if (bridge_is_up()) {
        printf("  Subnet    : 172.17.0.0/16\n");
        printf("  Gateway   : %s\n", BRIDGE_IP);
    }
    printf("\n");
    printf("  %-18s %-18s %-16s %-24s %-10s\n",
           "CONTAINER", "NAME", "IP", "PORTS", "STATE");
    printf("  -------------------------------------------------------------------------------------\n");

    for (Container *c = head; c != NULL; c = c->next) {
        if (c->ip_address[0] == '\0') continue;

        if (c->port_map_count > 0) {
            bridge_serialize_port_maps(c->port_maps, c->port_map_count,
                                       ports_buf, sizeof(ports_buf));
        } else {
            ports_buf[0] = '\0';
            snprintf(ports_buf, sizeof(ports_buf), "-");
        }

        printf("  %-18s %-18s %-16s %-24s %-10s\n",
               c->id, c->name, c->ip_address, ports_buf,
               state_to_string(c->state));
        any_networked = 1;
    }

    if (!any_networked) {
        printf("  (no containers with bridge networking)\n");
    }
    printf("\n");
    return 0;
}

int container_net(const char *id) {
    Container *container = NULL;

    if (id == NULL || id[0] == '\0') {
        printf("[error] usage: net <id>\n\n");
        return -1;
    }

    poll_states();
    container = find_container(id);
    if (container == NULL) {
        printf("[error] container %s not found\n\n", id);
        return -1;
    }

    printf("\n");
    printf("  container : %s\n", container->id);
    printf("  state     : %s\n", state_to_string(container->state));
    printf("  profile   : %s\n", network_profile());

    if (container->state == STATE_RUNNING && container->pid > 0) {
        char netns_path[128];
        char netns_link[256];
        ssize_t n;
        snprintf(netns_path, sizeof(netns_path), "/proc/%d/ns/net", (int)container->pid);
        n = readlink(netns_path, netns_link, sizeof(netns_link) - 1);
        if (n >= 0) {
            netns_link[n] = '\0';
            printf("  pid       : %d\n", (int)container->pid);
            printf("  netns     : %s\n", netns_link);
        }
    }

    if (container->ip_address[0] != '\0') {
        printf("  ip        : %s\n", container->ip_address);
        printf("  gateway   : %s\n", BRIDGE_IP);
        printf("  veth-host : %s\n", container->veth_host);
        printf("  bridge    : %s\n", BRIDGE_NAME);
    } else {
        printf("  ip        : (no bridge / loopback only)\n");
    }
    if (container->port_map_count > 0) {
        char ports_buf[128];
        bridge_serialize_port_maps(container->port_maps, container->port_map_count,
                                   ports_buf, sizeof(ports_buf));
        printf("  ports     : %s\n", ports_buf);
    }
    printf("\n");

    return 0;
}

void cleanup_all_containers(void) {
    Container *cursor = head;

    poll_states();

    while (cursor != NULL) {
        if (cursor->state == STATE_RUNNING || cursor->state == STATE_PAUSED) {
            stop_container_process(cursor, 1, 5); /* short 5s timeout on shutdown */
        }
        cursor = cursor->next;
    }

    save_metadata();

    cursor = head;
    while (cursor != NULL) {
        Container *next = cursor->next;
        free_container_stack(cursor);
        free(cursor);
        cursor = next;
    }

    head = NULL;
    tail = NULL;

    scheduler_stop();
}

void container_prune_all(void) {
    Container *cursor;
    int deleted = 0;

    poll_states();

    cursor = head;
    while (cursor != NULL) {
        Container *next = cursor->next;
        if (cursor->state != STATE_RUNNING && cursor->state != STATE_PAUSED) {
            char id_copy[CONTAINER_ID_LEN];
            copy_string(id_copy, sizeof(id_copy), cursor->id);
            remove_container(cursor);
            log_event("%s DELETED (prune)", id_copy);
            eventbus_emit(EVENT_CONTAINER_DELETED, id_copy, NULL, 0);
            free_container_stack(cursor);
            free(cursor);
            deleted++;
        }
        cursor = next;
    }

    /* reset sequence so next container starts from 0001 */
    next_sequence = 1;
    save_metadata();

    printf("[manager] pruned %d container%s — counter reset to 1\n\n",
           deleted, deleted == 1 ? "" : "s");
}

/* ── web API helpers ─────────────────────────────────────────────────────
 * Called from a webserver thread — reads the list without a lock.
 * Callers treat the output as a best-effort snapshot; slight staleness
 * is acceptable for a live-monitoring dashboard.
 * ─────────────────────────────────────────────────────────────────────── */

int container_json_all(char *buf, int buflen) {
    Container *c;
    int written = 0;
    int first   = 1;
    time_t now  = time(NULL);

    if (buf == NULL || buflen <= 2) return -1;

    written += snprintf(buf + written, (size_t)(buflen - written), "[");

    for (c = head; c != NULL && written < buflen - 512; c = c->next) {
        char uptime_buf[24]  = "-";
        char started_buf[32] = "-";
        char exit_buf[16]    = "-";
        char ports_buf[128]  = "";

        if (c->started_at > 0) {
            struct tm *tm_s = localtime(&c->started_at);
            strftime(started_buf, sizeof(started_buf), "%Y-%m-%dT%H:%M:%S", tm_s);
        }
        if (c->state == STATE_RUNNING && c->started_at > 0) {
            format_uptime(now - c->started_at, uptime_buf, sizeof(uptime_buf));
        } else if (c->started_at > 0 && c->stopped_at > 0) {
            format_uptime(c->stopped_at - c->started_at, uptime_buf, sizeof(uptime_buf));
        }
        if (c->state == STATE_STOPPED && c->exit_code >= 0)
            snprintf(exit_buf, sizeof(exit_buf), "%d", c->exit_code);

        if (c->port_map_count > 0)
            bridge_serialize_port_maps(c->port_maps, c->port_map_count,
                                       ports_buf, sizeof(ports_buf));

        written += snprintf(buf + written, (size_t)(buflen - written),
            "%s{"
            "\"id\":\"%s\","
            "\"name\":\"%s\","
            "\"state\":\"%s\","
            "\"pid\":%d,"
            "\"ip\":\"%s\","
            "\"ports\":\"%s\","
            "\"uptime\":\"%s\","
            "\"started_at\":\"%s\","
            "\"exit_code\":\"%s\","
            "\"command\":\"%s\","
            "\"image\":\"%s\""
            "}",
            first ? "" : ",",
            c->id,
            c->name,
            state_to_string(c->state),
            (int)c->pid,
            c->ip_address[0] ? c->ip_address : "",
            ports_buf,
            uptime_buf,
            started_buf,
            exit_buf,
            c->command_line,
            c->image_ref[0] ? c->image_ref : "");
        first = 0;
    }

    if (written < buflen - 1) {
        buf[written++] = ']';
        buf[written]   = '\0';
    }
    return written;
}

/* ── per-container RSS snapshot for the dashboard ───────────────────── */
/* Returns {"container-id":{rss_mb:N},...} — single read of /proc/pid/status */
int container_stats_json_all(char *buf, int buflen) {
    Container *c;
    int written = 0;
    int first   = 1;

    if (buf == NULL || buflen <= 2) return -1;

    written += snprintf(buf + written, (size_t)(buflen - written), "{");

    for (c = head; c != NULL && written < buflen - 128; c = c->next) {
        if (c->state != STATE_RUNNING || c->pid <= 0) continue;

        unsigned long rss_kb = 0;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/status", (int)c->pid);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "VmRSS:", 6) == 0) {
                    sscanf(line + 6, " %lu", &rss_kb);
                    break;
                }
            }
            fclose(f);
        }

        written += snprintf(buf + written, (size_t)(buflen - written),
            "%s\"%s\":{\"rss_mb\":%lu}",
            first ? "" : ",",
            c->id,
            rss_kb / 1024);
        first = 0;
    }

    if (written < buflen - 1) {
        buf[written++] = '}';
        buf[written]   = '\0';
    }
    return written;
}

int container_send_signal(const char *id, int sig) {
    Container *c;

    if (id == NULL || id[0] == '\0') return -1;

    for (c = head; c != NULL; c = c->next) {
        if (strcmp(c->id, id) == 0) {
            if (c->pid > 0 && c->state == STATE_RUNNING) {
                return kill(c->pid, sig) == 0 ? 0 : -1;
            }
            return -1;
        }
    }
    return -1;
}
