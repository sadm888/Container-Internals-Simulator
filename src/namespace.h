#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <stddef.h>
#include <sys/types.h>

#include "resource.h"

#define NAMESPACE_PROFILE "PID | UTS | MOUNT | NET"

typedef struct {
    const char *hostname;
    const char *rootfs;
    const char *command_line;
    ResourceConfig resource_limits;
} NamespaceConfig;

typedef struct {
    pid_t host_pid;
    pid_t namespace_pid;
} NamespaceStartResult;

int namespace_start_container(const NamespaceConfig *config,
                              char *stack,
                              size_t stack_size,
                              NamespaceStartResult *result);
const char *namespace_profile(void);
void namespace_format_start_error(int err, char *buffer, size_t buffer_size);

#endif
