#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "resource.h"

#define CGROUP_ROOT "/sys/fs/cgroup"

static int apply_single_limit_lower_only(int resource, rlim_t requested) {
    struct rlimit current;
    struct rlimit next;

    if (getrlimit(resource, &current) != 0) {
        return -1;
    }

    next = current;

    /* Do not try to raise limits as an unprivileged process. */
    if (current.rlim_cur != RLIM_INFINITY && requested > current.rlim_cur) {
        requested = current.rlim_cur;
    }
    if (current.rlim_max != RLIM_INFINITY && requested > current.rlim_max) {
        requested = current.rlim_max;
    }

    if (requested == current.rlim_cur && requested == current.rlim_max) {
        return 0;
    }

    next.rlim_cur = requested;
    if (next.rlim_max == RLIM_INFINITY || requested < next.rlim_max) {
        next.rlim_max = requested;
    }
    return setrlimit(resource, &next);
}

int resource_apply_limits(const ResourceConfig *config) {
    rlim_t memory_bytes = 0;
    unsigned long long memory_bytes_ull = 0;

    if (config == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 0 means "unlimited" (skip applying). */
    if (config->cpu_seconds != 0) {
        if (apply_single_limit_lower_only(RLIMIT_CPU, (rlim_t)config->cpu_seconds) != 0) {
            return -1;
        }
    }

    if (config->memory_mb != 0) {
        memory_bytes_ull = (unsigned long long)config->memory_mb * 1024ULL * 1024ULL;
        memory_bytes = (rlim_t)memory_bytes_ull;
        if (memory_bytes_ull != (unsigned long long)memory_bytes) {
            errno = EINVAL;
            return -1;
        }

        if (memory_bytes == 0) {
            errno = EINVAL;
            return -1;
        }

        if (apply_single_limit_lower_only(RLIMIT_AS, memory_bytes) != 0) {
            return -1;
        }
    }

    if (config->max_processes != 0) {
        if (apply_single_limit_lower_only(RLIMIT_NPROC, (rlim_t)config->max_processes) != 0) {
            return -1;
        }
    }

    return 0;
}

const char *resource_profile(void) {
    return RESOURCE_PROFILE;
}

void resource_format_limits(const ResourceConfig *config, char *buffer, size_t buffer_size) {
    char cpu_text[32];
    char mem_text[32];
    char proc_text[32];

    if (config == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }

    if (config->cpu_seconds == 0)
        snprintf(cpu_text, sizeof(cpu_text), "unlimited");
    else
        snprintf(cpu_text, sizeof(cpu_text), "%us", config->cpu_seconds);

    if (config->memory_mb == 0)
        snprintf(mem_text, sizeof(mem_text), "unlimited");
    else
        snprintf(mem_text, sizeof(mem_text), "%uMB", config->memory_mb);

    if (config->max_processes == 0)
        snprintf(proc_text, sizeof(proc_text), "unlimited");
    else
        snprintf(proc_text, sizeof(proc_text), "%u", config->max_processes);

    snprintf(buffer, buffer_size, "cpu=%s mem=%s nproc=%s", cpu_text, mem_text, proc_text);
}

void resource_format_error(int err, char *buffer, size_t buffer_size) {
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    switch (err) {
        case EPERM:
            snprintf(buffer, buffer_size, "resource limits need privileges the host denied");
            break;
        case EINVAL:
            snprintf(buffer, buffer_size, "resource control received an invalid limit configuration");
            break;
        default:
            snprintf(buffer, buffer_size, "%s", strerror(err));
            break;
    }
}

static int write_cgroup_file(const char *cgroup_path, const char *filename, const char *value) {
    char path[512];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", cgroup_path, filename);
    f = fopen(path, "w");
    if (f == NULL) {
        return -1;
    }
    fputs(value, f);
    fclose(f);
    return 0;
}

int resource_try_cgroup(const char *container_id, const ResourceConfig *config, pid_t pid) {
    char cgroup_path[256];
    char value[64];

    if (container_id == NULL || config == NULL || pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    snprintf(cgroup_path, sizeof(cgroup_path), "%s/%s", CGROUP_ROOT, container_id);
    if (mkdir(cgroup_path, 0755) != 0 && errno != EEXIST) {
        return -1;
    }

    if (config->memory_mb != 0) {
        snprintf(value, sizeof(value), "%llu\n",
                 (unsigned long long)config->memory_mb * 1024ULL * 1024ULL);
        if (write_cgroup_file(cgroup_path, "memory.max", value) != 0) {
            rmdir(cgroup_path);
            return -1;
        }
        /* Suppress swap so the memory limit can't be bypassed via swapping.
         * Best-effort: swap accounting may not be available on all kernels. */
        write_cgroup_file(cgroup_path, "memory.swap.max", "0\n");
    }

    if (config->max_processes != 0) {
        snprintf(value, sizeof(value), "%u\n", config->max_processes);
        write_cgroup_file(cgroup_path, "pids.max", value);
    }

    /* cpu_seconds is a total-time budget enforced by RLIMIT_CPU in the child.
     * cgroups cpu.max controls rate, not total time — skip it here. */

    snprintf(value, sizeof(value), "%d\n", (int)pid);
    if (write_cgroup_file(cgroup_path, "cgroup.procs", value) != 0) {
        rmdir(cgroup_path);
        return -1;
    }

    return 0;
}

void resource_cleanup_cgroup(const char *container_id) {
    char cgroup_path[256];

    if (container_id == NULL) {
        return;
    }

    snprintf(cgroup_path, sizeof(cgroup_path), "%s/%s", CGROUP_ROOT, container_id);
    rmdir(cgroup_path);
}
