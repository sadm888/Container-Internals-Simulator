#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

#include "resource.h"

static int apply_single_limit(int resource, rlim_t value) {
    struct rlimit limit;

    limit.rlim_cur = value;
    limit.rlim_max = value;
    return setrlimit(resource, &limit);
}

int resource_apply_limits(const ResourceConfig *config) {
    rlim_t memory_bytes = 0;

    if (config == NULL || config->cpu_seconds == 0 || config->memory_mb == 0 || config->max_processes == 0) {
        errno = EINVAL;
        return -1;
    }

    if (apply_single_limit(RLIMIT_CPU, (rlim_t)config->cpu_seconds) != 0) {
        return -1;
    }

    memory_bytes = (rlim_t)config->memory_mb * 1024 * 1024;
    if (memory_bytes == 0) {
        errno = EINVAL;
        return -1;
    }

    if (apply_single_limit(RLIMIT_AS, memory_bytes) != 0) {
        return -1;
    }

    if (apply_single_limit(RLIMIT_NPROC, (rlim_t)config->max_processes) != 0) {
        return -1;
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
             "cpu=%us mem=%uMB nproc=%u",
             config->cpu_seconds,
             config->memory_mb,
             config->max_processes);
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
