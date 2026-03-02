#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>

#include "container.h"
#include "namespace.h"
#include "logger.h"

static Container containers[MAX_CONTAINERS];
static int       container_count = 0;
static int       next_id         = 1;

/* Poll all running containers to see if any have exited on their own */
static void update_states(void) {
    for (int i = 0; i < container_count; i++) {
        if (containers[i].state == STATE_RUNNING) {
            pid_t result = waitpid(containers[i].pid, NULL, WNOHANG);
            if (result > 0) {
                containers[i].state = STATE_STOPPED;
                strcpy(containers[i].status, "Stopped");
                log_event("Container %d: exited (host PID %d)",
                          containers[i].id, containers[i].pid);
            }
        }
    }
}

int container_create(void) {
    if (container_count >= MAX_CONTAINERS) {
        printf("[Error] Maximum container limit (%d) reached.\n\n", MAX_CONTAINERS);
        return -1;
    }

    Container *c = &containers[container_count];
    c->id    = next_id++;
    c->state = STATE_CREATED;
    strcpy(c->status, "Created");

    printf("\n[Manager] Creating container %d ...\n", c->id);
    log_event("Container %d: CREATED", c->id);

    /*
     * create_pid_namespace() calls clone(CLONE_NEWPID | SIGCHLD).
     * The child process starts in a fresh PID namespace where it
     * sees itself as PID 1.  The host (parent) receives the real OS PID.
     */
    pid_t pid = create_pid_namespace(c->id);
    if (pid < 0) {
        printf("[Error] clone() failed.\n");
        printf("        Namespace creation requires root: sudo ./container-sim\n\n");
        log_event("Container %d: clone() FAILED", c->id);
        next_id--;
        return -1;
    }

    c->pid   = pid;
    c->state = STATE_RUNNING;
    strcpy(c->status, "Running");
    container_count++;

    /* Let the child print its namespace view before the prompt returns */
    usleep(120000);

    printf("\n[Manager] Container %d is RUNNING\n", c->id);
    printf("[Manager]   Host PID  : %d  (real kernel PID)\n", pid);
    printf("[Manager]   Namespace : PID — child reported PID 1 internally\n");
    printf("[Manager]   Isolation : ACTIVE\n\n");
    log_event("Container %d: RUNNING (host PID %d)", c->id, pid);

    return c->id;
}

int container_list(void) {
    update_states();

    printf("\n+----+----------+----------+\n");
    printf("| ID | Host PID | Status   |\n");
    printf("+----+----------+----------+\n");

    if (container_count == 0) {
        printf("|    No containers yet.    |\n");
    } else {
        for (int i = 0; i < container_count; i++) {
            printf("| %-2d | %-8d | %-8s |\n",
                   containers[i].id,
                   containers[i].pid,
                   containers[i].status);
        }
    }

    printf("+----+----------+----------+\n\n");
    return 0;
}

int container_stop(int id) {
    update_states();

    for (int i = 0; i < container_count; i++) {
        if (containers[i].id == id) {
            if (containers[i].state != STATE_RUNNING) {
                printf("[Manager] Container %d is not running (status: %s)\n\n",
                       id, containers[i].status);
                return -1;
            }

            printf("[Manager] Stopping container %d (host PID: %d)...\n",
                   id, containers[i].pid);

            kill(containers[i].pid, SIGKILL);
            waitpid(containers[i].pid, NULL, 0);

            containers[i].state = STATE_STOPPED;
            strcpy(containers[i].status, "Stopped");

            printf("[Manager] Container %d stopped.\n\n", id);
            log_event("Container %d: STOPPED by user (host PID %d killed)",
                      id, containers[i].pid);
            return 0;
        }
    }

    printf("[Error] Container %d not found.\n\n", id);
    return -1;
}

/* Called on simulator exit — cleans up any still-running containers */
void cleanup_all_containers(void) {
    for (int i = 0; i < container_count; i++) {
        if (containers[i].state == STATE_RUNNING) {
            kill(containers[i].pid, SIGKILL);
            waitpid(containers[i].pid, NULL, 0);
            log_event("Container %d: KILLED on simulator shutdown", containers[i].id);
        }
    }
}
