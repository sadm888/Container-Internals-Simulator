#ifndef RESOURCE_H
#define RESOURCE_H

#include <stddef.h>

#define RESOURCE_PROFILE "RLIMIT_CPU + RLIMIT_AS + RLIMIT_NPROC"

#define DEFAULT_CPU_LIMIT_SECONDS 30
#define DEFAULT_MEMORY_LIMIT_MB 256
#define DEFAULT_PROCESS_LIMIT 32

#define RESOURCE_ERROR_BASE 10000

typedef struct {
    unsigned int cpu_seconds;
    unsigned int memory_mb;
    unsigned int max_processes;
} ResourceConfig;

int resource_apply_limits(const ResourceConfig *config);
int resource_try_cgroup(const char *container_id, const ResourceConfig *config, pid_t pid);
void resource_cleanup_cgroup(const char *container_id);
const char *resource_profile(void);
void resource_format_limits(const ResourceConfig *config, char *buffer, size_t buffer_size);
void resource_format_error(int err, char *buffer, size_t buffer_size);

#endif
