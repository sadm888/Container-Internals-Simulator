#ifndef LOGGER_H
#define LOGGER_H

/*
 * log_event() — append a timestamped entry to container.log
 *
 * Accepts printf-style format string and variadic arguments.
 * Example:
 *   log_event("Container %d: RUNNING (host PID %d)", id, pid);
 */
void log_event(const char *format, ...);

#endif /* LOGGER_H */
