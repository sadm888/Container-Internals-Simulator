#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <stddef.h>
#include <sys/types.h>

#include "network.h"
#include "resource.h"
#include "security.h"

#define NAMESPACE_PROFILE "PID | UTS | MOUNT | NET"

/*
 * Callback invoked by namespace_start_container after clone() but BEFORE
 * writing the ContinueToken to the child.  The parent uses this window to
 * create the veth pair and move the peer into the container's network
 * namespace (identified by container_pid).
 *
 * On success: fill *cfg with the peer interface name, IP, and gateway,
 *             then return 0.
 * On failure or "no networking": leave cfg zeroed and return 0.
 *             Returning -1 aborts container startup.
 */
typedef int (*ns_net_setup_cb)(pid_t container_pid,
                               NetConfig *cfg,
                               void *userdata);

typedef struct {
    const char      *hostname;
    const char      *rootfs;
    const char      *command_line;
    ResourceConfig   resource_limits;
    SecurityConfig   security;
    int              log_fd;        /* -1 = inherit terminal */
    ns_net_setup_cb  net_setup;     /* NULL = loopback only  */
    void            *net_setup_data;
} NamespaceConfig;

typedef struct {
    pid_t host_pid;
    pid_t namespace_pid;
    int   exec_trigger_fd; /* write '1' here to trigger child's execv */
    int   exec_pipe_rd;    /* read to check execvp result (EOF = ok)  */
} NamespaceStartResult;

/*
 * Phase 1: clone + namespace setup.  Returns with child blocked waiting
 * for namespace_trigger_exec().  Caller prints banners between the two calls.
 */
int namespace_start_container(const NamespaceConfig    *config,
                              char                     *stack,
                              size_t                    stack_size,
                              NamespaceStartResult     *result);

/*
 * Phase 2: signal the child to call execv.
 * Must be called exactly once after namespace_start_container succeeds.
 */
int namespace_trigger_exec(NamespaceStartResult *result);

const char *namespace_profile(void);
void        namespace_format_start_error(int err, char *buffer, size_t buffer_size);

#endif
