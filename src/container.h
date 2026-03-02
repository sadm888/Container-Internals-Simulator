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
    int           id;
    pid_t         pid;
    ContainerState state;
    char          status[16];
} Container;

/* Lifecycle operations */
int  container_create(void);
int  container_list(void);
int  container_stop(int id);
void cleanup_all_containers(void);

#endif /* CONTAINER_H */
