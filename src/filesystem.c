/*
 * filesystem.c — Per-container root filesystem setup
 *
 * Each container gets its own isolated directory tree.
 * After chroot() into this directory, the container process cannot
 * access anything outside it — simulating filesystem isolation.
 *
 * Directory layout:
 *   /tmp/container-sim/
 *     container-1/
 *       proc/          ← isolated /proc mount point
 *       etc/
 *         hostname     ← "container-1"
 *     container-2/
 *       ...
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "filesystem.h"

#define BASE_DIR "/tmp/container-sim"

static void make_dir(const char *path) {
    mkdir(path, 0755);   /* ignore EEXIST — fine if it already exists */
}

int create_rootfs(int container_id, char *path_out, size_t path_size) {
    /* Build the container root path */
    snprintf(path_out, path_size, "%s/container-%d", BASE_DIR, container_id);

    /* Create directory hierarchy */
    make_dir(BASE_DIR);
    make_dir(path_out);

    char subdir[256];

    /* /proc — mount point for isolated proc filesystem */
    snprintf(subdir, sizeof(subdir), "%s/proc", path_out);
    make_dir(subdir);

    /* /etc — basic config directory */
    snprintf(subdir, sizeof(subdir), "%s/etc", path_out);
    make_dir(subdir);

    /* /etc/hostname — container's own hostname file */
    char hostname_file[256];
    snprintf(hostname_file, sizeof(hostname_file), "%s/etc/hostname", path_out);
    FILE *f = fopen(hostname_file, "w");
    if (f) {
        fprintf(f, "container-%d\n", container_id);
        fclose(f);
    }

    return 0;
}
