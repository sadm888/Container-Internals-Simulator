#ifndef FILESYSTEM_H
#define FILESYSTEM_H

#include <stddef.h>

#define FILESYSTEM_PROFILE "pivot_root + /proc (chroot fallback, proc best-effort)"

typedef struct {
    const char *rootfs;
} FilesystemConfig;

int filesystem_prepare_rootfs(const char *requested_rootfs,
                              char *resolved_rootfs,
                              size_t resolved_rootfs_size);
int filesystem_isolate_rootfs(const FilesystemConfig *config);
const char *filesystem_profile(void);
void filesystem_format_error(int err, char *buffer, size_t buffer_size);

#endif
