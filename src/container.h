#ifndef CONTAINER_H
#define CONTAINER_H

#include <sys/types.h>

typedef enum {
    STATE_RUNNING,
    STATE_STOPPED
} ContainerState;

typedef struct Container {
    int               id;
    pid_t             pid;
    ContainerState    state;
    char              status[16];
    char             *stack;           // dynamically allocated 1MB stack
    struct Container *next;            // linked list pointer
} Container;

int  container_create(void);
int  container_list(void);
int  container_stop(int id);
void cleanup_all_containers(void);

#endif