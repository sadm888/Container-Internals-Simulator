#ifndef NAMESPACE_H
#define NAMESPACE_H

#include <sys/types.h>

/* spawns a new process under pid+uts+mount namespaces, returns host pid */
pid_t create_pid_namespace(int container_id,
                           const char *hostname,
                           const char *rootfs);

#endif
