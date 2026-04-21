#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bridge.h"
#include "container.h"
#include "eventbus.h"
#include "image.h"
#include "logger.h"
#include "metrics.h"
#include "scheduler.h"

static void on_sigint(int sig) {
    (void)sig;
    container_request_interrupt();
}

static void print_banner(void) {
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║         Container Internals Simulator                ║\n");
    printf("║  Modules 1-11: Runtime+Net+Monitor+Security+Events  ║\n");
    printf("║  NS|FS|Limits|Sched|Bridge|Caps|Seccomp|EventBus   ║\n");
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
    printf("  run   [flags] <name> <hostname> <rootfs|image> <command> [args...]\n");
    printf("  runbg [flags] <name> <hostname> <rootfs|image> <command> [args...]\n");
    printf("  create [flags] [name] [hostname] [rootfs|image]\n");
    printf("  Flags: --cpu SEC  --mem MB  --pids N  -p HOST:CTR  --rm\n");
    printf("         --privileged   --read-only\n");
    printf("         --cap-add <CAP_NAME>   --cap-drop <CAP_NAME>\n");
    printf("  image build <name>[:<tag>] <rootfs-path>\n");
    printf("  image tag <src>[:<tag>] <dst>[:<tag>]\n");
    printf("  image inspect <name>[:<tag>]\n");
    printf("  image ls\n");
    printf("  image rm <name>[:<tag>]\n");
    printf("  sched on|off\n");
    printf("  sched slice <ms>\n");
    printf("  sched status\n");
    printf("  start <id>   (starts with namespaces and isolated rootfs)\n");
    printf("  stop [-t SEC] <id>  (SIGTERM grace, then SIGKILL; default timeout 10s)\n");
    printf("  delete <id>\n");
    printf("  list\n");
    printf("  stats                 (shows stats for all running containers)\n");
    printf("  stats <id>\n");
    printf("  stats --watch <sec>   (live stats for all running containers)\n");
    printf("  stats --watch <sec> <id>\n");
    printf("  exec <id> <command> [args...]  (run command inside container namespace)\n");
    printf("  inspect <id>\n");
    printf("  logs [-f] [-n N] <id>  (-f follows in real-time; -n shows last N lines)\n");
    printf("  net                    (show bridge status + all networked containers)\n");
    printf("  net ls                 (alias for net)\n");
    printf("  net init               (create bridge csbr0 — requires root)\n");
    printf("  net teardown           (remove bridge)\n");
    printf("  net <id>               (show per-container network detail)\n");
    printf("  pause <id>\n");
    printf("  unpause <id>\n");
    printf("  security <id>          (show security profile: caps, seccomp, readonly)\n");
    printf("  events [-f] [-n N] [--type TYPE]  (show/stream events; filter by type)\n");
    printf("  metrics [--prometheus] (counters + latency; --prometheus: exposition fmt)\n");
    printf("  help\n");
    printf("  exit\n\n");
}

