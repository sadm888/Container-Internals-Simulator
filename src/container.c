#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/wait.h>
#include <unistd.h>

#include "container.h"
#include "filesystem.h"
#include "logger.h"
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

static const char *state_to_string(ContainerState state) {
    switch (state) {
        case STATE_CREATED:
            return "CREATED";
        case STATE_RUNNING:
            return "RUNNING";
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
        fprintf(file, "%s\t%s\t%d\t%s\t%s\t%s\t%s\t%u\t%u\t%u\n",
                cursor->id,
                cursor->name,
                (int)cursor->pid,
                cursor->hostname,
                cursor->rootfs,
                state_to_string(cursor->state),
                cursor->command_line,
                cursor->resource_limits.cpu_seconds,
                cursor->resource_limits.memory_mb,
                cursor->resource_limits.max_processes);
    }

    if (fclose(file) != 0) {
        printf("[error] failed to flush container metadata\n\n");
        log_event("metadata save failed: fclose");
        return -1;
    }

    if (rename(METADATA_FILE_TMP, METADATA_FILE) != 0) {
        printf("[error] failed to finalize container metadata\n\n");
        log_event("metadata save failed: rename");
        return -1;
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

static int sync_container_state(Container *container) {
    int state_changed = 0;
    int status = 0;
    pid_t result = 0;

    if (container->state != STATE_RUNNING || container->pid <= 0) {
        return 0;
    }

    result = waitpid(container->pid, &status, WNOHANG);
    if (result > 0) {
        char reason[64];
        pid_t old_pid = container->pid;

        container->state = STATE_STOPPED;
        container->pid = -1;
        free_container_stack(container);
        container_scheduler_on_stopped(old_pid);
        format_wait_status(status, reason, sizeof(reason));
        log_event("%s exited and was reaped by manager (%s)", container->id, reason);
        return 1;
    }

    if (result == 0) {
        return 0;
    }

    if (errno == ECHILD && !is_pid_alive(container->pid)) {
        container->state = STATE_STOPPED;
        container->pid = -1;
        free_container_stack(container);
        log_event("%s marked stopped after recovery check", container->id);
        state_changed = 1;
    }

    return state_changed;
}

static void poll_states(void) {
    int changed = 0;

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
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
    container->pid = -1;
    container->state = STATE_STOPPED;
    free_container_stack(container);

    if (save_metadata() != 0) {
        return -1;
    }

    if (!quiet) {
        printf("[manager] %s finished\n\n", container->id);
    }

    return 0;
}

static int stop_container_process(Container *container, int quiet) {
    if (container->state != STATE_RUNNING || container->pid <= 0) {
        if (!quiet) {
            printf("[manager] %s is not running\n\n", container->id);
        }
        return -1;
    }

    if (kill(container->pid, SIGKILL) != 0 && errno != ESRCH) {
        if (!quiet) {
            printf("[error] failed to stop %s\n\n", container->id);
        }
        log_event("failed to stop %s (pid %d)", container->id, container->pid);
        return -1;
    }

    if (waitpid(container->pid, NULL, 0) < 0 && errno != ECHILD) {
        if (!quiet) {
            printf("[error] failed to reap %s\n\n", container->id);
        }
        log_event("failed to reap %s (pid %d)", container->id, container->pid);
        return -1;
    }

    log_event("%s STOPPED (pid %d)", container->id, container->pid);
    container_scheduler_on_stopped(container->pid);
    container->pid = -1;
    container->state = STATE_STOPPED;
    free_container_stack(container);
    save_metadata();

    if (!quiet) {
        printf("[manager] %s stopped\n\n", container->id);
    }

    return 0;
}

static void print_container_banner(const Container *container, pid_t namespace_pid) {
    char pid_text[16];
    char limits_text[128];

    if (container->pid > 0) {
        snprintf(pid_text, sizeof(pid_text), "%d", (int)container->pid);
    } else {
        copy_string(pid_text, sizeof(pid_text), "-");
    }

    printf("\n");
    printf("  id       : %s\n", container->id);
    printf("  name     : %s\n", container->name);
    printf("  pid      : %s\n", pid_text);
    printf("  hostname : %s\n", container->hostname);
    printf("  rootfs   : %s\n", container->rootfs);
    printf("  command  : %s\n", container->command_line);
    resource_format_limits(&container->resource_limits, limits_text, sizeof(limits_text));
    printf("  limits   : %s\n", limits_text);
    printf("  isolate  : %s\n", namespace_profile());
    printf("  fs mode  : %s\n", filesystem_profile());
    printf("  res mode : %s\n", resource_profile());
    if (namespace_pid > 0) {
        printf("  ns pid   : %d\n", (int)namespace_pid);
    }
    printf("  state    : %s\n\n", state_to_string(container->state));
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
        char *fields[10];
        char *token = NULL;
        Container *container = NULL;
        int index = 0;

        trim_newline(line);
        token = strtok(line, "\t");
        while (token != NULL && index < 10) {
            fields[index++] = token;
            token = strtok(NULL, "\t");
        }

        if (index != 6 && index != 7 && index != 10) {
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
        if (index >= 7) {
            copy_string(container->command_line, sizeof(container->command_line), fields[6]);
        } else {
            copy_string(container->command_line, sizeof(container->command_line), DEFAULT_CONTAINER_COMMAND);
        }
        if (index >= 10) {
            container->resource_limits.cpu_seconds = (unsigned int)strtoul(fields[7], NULL, 10);
            container->resource_limits.memory_mb = (unsigned int)strtoul(fields[8], NULL, 10);
            container->resource_limits.max_processes = (unsigned int)strtoul(fields[9], NULL, 10);
        } else {
            memset(&container->resource_limits, 0, sizeof(container->resource_limits));
        }
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

    if (spec != NULL && spec->rootfs != NULL && spec->rootfs[0] != '\0') {
        copy_string(requested_rootfs, sizeof(requested_rootfs), spec->rootfs);
    } else {
        snprintf(requested_rootfs, sizeof(requested_rootfs), "./rootfs/%s", container->id);
    }

    if (spec != NULL && spec->command_line != NULL && spec->command_line[0] != '\0') {
        copy_string(container->command_line, sizeof(container->command_line), spec->command_line);
    } else {
        copy_string(container->command_line, sizeof(container->command_line), DEFAULT_CONTAINER_COMMAND);
    }

    memset(&container->resource_limits, 0, sizeof(container->resource_limits));
    if (spec != NULL) {
        container->resource_limits = spec->resource_limits;
        (void)normalize_resource_limits(&container->resource_limits);
    }

    if (filesystem_prepare_rootfs(requested_rootfs, container->rootfs, sizeof(container->rootfs)) != 0) {
        saved_errno = errno;
        printf("[error] failed to prepare rootfs for %s\n", container->id);
        printf("[hint] %s\n\n", strerror(saved_errno));
        free(container);
        return -1;
    }

    container->pid = -1;
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

static int start_container_by_id(const char *id, int schedule_target) {
    Container *container = NULL;
    NamespaceConfig namespace_config;
    NamespaceStartResult start_result;
    int saved_errno = 0;

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
    namespace_config.hostname = container->hostname;
    namespace_config.rootfs = container->rootfs;
    namespace_config.command_line = container->command_line;
    namespace_config.resource_limits = container->resource_limits;

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

    container->pid = start_result.host_pid;
    container->state = STATE_RUNNING;

    if (save_metadata() != 0) {
        stop_container_process(container, 1);
        return -1;
    }

    printf("[manager] started %s\n", container->id);
    print_container_banner(container, start_result.namespace_pid);
    log_event("%s STARTED (pid %d, ns_pid %d, isolation=%s, command=%s, cpu=%us mem=%uMB nproc=%u)",
              container->id,
              container->pid,
              start_result.namespace_pid,
              namespace_profile(),
              container->command_line,
              container->resource_limits.cpu_seconds,
              container->resource_limits.memory_mb,
              container->resource_limits.max_processes);

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

    if (container_create(spec, container_id, sizeof(container_id)) != 0) {
        return -1;
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

int container_stop(const char *id) {
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

    return stop_container_process(container, 0);
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

    remove_container(container);
    if (save_metadata() != 0) {
        append_container(container);
        return -1;
    }

    log_event("%s DELETED", container->id);
    printf("[manager] deleted %s\n\n", container->id);
    free_container_stack(container);
    free(container);
    return 0;
}

int container_list(void) {
    int count = 0;

    poll_states();

    printf("\n");
    printf("Isolation profile: %s\n", namespace_profile());
    printf("Filesystem profile: %s\n", filesystem_profile());
    printf("%-16s %-16s %-8s %-16s %-28s %-10s %-24s %-24s\n",
           "ID",
           "NAME",
           "PID",
           "HOSTNAME",
           "ROOTFS",
           "STATE",
           "COMMAND",
           "LIMITS");
    printf("-------------------------------------------------------------------------------------------------------------------------------------------------\n");

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        char pid_text[16];

        if (cursor->pid > 0) {
            snprintf(pid_text, sizeof(pid_text), "%d", (int)cursor->pid);
        } else {
            copy_string(pid_text, sizeof(pid_text), "-");
        }

        char limits_text[128];

        resource_format_limits(&cursor->resource_limits, limits_text, sizeof(limits_text));
        printf("%-16s %-16s %-8s %-16s %-28s %-10s %-24s %-24s\n",
               cursor->id,
               cursor->name,
               pid_text,
               cursor->hostname,
               cursor->rootfs,
               state_to_string(cursor->state),
               cursor->command_line,
               limits_text);
        count++;
    }

    if (count == 0) {
        printf("no containers found\n");
    }

    printf("\n");
    return 0;
}

void cleanup_all_containers(void) {
    Container *cursor = head;

    poll_states();

    while (cursor != NULL) {
        if (cursor->state == STATE_RUNNING) {
            stop_container_process(cursor, 1);
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
