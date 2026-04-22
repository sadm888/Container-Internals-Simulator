#ifndef MONITOR_H
#define MONITOR_H

#include <stddef.h>
#include <sys/types.h>

#define MONITOR_PROFILE "/proc: stat + status"

typedef struct {
    pid_t pid;
    char state;
    unsigned long long utime_ticks;
    unsigned long long stime_ticks;
    unsigned long vsize_bytes;
    long rss_pages;
    long threads;
    unsigned long rss_bytes;
    double cpu_seconds;
    /* disk I/O — from /proc/<pid>/io (best-effort; requires kernel support) */
    unsigned long long read_bytes_io;
    unsigned long long write_bytes_io;
} MonitorStats;

int monitor_read(pid_t pid, MonitorStats *out);
const char *monitor_profile(void);
void monitor_format(const MonitorStats *stats, char *buffer, size_t buffer_size);

#endif

