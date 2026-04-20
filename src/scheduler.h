#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stddef.h>
#include <sys/types.h>

#define SCHEDULER_PROFILE "round-robin (SIGSTOP/SIGCONT)"

typedef struct {
    unsigned int time_slice_ms;
} SchedulerConfig;

int scheduler_start(const SchedulerConfig *config);
void scheduler_stop(void);
int scheduler_set_enabled(int enabled);
int scheduler_set_time_slice_ms(unsigned int time_slice_ms);
int scheduler_is_enabled(void);
unsigned int scheduler_get_time_slice_ms(void);

int scheduler_add_target(pid_t pid);
int scheduler_remove_target(pid_t pid);
void scheduler_clear_targets(void);

const char *scheduler_profile(void);

#endif

