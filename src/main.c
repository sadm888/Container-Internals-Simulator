#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "container.h"
#include "logger.h"

static void print_banner(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║         Container Internals Simulator                ║\n");
    printf("║         Module 4: Filesystem Isolation               ║\n");
    printf("║         Namespaces + pivot_root + /proc              ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
}

static int parse_command(char *line, char **args, int max_args) {
    int count = 0;
    char *token = strtok(line, " ");

    while (token != NULL && count < max_args) {
        args[count++] = token;
        token = strtok(NULL, " ");
    }

    return count;
}

static void print_help(void) {
    printf("Commands:\n");
    printf("  run <name> <hostname> <rootfs> <command> [args...]\n");
    printf("  runbg <name> <hostname> <rootfs> <command> [args...]\n");
    printf("  create [name] [hostname] [rootfs]\n");
    printf("  start <id>   (starts with namespaces and isolated rootfs)\n");
    printf("  stop <id>\n");
    printf("  delete <id>\n");
    printf("  list\n");
    printf("  help\n");
    printf("  exit\n\n");
}

int main(void) {
    char line[256];

    if (container_manager_init() != 0) {
        return 1;
    }

    print_banner();
    print_help();
    log_event("=== simulator started ===");

    while (1) {
        char *args[32] = {0};
        int argc = 0;

        printf("container-sim> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            printf("\n");
            cleanup_all_containers();
            log_event("=== simulator stopped ===");
            break;
        }

        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') {
            continue;
        }

        argc = parse_command(line, args, (int)(sizeof(args) / sizeof(args[0])));
        if (argc == 0) {
            continue;
        }

        if (strcmp(args[0], "run") == 0 || strcmp(args[0], "runbg") == 0) {
            ContainerSpec spec = {0};
            char command_line[CONTAINER_COMMAND_LEN];
            char container_id[CONTAINER_ID_LEN];
            int background = (strcmp(args[0], "runbg") == 0);
            size_t offset = 0;

            if (argc < 5) {
                printf("[error] usage: %s <name> <hostname> <rootfs> <command> [args...]\n\n", args[0]);
                continue;
            }

            spec.name = args[1];
            spec.hostname = args[2];
            spec.rootfs = args[3];

            command_line[0] = '\0';
            for (int i = 4; i < argc; i++) {
                int written = snprintf(command_line + offset,
                                       sizeof(command_line) - offset,
                                       "%s%s",
                                       (i == 4) ? "" : " ",
                                       args[i]);
                if (written < 0 || (size_t)written >= sizeof(command_line) - offset) {
                    offset = sizeof(command_line);
                    break;
                }
                offset += (size_t)written;
            }

            if (offset >= sizeof(command_line)) {
                printf("[error] command is too long\n\n");
                continue;
            }

            spec.command_line = command_line;

            if ((!background && container_run(&spec, container_id, sizeof(container_id)) == 0) ||
                (background && container_run_background(&spec, container_id, sizeof(container_id)) == 0)) {
                if (background) {
                    printf("[hint] runtime workload running in background as %s\n\n", container_id);
                } else {
                    printf("[hint] runtime workload completed in %s\n\n", container_id);
                }
            }
        } else if (strcmp(args[0], "create") == 0) {
            ContainerSpec spec = {0};
            char container_id[CONTAINER_ID_LEN];

            if (argc > 4) {
                printf("[error] usage: create [name] [hostname] [rootfs]\n\n");
                continue;
            }

            spec.name = (argc >= 2) ? args[1] : NULL;
            spec.hostname = (argc >= 3) ? args[2] : NULL;
            spec.rootfs = (argc >= 4) ? args[3] : NULL;
            spec.command_line = NULL;

            if (container_create(&spec, container_id, sizeof(container_id)) == 0) {
                printf("[hint] start it with: start %s\n\n", container_id);
            }
        } else if (strcmp(args[0], "start") == 0) {
            if (argc != 2) {
                printf("[error] usage: start <id>\n\n");
                continue;
            }
            container_start(args[1]);
        } else if (strcmp(args[0], "stop") == 0) {
            if (argc != 2) {
                printf("[error] usage: stop <id>\n\n");
                continue;
            }
            container_stop(args[1]);
        } else if (strcmp(args[0], "delete") == 0) {
            if (argc != 2) {
                printf("[error] usage: delete <id>\n\n");
                continue;
            }
            container_delete(args[1]);
        } else if (strcmp(args[0], "list") == 0) {
            if (argc != 1) {
                printf("[error] usage: list\n\n");
                continue;
            }
            container_list();
        } else if (strcmp(args[0], "help") == 0) {
            print_help();
        } else if (strcmp(args[0], "exit") == 0) {
            cleanup_all_containers();
            log_event("=== simulator stopped ===");
            printf("bye.\n");
            break;
        } else {
            printf("[error] unknown command: '%s'\n\n", args[0]);
        }
    }

    return 0;
}
