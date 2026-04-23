#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "container.h"
#include "logger.h"
#include "scheduler.h"

static void on_sigint(int sig) {
    (void)sig;
    container_request_interrupt();
}

static void print_rule(void) {
    printf("======================================================================\n");
}

static void print_section(const char *title) {
    print_rule();
    printf("%s\n", title);
    print_rule();
}

static void print_banner(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║         Container Internals Simulator                ║\n");
    printf("║         Runtime + Isolation + Monitoring             ║\n");
    printf("║         NS | FS | Limits | Sched | Netns | Logs      ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");
    printf("\n");
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
    print_section("Command Reference");
    printf("Lifecycle\n");
    printf("  run   [--cpu SEC] [--mem MB] [--pids N] <name> <hostname> <rootfs> <command> [args...]\n");
    printf("  runbg [--cpu SEC] [--mem MB] [--pids N] <name> <hostname> <rootfs> <command> [args...]\n");
    printf("  create [--cpu SEC] [--mem MB] [--pids N] [name] [hostname] [rootfs]\n");
    printf("  start <id>    start a created container with namespaces and isolated rootfs\n");
    printf("  stop <id>     stop a running container\n");
    printf("  delete <id>   delete a stopped container record\n");
    printf("  list          show all container records\n");
    printf("\n");
    printf("Scheduling\n");
    printf("  sched on\n");
    printf("  sched off\n");
    printf("  sched slice <ms>\n");
    printf("  sched status\n");
    printf("\n");
    printf("Observability\n");
    printf("  logs [-f] [-n N] [id]           view overall or container-specific logs\n");
    printf("  stats                           show stats for all running containers\n");
    printf("  stats <id>                      show stats for one container\n");
    printf("  stats --watch <sec>             live stats for all running containers\n");
    printf("  stats --watch <sec> <id>        live stats for one container\n");
    printf("\n");
    printf("General\n");
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

    (void)signal(SIGINT, on_sigint);

    if (container_manager_init() != 0) {
        return 1;
    }

    print_banner();
    print_help();
    log_event_type("SIMULATOR_STARTED", "interactive CLI started");

    while (1) {
        char *args[32] = {0};
        int argc = 0;

        printf("container-sim> ");
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            if (errno == EINTR || container_consume_interrupt()) {
                clearerr(stdin);
                printf("\n");
                continue;
            }
            printf("\n");
            cleanup_all_containers();
            log_event_type("SIMULATOR_STOPPED", "CLI stopped after input closed");
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
        } else if (strcmp(args[0], "sched") == 0) {
            if (argc < 2) {
                printf("[error] usage: sched on|off|slice <ms>|status\n\n");
                continue;
            }

            if (strcmp(args[1], "on") == 0) {
                SchedulerConfig config = {0};

                config.time_slice_ms = scheduler_get_time_slice_ms();
                if (config.time_slice_ms == 0) {
                    config.time_slice_ms = 200;
                }

                if (scheduler_start(&config) != 0) {
                    printf("[error] failed to start scheduler\n\n");
                    continue;
                }
                scheduler_set_enabled(1);
                container_scheduler_refresh_targets();
                printf("[manager] scheduler enabled (%s, slice=%ums)\n\n",
                       scheduler_profile(),
                       scheduler_get_time_slice_ms());
                log_event_type("SCHEDULER_ENABLED",
                               "profile=%s slice=%ums",
                               scheduler_profile(),
                               scheduler_get_time_slice_ms());
            } else if (strcmp(args[1], "off") == 0) {
                scheduler_set_enabled(0);
                container_scheduler_refresh_targets();
                printf("[manager] scheduler disabled\n\n");
                log_event_type("SCHEDULER_DISABLED", "scheduler disabled");
            } else if (strcmp(args[1], "slice") == 0) {
                unsigned int ms = 0;
                if (argc != 3) {
                    printf("[error] usage: sched slice <ms>\n\n");
                    continue;
                }
                ms = (unsigned int)strtoul(args[2], NULL, 10);
                if (scheduler_set_time_slice_ms(ms) != 0) {
                    printf("[error] invalid time slice\n\n");
                    continue;
                }
                printf("[manager] scheduler slice set to %ums\n\n", scheduler_get_time_slice_ms());
                log_event_type("SCHEDULER_UPDATED", "time_slice_ms=%u", scheduler_get_time_slice_ms());
            } else if (strcmp(args[1], "status") == 0) {
                printf("[manager] scheduler: %s, slice=%ums, mode=%s\n\n",
                       scheduler_profile(),
                       scheduler_get_time_slice_ms(),
                       scheduler_is_enabled() ? "enabled" : "disabled");
            } else {
                printf("[error] usage: sched on|off|slice <ms>|status\n\n");
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
        } else if (strcmp(args[0], "logs") == 0) {
            const char *container_id = NULL;
            int follow = 0;
            int tail_lines = -1;
            int index = 1;

            while (index < argc) {
                if (strcmp(args[index], "-f") == 0 || strcmp(args[index], "--follow") == 0) {
                    follow = 1;
                    index++;
                    continue;
                }
                if ((strcmp(args[index], "-n") == 0 || strcmp(args[index], "--tail") == 0) && index + 1 < argc) {
                    tail_lines = (int)strtol(args[index + 1], NULL, 10);
                    index += 2;
                    continue;
                }
                if (container_id == NULL) {
                    container_id = args[index];
                    index++;
                    continue;
                }
                break;
            }

            if (index != argc) {
                printf("[error] usage: logs [-f] [-n N] [id]\n\n");
                continue;
            }

            if (follow) {
                logger_follow(container_id, tail_lines);
            } else if (tail_lines >= 0) {
                logger_tail(container_id, tail_lines);
            } else {
                logger_print(container_id);
            }
        } else if (strcmp(args[0], "stats") == 0) {
            if (argc == 1) {
                container_stats_all();
            } else if (argc == 2) {
                container_stats(args[1]);
            } else if (argc == 3 && strcmp(args[1], "--watch") == 0) {
                unsigned int sec = (unsigned int)strtoul(args[2], NULL, 10);
                container_stats_all_watch(sec);
            } else if (argc == 4 && strcmp(args[1], "--watch") == 0) {
                unsigned int sec = (unsigned int)strtoul(args[2], NULL, 10);
                container_stats_watch(args[3], sec);
            } else {
                printf("[error] usage: stats [id] | stats --watch <sec> [id]\n\n");
            }
        } else if (strcmp(args[0], "help") == 0) {
            print_help();
        } else if (strcmp(args[0], "exit") == 0) {
            cleanup_all_containers();
            log_event_type("SIMULATOR_STOPPED", "CLI stopped by user command");
            printf("bye.\n");
            break;
        } else {
            printf("[error] unknown command: '%s'\n\n", args[0]);
            log_event_type("ERROR", "unknown command '%s'", args[0]);
        }
    }

    return 0;
}
