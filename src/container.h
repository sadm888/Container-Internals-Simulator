#ifndef CONTAINER_H
#define CONTAINER_H

#include <stddef.h>
#include <sys/types.h>

#include "resource.h"

#define CONTAINER_ID_LEN 64
#define CONTAINER_NAME_LEN 64
#define CONTAINER_HOSTNAME_LEN 64
#define CONTAINER_ROOTFS_LEN 512
#define CONTAINER_COMMAND_LEN 256

typedef enum {
    STATE_CREATED,
    STATE_RUNNING,
    STATE_STOPPED
} ContainerState;

typedef struct {
    const char *name;
    const char *hostname;
    const char *rootfs;
    const char *command_line;
    ResourceConfig resource_limits;
} ContainerSpec;

typedef struct Container {
    char              id[CONTAINER_ID_LEN];
    char              name[CONTAINER_NAME_LEN];
    pid_t             pid;
    char              hostname[CONTAINER_HOSTNAME_LEN];
    char              rootfs[CONTAINER_ROOTFS_LEN];
    char              command_line[CONTAINER_COMMAND_LEN];
    ResourceConfig    resource_limits;
    ContainerState    state;
    char             *stack;
    struct Container *next;
} Container;

int  container_manager_init(void);
int  container_create(const ContainerSpec *spec, char *out_id, size_t out_id_size);
int  container_run(const ContainerSpec *spec, char *out_id, size_t out_id_size);
int  container_run_background(const ContainerSpec *spec, char *out_id, size_t out_id_size);
int  container_start(const char *id);
int  container_stop(const char *id);
int  container_delete(const char *id);
int  container_list(void);
void cleanup_all_containers(void);

/* Scheduler integration (Module 6). */
void container_scheduler_refresh_targets(void);
void container_scheduler_on_started(pid_t pid);
void container_scheduler_on_stopped(pid_t pid);

/* Monitoring (Module 8). */
int container_stats(const char *id);
int container_stats_all(void);
int container_stats_watch(const char *id, unsigned int interval_sec);
int container_stats_all_watch(unsigned int interval_sec);

/* Ctrl+C handling for watch loops / CLI. */
void container_request_interrupt(void);
int container_consume_interrupt(void);

#endif
