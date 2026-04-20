#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "monitor.h"

static int read_file_to_buffer(const char *path, char *buffer, size_t buffer_size) {
    FILE *file = NULL;
    size_t n = 0;

    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }

    n = fread(buffer, 1, buffer_size - 1, file);
    if (ferror(file)) {
        int saved_errno = errno;
        fclose(file);
        errno = saved_errno;
        return -1;
    }

    buffer[n] = '\0';
    fclose(file);
    return 0;
}

static int parse_proc_stat(pid_t pid, MonitorStats *out) {
    char path[128];
    char buffer[4096];
    char *left_paren = NULL;
    char *right_paren = NULL;
    char *rest = NULL;
    char *tokens[64] = {0};
    int token_count = 0;
    char *cursor = NULL;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (read_file_to_buffer(path, buffer, sizeof(buffer)) != 0) {
        return -1;
    }

    left_paren = strchr(buffer, '(');
    right_paren = strrchr(buffer, ')');
    if (left_paren == NULL || right_paren == NULL || right_paren <= left_paren) {
        errno = EPROTO;
        return -1;
    }

    rest = right_paren + 2; /* skip ") " */
    if (rest[0] == '\0') {
        errno = EPROTO;
        return -1;
    }

    out->state = rest[0];
    rest += 2; /* skip "S " */

    cursor = rest;
    while (cursor != NULL && token_count < (int)(sizeof(tokens) / sizeof(tokens[0]))) {
        tokens[token_count++] = cursor;
        cursor = strchr(cursor, ' ');
        if (cursor == NULL) {
            break;
        }
        *cursor = '\0';
        cursor++;
    }

    /*
     * tokens[0] = ppid (field 4)
     * tokens[10] = utime (field 14)
     * tokens[11] = stime (field 15)
     * tokens[16] = num_threads (field 20)
     * tokens[19] = vsize (field 23)
     * tokens[20] = rss (field 24)
     */
    if (token_count < 21) {
        errno = EPROTO;
        return -1;
    }

    out->utime_ticks = strtoull(tokens[10], NULL, 10);
    out->stime_ticks = strtoull(tokens[11], NULL, 10);
    out->threads = strtol(tokens[16], NULL, 10);
    out->vsize_bytes = strtoul(tokens[19], NULL, 10);
    out->rss_pages = strtol(tokens[20], NULL, 10);
    return 0;
}

static int parse_proc_status(pid_t pid, MonitorStats *out) {
    char path[128];
    char buffer[8192];
    char *line = NULL;
    char *saveptr = NULL;
    long threads = -1;

    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    if (read_file_to_buffer(path, buffer, sizeof(buffer)) != 0) {
        return -1;
    }

    line = strtok_r(buffer, "\n", &saveptr);
    while (line != NULL) {
        if (strncmp(line, "Threads:", 8) == 0) {
            threads = strtol(line + 8, NULL, 10);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    if (threads > 0) {
        out->threads = threads;
    }

    return 0;
}

int monitor_read(pid_t pid, MonitorStats *out) {
    long page_size = 0;
    long hz = 0;

    if (out == NULL || pid <= 0) {
        errno = EINVAL;
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->pid = pid;
    out->threads = -1;

    if (parse_proc_stat(pid, out) != 0) {
        return -1;
    }

    /* Best-effort. */
    (void)parse_proc_status(pid, out);

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size > 0 && out->rss_pages > 0) {
        out->rss_bytes = (unsigned long)out->rss_pages * (unsigned long)page_size;
    }

    hz = sysconf(_SC_CLK_TCK);
    if (hz > 0) {
        out->cpu_seconds = (double)(out->utime_ticks + out->stime_ticks) / (double)hz;
    }

    return 0;
}

const char *monitor_profile(void) {
    return MONITOR_PROFILE;
}

void monitor_format(const MonitorStats *stats, char *buffer, size_t buffer_size) {
    double rss_mb = 0.0;
    double vsize_mb = 0.0;

    if (stats == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }

    rss_mb = (double)stats->rss_bytes / (1024.0 * 1024.0);
    vsize_mb = (double)stats->vsize_bytes / (1024.0 * 1024.0);

    snprintf(buffer,
             buffer_size,
             "state=%c cpu=%.2fs rss=%.1fMB vsize=%.1fMB thr=%ld",
             stats->state,
             stats->cpu_seconds,
             rss_mb,
             vsize_mb,
             stats->threads);
}

