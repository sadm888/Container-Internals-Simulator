#ifndef CONTAINER_H
#define CONTAINER_H

#include <stddef.h>
#include <sys/types.h>

#define CONTAINER_ID_LEN 64
#define CONTAINER_NAME_LEN 64
#define CONTAINER_HOSTNAME_LEN 64
#define CONTAINER_ROOTFS_LEN 512

typedef enum {
    STATE_CREATED,
    STATE_RUNNING,
    STATE_STOPPED
} ContainerState;

typedef struct {
    const char *name;
    const char *hostname;
    const char *rootfs;
} ContainerSpec;

typedef struct Container {
    char              id[CONTAINER_ID_LEN];
    char              name[CONTAINER_NAME_LEN];
    pid_t             pid;
    char              hostname[CONTAINER_HOSTNAME_LEN];
    char              rootfs[CONTAINER_ROOTFS_LEN];
    ContainerState    state;
    char             *stack;
    struct Container *next;
} Container;

int  container_manager_init(void);
int  container_create(const ContainerSpec *spec, char *out_id, size_t out_id_size);
int  container_start(const char *id);
int  container_stop(const char *id);
int  container_delete(const char *id);
int  container_list(void);
void cleanup_all_containers(void);

#endif
