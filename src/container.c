#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>

#include "container.h"
#include "namespace.h"
#include "filesystem.h"
#include "logger.h"

static Container containers[MAX_CONTAINERS];
static int container_count = 0;
static int next_id = 1;

/* check if any running containers have exited on their own */
static void poll_states(void) {
    for (int i = 0; i < container_count; i++) {
        if (containers[i].state == STATE_RUNNING) {
            if (waitpid(containers[i].pid, NULL, WNOHANG) > 0) {
                containers[i].state = STATE_STOPPED;
                strcpy(containers[i].status, "Stopped");
                log_event("container-%d exited (pid %d)",
                          containers[i].id, containers[i].pid);
            }
        }
    }
}

int container_create(void) {
    if (container_count >= MAX_CONTAINERS) {
        printf("max containers reached (%d)\n\n", MAX_CONTAINERS);
        return -1;
    }

    Container *c = &containers[container_count];
    c->id    = next_id++;
    c->state = STATE_CREATED;
    strcpy(c->status, "Created");

    /* build hostname and rootfs path for this container */
    snprintf(c->hostname, sizeof(c->hostname), "container-%d", c->id);
    if (create_rootfs(c->id, c->rootfs, sizeof(c->rootfs)) < 0) {
        printf("[error] failed to create rootfs\n\n");
        next_id--;
        return -1;
    }

    printf("\n[manager] creating %s ...\n", c->hostname);
    log_event("%s: CREATED", c->hostname);

    pid_t pid = create_pid_namespace(c->id, c->hostname, c->rootfs);
    if (pid < 0) {
        printf("[error] clone() failed — run with sudo\n\n");
        log_event("%s: clone() failed", c->hostname);
        next_id--;
        return -1;
    }

    c->pid   = pid;
    c->state = STATE_RUNNING;
    strcpy(c->status, "Running");
    container_count++;

    /* small delay so child prints its namespace info before prompt returns */
    usleep(150000);

    printf("\n[manager] %s is up\n", c->hostname);
    printf("          host pid : %d\n", pid);
    printf("          rootfs   : %s\n", c->rootfs);
    printf("          isolation: pid + uts + mount namespaces\n\n");
    log_event("%s: RUNNING (host pid %d)", c->hostname, pid);

    return c->id;
}

int container_list(void) {
    poll_states();

    printf("\n+----+--------------+----------+----------+\n");
    printf("| ID | Hostname     | Host PID | Status   |\n");
    printf("+----+--------------+----------+----------+\n");

    if (container_count == 0) {
        printf("|         no containers yet               |\n");
    } else {
        for (int i = 0; i < container_count; i++) {
            printf("| %-2d | %-12s | %-8d | %-8s |\n",
                   containers[i].id,
                   containers[i].hostname,
                   containers[i].pid,
                   containers[i].status);
        }
    }

    printf("+----+--------------+----------+----------+\n\n");
    return 0;
}

int container_stop(int id) {
    poll_states();

    for (int i = 0; i < container_count; i++) {
        if (containers[i].id != id) continue;

        if (containers[i].state != STATE_RUNNING) {
            printf("[manager] %s is not running (status: %s)\n\n",
                   containers[i].hostname, containers[i].status);
            return -1;
        }

        printf("[manager] stopping %s (pid %d)...\n",
               containers[i].hostname, containers[i].pid);

        kill(containers[i].pid, SIGKILL);
        waitpid(containers[i].pid, NULL, 0);

        containers[i].state = STATE_STOPPED;
        strcpy(containers[i].status, "Stopped");

        printf("[manager] %s stopped\n\n", containers[i].hostname);
        log_event("%s: STOPPED (pid %d killed)",
                  containers[i].hostname, containers[i].pid);
        return 0;
    }

    printf("[error] container %d not found\n\n", id);
    return -1;
}

void cleanup_all_containers(void) {
    for (int i = 0; i < container_count; i++) {
        if (containers[i].state == STATE_RUNNING) {
            kill(containers[i].pid, SIGKILL);
            waitpid(containers[i].pid, NULL, 0);
            log_event("%s: killed on shutdown", containers[i].hostname);
        }
    }
}
