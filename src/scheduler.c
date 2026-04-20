#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "scheduler.h"

typedef struct {
    int enabled;
    unsigned int time_slice_ms;
    pid_t *targets;
    size_t target_count;
    size_t target_capacity;
    size_t current_index;
    pthread_mutex_t mutex;
    pthread_t thread;
    int thread_started;
    int stop_requested;
} SchedulerState;

static SchedulerState g_state = {
    0,
    200,
    NULL,
    0,
    0,
    0,
    PTHREAD_MUTEX_INITIALIZER,
    0,
    0,
    0
};

static int pid_is_alive(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }
    if (kill(pid, 0) == 0) {
        return 1;
    }
    return errno == EPERM;
}

static void nanosleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000U);
    ts.tv_nsec = (long)((ms % 1000U) * 1000000UL);
    (void)nanosleep(&ts, NULL);
}

static int ensure_capacity_locked(size_t desired) {
    pid_t *next = NULL;
    size_t next_capacity = 0;

    if (desired <= g_state.target_capacity) {
        return 0;
    }

    next_capacity = (g_state.target_capacity == 0) ? 8 : g_state.target_capacity;
    while (next_capacity < desired) {
        next_capacity *= 2;
    }

    next = realloc(g_state.targets, next_capacity * sizeof(*next));
    if (next == NULL) {
        return -1;
    }

    g_state.targets = next;
    g_state.target_capacity = next_capacity;
    return 0;
}

static void prune_dead_locked(void) {
    size_t write_index = 0;

    for (size_t i = 0; i < g_state.target_count; i++) {
        pid_t pid = g_state.targets[i];
        if (!pid_is_alive(pid)) {
            continue;
        }
        g_state.targets[write_index++] = pid;
    }

    g_state.target_count = write_index;
    if (g_state.current_index >= g_state.target_count) {
        g_state.current_index = 0;
    }
}

static void *scheduler_thread_main(void *arg) {
    (void)arg;

    while (1) {
        int enabled = 0;
        unsigned int slice_ms = 0;
        pid_t current = -1;
        pid_t next = -1;
        size_t count = 0;

        pthread_mutex_lock(&g_state.mutex);
        if (g_state.stop_requested) {
            pthread_mutex_unlock(&g_state.mutex);
            break;
        }

        enabled = g_state.enabled;
        slice_ms = g_state.time_slice_ms;

        prune_dead_locked();
        count = g_state.target_count;

        if (!enabled || count < 2) {
            pthread_mutex_unlock(&g_state.mutex);
            nanosleep_ms(50);
            continue;
        }

        current = g_state.targets[g_state.current_index % count];
        g_state.current_index = (g_state.current_index + 1) % count;
        next = g_state.targets[g_state.current_index % count];
        pthread_mutex_unlock(&g_state.mutex);

        if (current > 0) {
            (void)kill(current, SIGSTOP);
        }
        if (next > 0) {
            (void)kill(next, SIGCONT);
        }

        nanosleep_ms(slice_ms);
    }

    return NULL;
}

int scheduler_start(const SchedulerConfig *config) {
    if (config == NULL || config->time_slice_ms == 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_state.mutex);
    g_state.time_slice_ms = config->time_slice_ms;

    if (!g_state.thread_started) {
        g_state.stop_requested = 0;
        if (pthread_create(&g_state.thread, NULL, scheduler_thread_main, NULL) != 0) {
            pthread_mutex_unlock(&g_state.mutex);
            errno = EAGAIN;
            return -1;
        }
        g_state.thread_started = 1;
    }
    pthread_mutex_unlock(&g_state.mutex);
    return 0;
}

void scheduler_stop(void) {
    pthread_mutex_lock(&g_state.mutex);
    if (!g_state.thread_started) {
        pthread_mutex_unlock(&g_state.mutex);
        return;
    }
    g_state.stop_requested = 1;
    pthread_mutex_unlock(&g_state.mutex);

    (void)pthread_join(g_state.thread, NULL);

    pthread_mutex_lock(&g_state.mutex);
    g_state.thread_started = 0;
    g_state.stop_requested = 0;
    pthread_mutex_unlock(&g_state.mutex);
}

int scheduler_set_enabled(int enabled) {
    pthread_mutex_lock(&g_state.mutex);
    g_state.enabled = enabled ? 1 : 0;
    pthread_mutex_unlock(&g_state.mutex);
    return 0;
}

int scheduler_set_time_slice_ms(unsigned int time_slice_ms) {
    if (time_slice_ms == 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&g_state.mutex);
    g_state.time_slice_ms = time_slice_ms;
    pthread_mutex_unlock(&g_state.mutex);
    return 0;
}

int scheduler_is_enabled(void) {
    int enabled = 0;
    pthread_mutex_lock(&g_state.mutex);
    enabled = g_state.enabled;
    pthread_mutex_unlock(&g_state.mutex);
    return enabled;
}

unsigned int scheduler_get_time_slice_ms(void) {
    unsigned int value = 0;
    pthread_mutex_lock(&g_state.mutex);
    value = g_state.time_slice_ms;
    pthread_mutex_unlock(&g_state.mutex);
    return value;
}

int scheduler_add_target(pid_t pid) {
    pthread_mutex_lock(&g_state.mutex);
    for (size_t i = 0; i < g_state.target_count; i++) {
        if (g_state.targets[i] == pid) {
            pthread_mutex_unlock(&g_state.mutex);
            return 0;
        }
    }

    if (ensure_capacity_locked(g_state.target_count + 1) != 0) {
        pthread_mutex_unlock(&g_state.mutex);
        errno = ENOMEM;
        return -1;
    }

    g_state.targets[g_state.target_count++] = pid;
    pthread_mutex_unlock(&g_state.mutex);
    return 0;
}

int scheduler_remove_target(pid_t pid) {
    int removed = 0;
    pthread_mutex_lock(&g_state.mutex);
    for (size_t i = 0; i < g_state.target_count; i++) {
        if (g_state.targets[i] != pid) {
            continue;
        }
        g_state.targets[i] = g_state.targets[g_state.target_count - 1];
        g_state.target_count--;
        removed = 1;
        break;
    }
    if (g_state.current_index >= g_state.target_count) {
        g_state.current_index = 0;
    }
    pthread_mutex_unlock(&g_state.mutex);
    return removed ? 0 : -1;
}

void scheduler_clear_targets(void) {
    pthread_mutex_lock(&g_state.mutex);
    g_state.target_count = 0;
    g_state.current_index = 0;
    pthread_mutex_unlock(&g_state.mutex);
}

const char *scheduler_profile(void) {
    return SCHEDULER_PROFILE;
}

