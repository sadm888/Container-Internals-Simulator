#ifndef CONTAINER_H
#define CONTAINER_H

#include <sys/types.h>

#define MAX_CONTAINERS 10

typedef enum {
    STATE_CREATED,
    STATE_RUNNING,
    STATE_PAUSED,
    STATE_STOPPED
} ContainerState;

typedef struct {
    int            id;
    pid_t          pid;
    ContainerState state;
    char           status[16];
    char           hostname[32];   /* UTS namespace hostname  */
    char           rootfs[128];    /* chroot path             */
} Container;

int  container_create(void);
int  container_list(void);
int  container_stop(int id);
void cleanup_all_containers(void);

#endif
