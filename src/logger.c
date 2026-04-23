#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "container.h"
#include "logger.h"

#define LOG_FILE "container.log"

static pthread_mutex_t g_logger_lock = PTHREAD_MUTEX_INITIALIZER;

static void log_event_type_va(const char *event_type, const char *format, va_list args) {
    FILE *file = fopen(LOG_FILE, "a");
    time_t now;
    struct tm *tm_info;
    char time_buffer[32];

    if (file == NULL) {
        return;
    }

    now = time(NULL);
    tm_info = localtime(&now);
    if (tm_info == NULL) {
        fclose(file);
        return;
    }

    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", tm_info);
    fprintf(file, "[%s] %s: ", time_buffer, event_type);
    vfprintf(file, format, args);
    fputc('\n', file);
    fclose(file);
}

void log_event_type(const char *event_type, const char *format, ...) {
    va_list args;

    if (event_type == NULL || event_type[0] == '\0' || format == NULL) {
        return;
    }

    pthread_mutex_lock(&g_logger_lock);
    va_start(args, format);
    log_event_type_va(event_type, format, args);
    va_end(args);
    pthread_mutex_unlock(&g_logger_lock);
}

void log_event(const char *format, ...) {
    va_list args;

    if (format == NULL) {
        return;
    }

    pthread_mutex_lock(&g_logger_lock);
    va_start(args, format);
    log_event_type_va("INFO", format, args);
    va_end(args);
    pthread_mutex_unlock(&g_logger_lock);
}

const char *logger_path(void) {
    return LOG_FILE;
}

static int line_matches_container(const char *line, const char *container_id) {
    if (line == NULL) {
        return 0;
    }
    if (container_id == NULL || container_id[0] == '\0') {
        return 1;
    }
    return strstr(line, container_id) != NULL;
}

static int logger_open_read(FILE **out_file) {
    if (out_file == NULL) {
        errno = EINVAL;
        return -1;
    }

    *out_file = fopen(LOG_FILE, "r");
    if (*out_file == NULL) {
        printf("[error] no logs found at %s\n\n", LOG_FILE);
        return -1;
    }

    return 0;
}

static int logger_print_stream(FILE *file, const char *container_id, int *printed) {
    char line[1024];

    if (file == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        if (!line_matches_container(line, container_id)) {
            continue;
        }
        fputs(line, stdout);
        if (printed != NULL) {
            *printed = 1;
        }
    }

    return 0;
}

int logger_print(const char *container_id) {
    FILE *file = NULL;
    int printed = 0;

    if (logger_open_read(&file) != 0) {
        return -1;
    }

    (void)logger_print_stream(file, container_id, &printed);
    fclose(file);

    if (!printed) {
        if (container_id != NULL && container_id[0] != '\0') {
            printf("[manager] no logs found for %s\n\n", container_id);
        } else {
            printf("[manager] no logs recorded yet\n\n");
        }
        return 0;
    }

    printf("\n");
    fflush(stdout);
    return 0;
}

int logger_tail(const char *container_id, int line_count) {
    FILE *file = NULL;
    char **lines = NULL;
    char buffer[1024];
    int count = 0;
    int start = 0;
    int printed = 0;

    if (line_count <= 0) {
        printf("[error] usage: logs [-f] [-n N] [id]\n\n");
        return -1;
    }

    if (logger_open_read(&file) != 0) {
        return -1;
    }

    lines = calloc((size_t)line_count, sizeof(*lines));
    if (lines == NULL) {
        fclose(file);
        printf("[error] out of memory\n\n");
        return -1;
    }

    while (fgets(buffer, sizeof(buffer), file) != NULL) {
        size_t len = 0;

        if (!line_matches_container(buffer, container_id)) {
            continue;
        }

        len = strlen(buffer);
        free(lines[count % line_count]);
        lines[count % line_count] = malloc(len + 1);
        if (lines[count % line_count] == NULL) {
            fclose(file);
            for (int i = 0; i < line_count; i++) {
                free(lines[i]);
            }
            free(lines);
            printf("[error] out of memory\n\n");
            return -1;
        }
        memcpy(lines[count % line_count], buffer, len + 1);
        count++;
    }

    fclose(file);

    if (count == 0) {
        free(lines);
        if (container_id != NULL && container_id[0] != '\0') {
            printf("[manager] no logs found for %s\n\n", container_id);
        } else {
            printf("[manager] no logs recorded yet\n\n");
        }
        return 0;
    }

    start = (count > line_count) ? (count - line_count) : 0;
    for (int i = start; i < count; i++) {
        char *line = lines[i % line_count];
        if (line != NULL) {
            fputs(line, stdout);
            printed = 1;
        }
    }

    for (int i = 0; i < line_count; i++) {
        free(lines[i]);
    }
    free(lines);

    if (printed) {
        printf("\n");
        fflush(stdout);
    }
    return 0;
}

int logger_follow(const char *container_id, int initial_tail_lines) {
    long offset = 0;

    if (initial_tail_lines > 0) {
        if (logger_tail(container_id, initial_tail_lines) != 0) {
            return -1;
        }
    } else {
        if (logger_print(container_id) != 0) {
            return -1;
        }
    }

    {
        FILE *file = NULL;
        if (logger_open_read(&file) != 0) {
            return -1;
        }
        if (fseek(file, 0, SEEK_END) != 0) {
            fclose(file);
            printf("[error] failed to seek logs\n\n");
            return -1;
        }
        offset = ftell(file);
        fclose(file);
    }

    printf("[hint] following %s%s%s; press Ctrl+C to stop\n",
           LOG_FILE,
           (container_id != NULL && container_id[0] != '\0') ? " for " : "",
           (container_id != NULL && container_id[0] != '\0') ? container_id : "");
    fflush(stdout);

    while (1) {
        struct stat st;

        if (container_consume_interrupt()) {
            printf("\n");
            return 0;
        }

        container_refresh_state();

        if (stat(LOG_FILE, &st) == 0) {
            if ((long)st.st_size < offset) {
                offset = 0;
            }

            if ((long)st.st_size > offset) {
                FILE *file = fopen(LOG_FILE, "r");
                if (file != NULL) {
                    if (fseek(file, offset, SEEK_SET) == 0) {
                        int printed = 0;
                        (void)logger_print_stream(file, container_id, &printed);
                        offset = ftell(file);
                        if (printed) {
                            fflush(stdout);
                        }
                    }
                    fclose(file);
                }
            }
        }
        {
            struct timespec ts;
            ts.tv_sec = 0;
            ts.tv_nsec = 200000000L;
            nanosleep(&ts, NULL);
        }
    }
}
