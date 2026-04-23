#ifndef CONTAINER_H
#define CONTAINER_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#include "bridge.h"
#include "image.h"
#include "resource.h"
#include "security.h"

#define CONTAINER_ID_LEN 64
#define CONTAINER_NAME_LEN 64
#define CONTAINER_HOSTNAME_LEN 64
#define CONTAINER_ROOTFS_LEN 512
#define CONTAINER_COMMAND_LEN 256
#define CONTAINER_LOG_PATH_LEN 256

typedef enum {
    STATE_CREATED,
    STATE_RUNNING,
    STATE_PAUSED,
    STATE_STOPPED
} ContainerState;

typedef struct {
    const char   *name;
    const char   *hostname;
    const char   *rootfs;
    const char   *command_line;
    ResourceConfig resource_limits;
    SecurityConfig security;
    PortMapping    port_maps[MAX_PORT_MAPS];
    int            port_map_count;
} ContainerSpec;

typedef struct Container {
    char              id[CONTAINER_ID_LEN];
    char              name[CONTAINER_NAME_LEN];
    pid_t             pid;
    char              hostname[CONTAINER_HOSTNAME_LEN];
    char              rootfs[CONTAINER_ROOTFS_LEN];
    char              command_line[CONTAINER_COMMAND_LEN];
    char              log_path[CONTAINER_LOG_PATH_LEN];
    char              image_ref[IMAGE_REF_LEN]; /* "name:tag" or "" if not from image */
    ResourceConfig    resource_limits;
    char              ip_address[16];           /* "" or "172.17.0.N" if bridge active */
    char              veth_host[16];            /* "" or "vethXXXX" host-side interface */
    char              network_error[256];       /* startup networking diagnosis, not persisted */
    PortMapping       port_maps[MAX_PORT_MAPS];
    int               port_map_count;
    SecurityConfig    security;
    ContainerState    state;
    time_t            started_at;
    time_t            stopped_at;
    int               exit_code;
    long              oom_kill_count; /* runtime-only; not persisted */
    char             *stack;
    struct Container *next;
} Container;

int  container_manager_init(void);
int  container_create(const ContainerSpec *spec, char *out_id, size_t out_id_size);
int  container_run(const ContainerSpec *spec, char *out_id, size_t out_id_size);
int  container_run_background(const ContainerSpec *spec, char *out_id, size_t out_id_size);
int  container_start(const char *id);
int  container_stop(const char *id, int timeout_s); /* timeout_s=0 → use default (10s) */
int  container_delete(const char *id);
int  container_list(void);
void cleanup_all_containers(void);
void container_prune_all(void);

/* Scheduler integration (Module 6). */
void container_scheduler_refresh_targets(void);
void container_scheduler_on_started(pid_t pid);
void container_scheduler_on_stopped(pid_t pid);

/* Monitoring (Module 8). */
int container_stats(const char *id);
int container_stats_all(void);
int container_stats_watch(const char *id, unsigned int interval_sec);
int container_stats_all_watch(unsigned int interval_sec);
int container_inspect(const char *id);
int container_net(const char *id);
int container_net_summary(void);
int container_logs(const char *id);
int container_logs_tail(const char *id, int n);
int container_logs_follow(const char *id);
int container_logs_json_tail(const char *id, int n, char *buf, int buflen);
int container_exec(const char *id, const char *command_line);
int container_pause(const char *id);
int container_unpause(const char *id);
int container_security_show(const char *id);

/* Orchestrator helpers. */
int container_get_info(const char *id, ContainerState *state,
                       int *exit_code, pid_t *pid);
int container_exec_quiet(const char *id, const char *command_line);

/* Web API helpers. */
void container_refresh_state(void);
int container_json_all(char *buf, int buflen);
int container_stats_json_all(char *buf, int buflen);
int container_send_signal(const char *id, int sig);

/* Ctrl+C handling for watch loops / CLI. */
void container_request_interrupt(void);
int container_consume_interrupt(void);

#endif
