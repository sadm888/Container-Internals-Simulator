/*
 * main.c — Container Internals Simulator
 *
 * Interactive CLI (REPL) for managing simulated containers.
 *
 * Commands:
 *   create       — create + start a new container (uses clone/PID namespace)
 *   list         — list all containers with host PID and status
 *   stop <id>    — stop a running container
 *   help         — show command reference
 *   exit / quit  — clean up and exit
 *
 * Must be run as root for clone(CLONE_NEWPID) to succeed:
 *   sudo ./container-sim
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "container.h"
#include "logger.h"

static void print_banner(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║       Container Internals Simulator  v1.0            ║\n");
    printf("║       Linux Primitives  —  No Docker                 ║\n");
    printf("║       Syscalls: clone()  setrlimit()  chroot()       ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

static void print_help(void) {
    printf("Commands:\n");
    printf("  create       — Create and start a new container\n");
    printf("  list         — List all containers and status\n");
    printf("  stop <id>    — Stop a running container  (e.g. stop 1)\n");
    printf("  help         — Show this help\n");
    printf("  exit         — Shutdown simulator\n\n");
}

int main(void) {
    if (geteuid() != 0) {
        printf("[Warning] Not running as root.\n");
        printf("          PID namespace creation requires root privileges.\n");
        printf("          Run: sudo ./container-sim\n\n");
    }

    print_banner();
    print_help();

    log_event("=== Container Simulator started (simulator PID %d) ===",
              getpid());

    char line[256];

    while (1) {
        printf("container-sim> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            /* EOF (Ctrl-D) — treat like exit */
            printf("\n");
            break;
        }

        /* Strip trailing newline */
        line[strcspn(line, "\n")] = '\0';

        /* ── Command dispatch ─────────────────────────────── */

        if (strcmp(line, "create") == 0) {
            container_create();

        } else if (strcmp(line, "list") == 0) {
            container_list();

        } else if (strncmp(line, "stop ", 5) == 0) {
            int id = atoi(line + 5);
            if (id <= 0) {
                printf("[Error] Usage: stop <id>  (e.g., stop 1)\n\n");
            } else {
                container_stop(id);
            }

        } else if (strcmp(line, "help") == 0) {
            print_help();

        } else if (strcmp(line, "exit") == 0 ||
                   strcmp(line, "quit") == 0) {
            printf("[Simulator] Cleaning up all running containers...\n");
            cleanup_all_containers();
            printf("[Simulator] Shutdown complete.\n");
            log_event("=== Container Simulator stopped ===");
            break;

        } else if (line[0] != '\0') {
            /* Non-empty unrecognised input */
            printf("[Error] Unknown command: '%s'\n", line);
            printf("        Type 'help' for available commands.\n\n");
        }
    }

    return 0;
}
