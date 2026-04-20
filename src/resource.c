#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#include "resource.h"

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
    if (config == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer,
             buffer_size,
             "cpu=%s mem=%s nproc=%s",
             (config->cpu_seconds == 0) ? "unlimited" : "",
             (config->memory_mb == 0) ? "unlimited" : "",
             (config->max_processes == 0) ? "unlimited" : "");

    if (config->cpu_seconds != 0 || config->memory_mb != 0 || config->max_processes != 0) {
        char cpu_text[32];
        char mem_text[32];
        char proc_text[32];

        if (config->cpu_seconds == 0) {
            snprintf(cpu_text, sizeof(cpu_text), "unlimited");
        } else {
            snprintf(cpu_text, sizeof(cpu_text), "%us", config->cpu_seconds);
        }

        if (config->memory_mb == 0) {
            snprintf(mem_text, sizeof(mem_text), "unlimited");
        } else {
            snprintf(mem_text, sizeof(mem_text), "%uMB", config->memory_mb);
        }

        if (config->max_processes == 0) {
            snprintf(proc_text, sizeof(proc_text), "unlimited");
        } else {
            snprintf(proc_text, sizeof(proc_text), "%u", config->max_processes);
        }

        snprintf(buffer, buffer_size, "cpu=%s mem=%s nproc=%s", cpu_text, mem_text, proc_text);
    }
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
