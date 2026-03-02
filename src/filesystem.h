#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stddef.h>

/* creates /tmp/container-sim/container-<id>/{proc,etc/hostname}
 * writes the full path into path_out, returns 0 on success */
int create_rootfs(int container_id, char *path_out, size_t path_size);

#endif
