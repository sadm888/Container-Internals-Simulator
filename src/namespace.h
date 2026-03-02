#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <sys/types.h>

/*
 * create_pid_namespace()
 *
 * Forks a new process into isolated namespaces using clone(2):
 *
 *   CLONE_NEWPID  — PID namespace  : child sees itself as PID 1
 *   CLONE_NEWUTS  — UTS namespace  : child gets its own hostname
 *   CLONE_NEWNS   — Mount namespace: child gets its own mount table
 *
 * Inside the child:
 *   - sethostname() sets the container's hostname
 *   - /proc is remounted so ps only shows container processes
 *   - chroot() jails the process into its own rootfs directory
 *
 * Returns host PID of the container process, or -1 on failure.
 */
pid_t create_pid_namespace(int container_id,
                           const char *hostname,
                           const char *rootfs);

#endif /* NAMESPACE_H */
