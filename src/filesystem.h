#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stddef.h>

/*
 * create_rootfs()
 *
 * Creates an isolated directory tree for the container under
 * /tmp/container-sim/container-<id>/
 *
 * Structure created:
 *   container-<id>/
 *     proc/        ← mount point for isolated /proc
 *     etc/
 *       hostname   ← contains "container-<id>"
 *
 * path_out receives the full path string.
 * Returns 0 on success, -1 on failure.
 */
int create_rootfs(int container_id, char *path_out, size_t path_size);

#endif /* FILESYSTEM_H */
