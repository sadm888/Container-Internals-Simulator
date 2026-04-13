#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "container.h"
#include "filesystem.h"
#include "logger.h"
#include "namespace.h"

#define STACK_SIZE (1024 * 1024)
#define METADATA_FILE "containers.meta"
#define METADATA_FILE_TMP "containers.meta.tmp"

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
        fprintf(file, "%s\t%s\t%d\t%s\t%s\t%s\n",
                cursor->id,
                cursor->name,
                (int)cursor->pid,
                cursor->hostname,
                cursor->rootfs,
                state_to_string(cursor->state));
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

static int sync_container_state(Container *container) {
    int state_changed = 0;
    int status = 0;
    pid_t result = 0;

    if (container->state != STATE_RUNNING || container->pid <= 0) {
        return 0;
    }

    result = waitpid(container->pid, &status, WNOHANG);
    if (result > 0) {
        container->state = STATE_STOPPED;
        container->pid = -1;
        free_container_stack(container);
        log_event("%s exited and was reaped by manager", container->id);
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
    printf("  isolate  : %s\n", namespace_profile());
    printf("  fs mode  : %s\n", filesystem_profile());
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

int container_manager_init(void) {
    FILE *file = fopen(METADATA_FILE, "r");
    char line[1024];
    int restored = 0;

    if (file == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *fields[6];
        char *token = NULL;
        Container *container = NULL;
        int index = 0;

        trim_newline(line);
        token = strtok(line, "\t");
        while (token != NULL && index < 6) {
            fields[index++] = token;
            token = strtok(NULL, "\t");
        }

        if (index != 6) {
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
    log_event("%s CREATED (name=%s hostname=%s rootfs=%s)",
              container->id,
              container->name,
              container->hostname,
              container->rootfs);
    return 0;
}

int container_start(const char *id) {
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

    if (namespace_start_container(&namespace_config,
                                  container->stack,
                                  STACK_SIZE,
                                  &start_result) != 0) {
        saved_errno = errno;
        print_start_error(container, saved_errno);
        log_event("startup isolation failed for %s: %s", container->id, strerror(saved_errno));
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
    log_event("%s STARTED (pid %d, ns_pid %d, isolation=%s)",
              container->id,
              container->pid,
              start_result.namespace_pid,
              namespace_profile());
    return 0;
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
    printf("%-16s %-16s %-8s %-16s %-28s %-10s\n",
           "ID",
           "NAME",
           "PID",
           "HOSTNAME",
           "ROOTFS",
           "STATE");
    printf("-----------------------------------------------------------------------------------------------\n");

    for (Container *cursor = head; cursor != NULL; cursor = cursor->next) {
        char pid_text[16];

        if (cursor->pid > 0) {
            snprintf(pid_text, sizeof(pid_text), "%d", (int)cursor->pid);
        } else {
            copy_string(pid_text, sizeof(pid_text), "-");
        }

        printf("%-16s %-16s %-8s %-16s %-28s %-10s\n",
               cursor->id,
               cursor->name,
               pid_text,
               cursor->hostname,
               cursor->rootfs,
               state_to_string(cursor->state));
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
}
