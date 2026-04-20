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
    printf("  run [--cpu SEC] [--mem MB] [--pids N] <name> <hostname> <rootfs> <command> [args...]\n");
    printf("  runbg [--cpu SEC] [--mem MB] [--pids N] <name> <hostname> <rootfs> <command> [args...]\n");
    printf("  create [--cpu SEC] [--mem MB] [--pids N] [name] [hostname] [rootfs]\n");
    printf("  start <id>   (starts with namespaces and isolated rootfs)\n");
    printf("  stop <id>\n");
    printf("  delete <id>\n");
    printf("  list\n");
    printf("  help\n");
    printf("  exit\n\n");
}

static int parse_limit_flags(char **args, int argc, int *index, ResourceConfig *limits) {
    if (args == NULL || index == NULL || limits == NULL) {
        return -1;
    }

    while (*index < argc) {
        const char *flag = args[*index];

        if (flag == NULL || strncmp(flag, "--", 2) != 0) {
            break;
        }

        if (strcmp(flag, "--cpu") == 0) {
            if (*index + 1 >= argc) {
                return -1;
            }
            limits->cpu_seconds = (unsigned int)strtoul(args[*index + 1], NULL, 10);
            *index += 2;
            continue;
        }
        if (strcmp(flag, "--mem") == 0) {
            if (*index + 1 >= argc) {
                return -1;
            }
            limits->memory_mb = (unsigned int)strtoul(args[*index + 1], NULL, 10);
            *index += 2;
            continue;
        }
        if (strcmp(flag, "--pids") == 0) {
            if (*index + 1 >= argc) {
                return -1;
            }
            limits->max_processes = (unsigned int)strtoul(args[*index + 1], NULL, 10);
            *index += 2;
            continue;
        }

        return -1;
    }

    return 0;
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
            int index = 1;

            if (parse_limit_flags(args, argc, &index, &spec.resource_limits) != 0) {
                printf("[error] usage: %s [--cpu SEC] [--mem MB] [--pids N] <name> <hostname> <rootfs> <command> [args...]\n\n",
                       args[0]);
                continue;
            }

            if (argc - index < 4) {
                printf("[error] usage: %s [--cpu SEC] [--mem MB] [--pids N] <name> <hostname> <rootfs> <command> [args...]\n\n",
                       args[0]);
                continue;
            }

            spec.name = args[index + 0];
            spec.hostname = args[index + 1];
            spec.rootfs = args[index + 2];

            command_line[0] = '\0';
            for (int i = index + 3; i < argc; i++) {
                int written = snprintf(command_line + offset,
                                       sizeof(command_line) - offset,
                                       "%s%s",
                                       (i == index + 3) ? "" : " ",
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
            int index = 1;

            if (parse_limit_flags(args, argc, &index, &spec.resource_limits) != 0) {
                printf("[error] usage: create [--cpu SEC] [--mem MB] [--pids N] [name] [hostname] [rootfs]\n\n");
                continue;
            }

            if (argc - index > 3) {
                printf("[error] usage: create [--cpu SEC] [--mem MB] [--pids N] [name] [hostname] [rootfs]\n\n");
                continue;
            }

            spec.name = (argc - index >= 1) ? args[index + 0] : NULL;
            spec.hostname = (argc - index >= 2) ? args[index + 1] : NULL;
            spec.rootfs = (argc - index >= 3) ? args[index + 2] : NULL;
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
