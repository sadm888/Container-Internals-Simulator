#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "container.h"
#include "image.h"
#include "logger.h"
#include "scheduler.h"

static void on_sigint(int sig) {
    (void)sig;
    container_request_interrupt();
}

static void print_banner(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║         Container Internals Simulator                ║\n");
    printf("║         Modules 1-8: Runtime + Monitoring            ║\n");
    printf("║         NS | FS | Limits | Sched | Netns | Stats     ║\n");
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
    printf("  run [--cpu SEC] [--mem MB] [--pids N] [--rm] <name> <hostname> <rootfs|image> <command> [args...]\n");
    printf("  runbg [--cpu SEC] [--mem MB] [--pids N] <name> <hostname> <rootfs|image> <command> [args...]\n");
    printf("  create [--cpu SEC] [--mem MB] [--pids N] [name] [hostname] [rootfs|image]\n");
    printf("  image build <name>[:<tag>] <rootfs-path>\n");
    printf("  image tag <src>[:<tag>] <dst>[:<tag>]\n");
    printf("  image inspect <name>[:<tag>]\n");
    printf("  image ls\n");
    printf("  image rm <name>[:<tag>]\n");
    printf("  sched on|off\n");
    printf("  sched slice <ms>\n");
    printf("  sched status\n");
    printf("  start <id>   (starts with namespaces and isolated rootfs)\n");
    printf("  stop <id>\n");
    printf("  delete <id>\n");
    printf("  list\n");
    printf("  stats                 (shows stats for all running containers)\n");
    printf("  stats <id>\n");
    printf("  stats --watch <sec>   (live stats for all running containers)\n");
    printf("  stats --watch <sec> <id>\n");
    printf("  exec <id> <command> [args...]  (run command inside container namespace)\n");
    printf("  inspect <id>\n");
    printf("  logs <id>\n");
    printf("  net <id>\n");
    printf("  pause <id>\n");
    printf("  unpause <id>\n");
    printf("  help\n");
    printf("  exit\n\n");
}

static int parse_limit_flags(char **args, int argc, int *index, ResourceConfig *limits, int *auto_remove) {
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
        if (strcmp(flag, "--rm") == 0) {
            if (auto_remove != NULL) {
                *auto_remove = 1;
            }
            *index += 1;
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
    log_event("=== simulator started ===");

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
            int auto_remove = 0;
            size_t offset = 0;
            int index = 1;

            if (parse_limit_flags(args, argc, &index, &spec.resource_limits, &auto_remove) != 0) {
                printf("[error] usage: %s [--cpu SEC] [--mem MB] [--pids N] [--rm] <name> <hostname> <rootfs> <command> [args...]\n\n",
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
                    if (auto_remove) {
                        container_delete(container_id);
                    }
                }
            }
        } else if (strcmp(args[0], "create") == 0) {
            ContainerSpec spec = {0};
            char container_id[CONTAINER_ID_LEN];
            int index = 1;

            if (parse_limit_flags(args, argc, &index, &spec.resource_limits, NULL) != 0) {
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
            } else if (strcmp(args[1], "off") == 0) {
                scheduler_set_enabled(0);
                container_scheduler_refresh_targets();
                printf("[manager] scheduler disabled\n\n");
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
        } else if (strcmp(args[0], "exec") == 0) {
            if (argc < 3) {
                printf("[error] usage: exec <id> <command> [args...]\n\n");
                continue;
            }
            {
                char exec_cmd[256] = {0};
                size_t off = 0;
                for (int i = 2; i < argc; i++) {
                    int w = snprintf(exec_cmd + off, sizeof(exec_cmd) - off,
                                     "%s%s", (i == 2) ? "" : " ", args[i]);
                    if (w < 0 || (size_t)w >= sizeof(exec_cmd) - off) break;
                    off += (size_t)w;
                }
                container_exec(args[1], exec_cmd);
            }
        } else if (strcmp(args[0], "inspect") == 0) {
            if (argc != 2) {
                printf("[error] usage: inspect <id>\n\n");
                continue;
            }
            container_inspect(args[1]);
        } else if (strcmp(args[0], "logs") == 0) {
            if (argc != 2) {
                printf("[error] usage: logs <id>\n\n");
                continue;
            }
            container_logs(args[1]);
        } else if (strcmp(args[0], "net") == 0) {
            if (argc != 2) {
                printf("[error] usage: net <id>\n\n");
                continue;
            }
            container_net(args[1]);
        } else if (strcmp(args[0], "image") == 0) {
            if (argc < 2) {
                printf("[error] usage: image build|ls|rm\n\n");
                continue;
            }
            if (strcmp(args[1], "build") == 0) {
                if (argc < 4) {
                    printf("[error] usage: image build <name>[:<tag>] <rootfs-path>\n\n");
                    continue;
                }
                {
                    char img_name[64];
                    char img_tag[32];
                    const char *colon = strchr(args[2], ':');

                    if (colon != NULL) {
                        int nlen = (int)(colon - args[2]);
                        if (nlen >= (int)sizeof(img_name)) nlen = (int)sizeof(img_name) - 1;
                        memcpy(img_name, args[2], (size_t)nlen);
                        img_name[nlen] = '\0';
                        snprintf(img_tag, sizeof(img_tag), "%s", colon + 1);
                    } else {
                        snprintf(img_name, sizeof(img_name), "%s", args[2]);
                        img_tag[0] = '\0';
                    }
                    image_build(img_name, img_tag, args[3]);
                }
            } else if (strcmp(args[1], "tag") == 0) {
                if (argc < 4) {
                    printf("[error] usage: image tag <src>[:<tag>] <dst>[:<tag>]\n\n");
                    continue;
                }
                image_tag(args[2], args[3]);
            } else if (strcmp(args[1], "inspect") == 0) {
                if (argc < 3) {
                    printf("[error] usage: image inspect <name>[:<tag>]\n\n");
                    continue;
                }
                image_inspect(args[2]);
            } else if (strcmp(args[1], "ls") == 0) {
                image_list();
            } else if (strcmp(args[1], "rm") == 0) {
                if (argc < 3) {
                    printf("[error] usage: image rm <name>[:<tag>]\n\n");
                    continue;
                }
                {
                    char img_name[64];
                    char img_tag[32];
                    const char *colon = strchr(args[2], ':');

                    if (colon != NULL) {
                        int nlen = (int)(colon - args[2]);
                        if (nlen >= (int)sizeof(img_name)) nlen = (int)sizeof(img_name) - 1;
                        memcpy(img_name, args[2], (size_t)nlen);
                        img_name[nlen] = '\0';
                        snprintf(img_tag, sizeof(img_tag), "%s", colon + 1);
                    } else {
                        snprintf(img_name, sizeof(img_name), "%s", args[2]);
                        img_tag[0] = '\0';
                    }
                    image_remove(img_name, img_tag);
                }
            } else {
                printf("[error] unknown image subcommand: %s\n\n", args[1]);
            }
        } else if (strcmp(args[0], "pause") == 0) {
            if (argc != 2) {
                printf("[error] usage: pause <id>\n\n");
                continue;
            }
            container_pause(args[1]);
        } else if (strcmp(args[0], "unpause") == 0) {
            if (argc != 2) {
                printf("[error] usage: unpause <id>\n\n");
                continue;
            }
            container_unpause(args[1]);
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
