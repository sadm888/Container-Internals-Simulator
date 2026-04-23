#ifndef LOGGER_H
#define LOGGER_H

void log_event(const char *format, ...);
void log_event_type(const char *event_type, const char *format, ...);
const char *logger_path(void);
int logger_print(const char *container_id);
int logger_tail(const char *container_id, int line_count);
int logger_follow(const char *container_id, int initial_tail_lines);

#endif
