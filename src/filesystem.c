#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "filesystem.h"

#define BASE "/tmp/container-sim"

static void mkdirp(const char *path) {
    mkdir(path, 0755); /* ignore EEXIST */
}

int create_rootfs(int id, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/container-%d", BASE, id);

    mkdirp(BASE);
    mkdirp(out);

    char tmp[256];

    snprintf(tmp, sizeof(tmp), "%s/proc", out);
    mkdirp(tmp);

    snprintf(tmp, sizeof(tmp), "%s/etc", out);
    mkdirp(tmp);

    /* hostname file so the container has something under /etc */
    snprintf(tmp, sizeof(tmp), "%s/etc/hostname", out);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fprintf(f, "container-%d\n", id);
        fclose(f);
    }

    return 0;
}
