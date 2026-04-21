#ifndef METRICS_H
#define METRICS_H

#include <time.h>
#include "eventbus.h"

/*
 * Runtime metrics aggregator — subscribes to the event bus and maintains
 * counters and latency histograms for the lifetime of the simulator.
 *
 * Startup latency is tracked with millisecond precision using
 * CLOCK_MONOTONIC; the p99 approximation uses a 1 ms-bucket array.
 */

#define METRICS_LAT_BUCKETS 2000   /* 0–1999 ms; 2000+ clamped to last bucket */

typedef struct {
    /* ── lifecycle counters ────────────────────────────────────────── */
    unsigned long containers_started;
    unsigned long containers_stopped;
    unsigned long containers_deleted;
    unsigned long containers_paused;
    unsigned long exec_launches;
    unsigned long oom_kills;
    unsigned long images_built;
    unsigned long images_removed;
    unsigned long sched_toggles;
    unsigned long containers_unpaused;
    unsigned long events_total;

    /* ── startup latency (ms) ──────────────────────────────────────── */
    unsigned long startup_count;
    unsigned long startup_total_ms;
    unsigned long startup_max_ms;
    unsigned long startup_min_ms;      /* ULONG_MAX if no samples yet */
    unsigned long startup_buckets[METRICS_LAT_BUCKETS]; /* histogram */

    /* ── memory high-water mark ────────────────────────────────────── */
    unsigned long mem_highwater_mb;

    /* ── time ──────────────────────────────────────────────────────── */
    time_t        uptime_start;
    time_t        last_event_time;
} Metrics;

void           metrics_init(void);
void           metrics_on_event(const Event *e);       /* called by eventbus  */
void           metrics_record_startup_ms(unsigned long ms); /* called by container.c */
void           metrics_update_mem_highwater(unsigned long mb);

const Metrics *metrics_snapshot(void);   /* returns pointer to live struct  */
void           metrics_print(void);      /* table to stdout                 */
void           metrics_print_prometheus(void); /* Prometheus exposition format */

#endif
