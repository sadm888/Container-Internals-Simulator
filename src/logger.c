/*
 * logger.c — Persistent event log for container lifecycle events
 *
 * All events are appended to "container.log" in the current directory
 * with a human-readable timestamp prefix.
 *
 * Log format:
 *   [YYYY-MM-DD HH:MM:SS] <message>
 */

#include <stdio.h>
#include <time.h>
#include <stdarg.h>

#include "logger.h"

#define LOG_FILE "container.log"

void log_event(const char *format, ...) {
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;   /* silently skip if file can't be opened */

    /* Build timestamp string */
    time_t     t       = time(NULL);
    struct tm *tm_info = localtime(&t);
    char       tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm_info);

    fprintf(f, "[%s] ", tbuf);

    /* Write the caller's formatted message */
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);

    fprintf(f, "\n");
    fclose(f);
}