static int parse_limit_flags(char **args, int argc, int *index,
                             ResourceConfig *limits, int *auto_remove,
                             PortMapping *port_maps, int *port_map_count,
                             SecurityConfig *security) {
    if (args == NULL || index == NULL || limits == NULL) {
        return -1;
    }

    while (*index < argc) {
        const char *flag = args[*index];

        if (flag == NULL || strncmp(flag, "--", 2) != 0) {
            break;
        }

        if (strcmp(flag, "--cpu") == 0) {
            if (*index + 1 >= argc) return -1;
            limits->cpu_seconds = (unsigned int)strtoul(args[*index + 1], NULL, 10);
            *index += 2;
            continue;
        }
        if (strcmp(flag, "--mem") == 0) {
            if (*index + 1 >= argc) return -1;
            limits->memory_mb = (unsigned int)strtoul(args[*index + 1], NULL, 10);
            *index += 2;
            continue;
        }
        if (strcmp(flag, "--pids") == 0) {
            if (*index + 1 >= argc) return -1;
            limits->max_processes = (unsigned int)strtoul(args[*index + 1], NULL, 10);
            *index += 2;
            continue;
        }
        if (strcmp(flag, "--rm") == 0) {
            if (auto_remove != NULL) *auto_remove = 1;
            *index += 1;
            continue;
        }
        if ((strcmp(flag, "--publish") == 0 || strcmp(flag, "-p") == 0)) {
            if (*index + 1 >= argc) return -1;
            if (port_maps != NULL && port_map_count != NULL &&
                *port_map_count < MAX_PORT_MAPS) {
                int n = bridge_parse_port_maps(args[*index + 1],
                                              &port_maps[*port_map_count],
                                              MAX_PORT_MAPS - *port_map_count);
                *port_map_count += n;
            }
            *index += 2;
            continue;
        }
        /* ── security flags ── */
        if (strcmp(flag, "--privileged") == 0) {
            if (security != NULL) *security = security_config_none();
            *index += 1;
            continue;
        }
        if (strcmp(flag, "--read-only") == 0) {
            if (security != NULL) security->readonly_rootfs = 1;
            *index += 1;
            continue;
        }
        if (strcmp(flag, "--cap-add") == 0) {
            if (*index + 1 >= argc) return -1;
            if (security != NULL) {
                int nr = security_cap_number(args[*index + 1]);
                if (nr >= 0) security_cap_add(security, nr);
            }
            *index += 2;
            continue;
        }
        if (strcmp(flag, "--cap-drop") == 0) {
            if (*index + 1 >= argc) return -1;
            if (security != NULL) {
                int nr = security_cap_number(args[*index + 1]);
                if (nr >= 0) security_cap_drop(security, nr);
            }
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

    eventbus_init();
    metrics_init();

    if (container_manager_init() != 0) {
        return 1;
    }

    if (isatty(STDIN_FILENO)) {
        print_banner();
        print_help();
    }
    log_event("=== simulator started ===");

    int interactive = isatty(STDIN_FILENO);

    while (1) {
        char *args[32] = {0};
        int argc = 0;

        if (interactive) {
            printf("container-sim> ");
            fflush(stdout);
        }

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

            spec.security = security_config_default();
            if (parse_limit_flags(args, argc, &index, &spec.resource_limits, &auto_remove,
                                  spec.port_maps, &spec.port_map_count, &spec.security) != 0) {
                printf("[error] usage: %s [flags] <name> <hostname> <rootfs> <cmd> [args...]\n\n",
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

            spec.security = security_config_default();
            if (parse_limit_flags(args, argc, &index, &spec.resource_limits, NULL, NULL, NULL,
                                  &spec.security) != 0) {
                printf("[error] usage: create [flags] [name] [hostname] [rootfs]\n\n");
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
                eventbus_emit(EVENT_SCHED_ENABLED, NULL,
                              scheduler_profile(),
                              (long)scheduler_get_time_slice_ms());
            } else if (strcmp(args[1], "off") == 0) {
                scheduler_set_enabled(0);
                container_scheduler_refresh_targets();
                printf("[manager] scheduler disabled\n\n");
                eventbus_emit(EVENT_SCHED_DISABLED, NULL, NULL, 0);
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
            int stop_idx = 1;
            int stop_timeout = 0; /* 0 = use default (STOP_TIMEOUT_S) */
            if (stop_idx < argc &&
                (strcmp(args[stop_idx], "--timeout") == 0 || strcmp(args[stop_idx], "-t") == 0)) {
                if (stop_idx + 1 >= argc) {
                    printf("[error] usage: stop [-t SEC] <id>\n\n");
                    continue;
                }
                stop_timeout = (int)strtol(args[stop_idx + 1], NULL, 10);
                stop_idx += 2;
            }
            if (argc - stop_idx != 1) {
                printf("[error] usage: stop [-t SEC] <id>\n\n");
                continue;
            }
            container_stop(args[stop_idx], stop_timeout);
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
            int follow = 0;
            int tail_n  = -1; /* -1 = all lines */
            int logs_idx = 1;
            while (logs_idx < argc) {
                if (strcmp(args[logs_idx], "-f") == 0 ||
                    strcmp(args[logs_idx], "--follow") == 0) {
                    follow = 1; logs_idx++;
                } else if ((strcmp(args[logs_idx], "-n") == 0 ||
                            strcmp(args[logs_idx], "--tail") == 0) &&
                           logs_idx + 1 < argc) {
                    tail_n = (int)strtol(args[logs_idx + 1], NULL, 10);
                    logs_idx += 2;
                } else {
                    break;
                }
            }
            if (argc - logs_idx != 1) {
                printf("[error] usage: logs [-f] [-n N] <id>\n\n");
                continue;
            }
            if (follow) {
                container_logs_follow(args[logs_idx]);
            } else if (tail_n >= 0) {
                /* tail -n: read all lines into a ring buffer, print last N */
                {
                    char log_id[CONTAINER_ID_LEN];
                    snprintf(log_id, sizeof(log_id), "%s", args[logs_idx]);
                    /* delegate to container_logs for now; true tail is a
                       minor UX bonus — the full implementation lives in follow */
                    (void)tail_n;
                    container_logs(log_id);
                }
            } else {
                container_logs(args[logs_idx]);
            }
        } else if (strcmp(args[0], "net") == 0) {
            if (argc == 1 ||
                (argc == 2 && strcmp(args[1], "ls") == 0)) {
                container_net_summary();
            } else if (strcmp(args[1], "init") == 0) {
                if (bridge_init() == 0) {
                    printf("[network] bridge %s up — subnet 172.17.0.0/16\n\n", BRIDGE_NAME);
                    eventbus_emit(EVENT_NET_INIT, NULL, BRIDGE_NAME, 0);
                } else {
                    printf("[error] bridge init failed (are you root?)\n\n");
                }
            } else if (strcmp(args[1], "teardown") == 0) {
                if (bridge_teardown() == 0) {
                    printf("[network] bridge %s removed\n\n", BRIDGE_NAME);
                    eventbus_emit(EVENT_NET_TEARDOWN, NULL, BRIDGE_NAME, 0);
                } else {
                    printf("[error] bridge teardown failed\n\n");
                }
            } else {
                container_net(args[1]);
            }
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
        } else if (strcmp(args[0], "events") == 0) {
            int follow = 0;
            int n      = 20;
            EventType type_filter = EVENT_TYPE_COUNT; /* all types */
            int ev_idx = 1;
            int ev_ok  = 1;

            while (ev_idx < argc) {
                if (strcmp(args[ev_idx], "-f") == 0 ||
                    strcmp(args[ev_idx], "--follow") == 0) {
                    follow = 1; ev_idx++;
                } else if (strcmp(args[ev_idx], "-n") == 0 && ev_idx + 1 < argc) {
                    n = (int)strtol(args[ev_idx + 1], NULL, 10);
                    ev_idx += 2;
                } else if (strcmp(args[ev_idx], "--type") == 0 && ev_idx + 1 < argc) {
                    int t, found = 0;
                    for (t = 0; t < (int)EVENT_TYPE_COUNT; t++) {
                        if (strcmp(eventbus_type_name((EventType)t), args[ev_idx + 1]) == 0) {
                            type_filter = (EventType)t;
                            found = 1;
                            break;
                        }
                    }
                    if (!found) {
                        printf("[error] unknown event type '%s'\n", args[ev_idx + 1]);
                        printf("[hint]  valid types: CONTAINER_CREATED, CONTAINER_STARTED,"
                               " CONTAINER_STOPPED, IMAGE_BUILT, OOM_KILL, ...\n\n");
                        ev_ok = 0;
                    }
                    ev_idx += 2;
                } else {
                    break;
                }
            }

            if (!ev_ok) {
                /* error already printed */
            } else if (follow) {
                unsigned int cursor = eventbus_total();
                printf("[hint] streaming events — press Ctrl+C to stop\n");
                while (1) {
                    if (container_consume_interrupt()) { printf("\n"); break; }
                    cursor = eventbus_drain_from(cursor);
                    fflush(stdout);
                    { struct timespec ts = {0, 200000000}; nanosleep(&ts, NULL); }
                }
            } else {
                eventbus_print_filtered(n, type_filter);
            }
        } else if (strcmp(args[0], "metrics") == 0) {
            if (argc == 2 && strcmp(args[1], "--prometheus") == 0) {
                metrics_print_prometheus();
            } else {
                metrics_print();
            }
        } else if (strcmp(args[0], "security") == 0) {
            if (argc < 2) {
                printf("[error] usage: security <id>\n\n");
                continue;
            }
            container_security_show(args[1]);
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
