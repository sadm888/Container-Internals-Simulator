#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

#include "eventbus.h"
#include "metrics.h"

static Metrics          g_metrics;
static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;

void metrics_init(void) {
    pthread_mutex_lock(&g_lock);
    g_metrics = (Metrics){0};
    g_metrics.startup_min_ms  = ULONG_MAX;
    g_metrics.uptime_start    = time(NULL);
    pthread_mutex_unlock(&g_lock);
}

void metrics_on_event(const Event *e) {
    pthread_mutex_lock(&g_lock);
    g_metrics.last_event_time = e->timestamp;
    g_metrics.events_total++;

    switch (e->type) {
        case EVENT_CONTAINER_STARTED:   g_metrics.containers_started++;   break;
        case EVENT_CONTAINER_STOPPED:   g_metrics.containers_stopped++;   break;
        case EVENT_CONTAINER_DELETED:   g_metrics.containers_deleted++;   break;
        case EVENT_CONTAINER_PAUSED:    g_metrics.containers_paused++;    break;
        case EVENT_CONTAINER_UNPAUSED:  g_metrics.containers_unpaused++;  break;
        case EVENT_EXEC_LAUNCHED:       g_metrics.exec_launches++;        break;
        case EVENT_OOM_KILL:            g_metrics.oom_kills++;            break;
        case EVENT_IMAGE_BUILT:         g_metrics.images_built++;         break;
        case EVENT_IMAGE_REMOVED:       g_metrics.images_removed++;       break;
        case EVENT_SCHED_ENABLED:
        case EVENT_SCHED_DISABLED:      g_metrics.sched_toggles++;        break;
        default: break;
    }
    pthread_mutex_unlock(&g_lock);
}

void metrics_record_startup_ms(unsigned long ms) {
    unsigned long bucket;

    pthread_mutex_lock(&g_lock);
    g_metrics.startup_count++;
    g_metrics.startup_total_ms += ms;

    if (ms > g_metrics.startup_max_ms)
        g_metrics.startup_max_ms = ms;
    if (ms < g_metrics.startup_min_ms)
        g_metrics.startup_min_ms = ms;

    bucket = (ms < METRICS_LAT_BUCKETS) ? ms : (METRICS_LAT_BUCKETS - 1);
    g_metrics.startup_buckets[bucket]++;
    pthread_mutex_unlock(&g_lock);
}

void metrics_update_mem_highwater(unsigned long mb) {
    pthread_mutex_lock(&g_lock);
    if (mb > g_metrics.mem_highwater_mb)
        g_metrics.mem_highwater_mb = mb;
    pthread_mutex_unlock(&g_lock);
}

const Metrics *metrics_snapshot(void) {
    return &g_metrics;   /* caller reads under their own lock if needed */
}

/* ── p99 approximation from histogram ───────────────────────────────── */

static unsigned long compute_p99(const Metrics *m) {
    unsigned long target, cumulative, i;

    if (m->startup_count == 0) return 0;

    target     = (m->startup_count * 99 + 99) / 100;   /* ceil(count * 0.99) */
    cumulative = 0;

    for (i = 0; i < METRICS_LAT_BUCKETS; i++) {
        cumulative += m->startup_buckets[i];
        if (cumulative >= target)
            return i;
    }
    return METRICS_LAT_BUCKETS - 1;
}

/* ── Prometheus-style table output ───────────────────────────────────── */

void metrics_print(void) {
    Metrics snap;
    time_t  now;
    long    uptime;
    unsigned long avg_ms;

    pthread_mutex_lock(&g_lock);
    snap = g_metrics;
    pthread_mutex_unlock(&g_lock);

    now    = time(NULL);
    uptime = (long)(now - snap.uptime_start);

    printf("\n");
    printf("  %-42s %s\n", "METRIC", "VALUE");
    printf("  %s\n",
           "──────────────────────────────────────────────────────");

    /* ── lifecycle ── */
    printf("  %-42s %lu\n", "containers_started_total",   snap.containers_started);
    printf("  %-42s %lu\n", "containers_stopped_total",   snap.containers_stopped);
    printf("  %-42s %lu\n", "containers_deleted_total",   snap.containers_deleted);
    printf("  %-42s %lu\n", "containers_paused_total",    snap.containers_paused);
    printf("  %-42s %lu\n", "containers_unpaused_total",  snap.containers_unpaused);
    printf("  %-42s %lu\n", "exec_launches_total",        snap.exec_launches);
    printf("  %-42s %lu\n", "oom_kills_total",            snap.oom_kills);
    printf("  %-42s %lu\n", "images_built_total",         snap.images_built);
    printf("  %-42s %lu\n", "images_removed_total",       snap.images_removed);
    printf("  %-42s %lu\n", "scheduler_toggles_total",    snap.sched_toggles);
    printf("  %-42s %lu\n", "events_total",               snap.events_total);

    printf("  %s\n",
           "──────────────────────────────────────────────────────");

    /* ── startup latency ── */
    if (snap.startup_count > 0) {
        avg_ms = snap.startup_total_ms / snap.startup_count;
        printf("  %-42s %lu\n",  "startup_latency_samples",       snap.startup_count);
        printf("  %-42s %lu ms\n","startup_latency_min_ms",        snap.startup_min_ms);
        printf("  %-42s %lu ms\n","startup_latency_avg_ms",        avg_ms);
        printf("  %-42s %lu ms\n","startup_latency_max_ms",        snap.startup_max_ms);
        printf("  %-42s %lu ms\n","startup_latency_p99_ms",        compute_p99(&snap));
    } else {
        printf("  %-42s %s\n",   "startup_latency_samples", "0 (no containers started)");
    }

    printf("  %s\n",
           "──────────────────────────────────────────────────────");

    /* ── resource high-water ── */
    printf("  %-42s %lu MB\n", "memory_highwater_mb", snap.mem_highwater_mb);

    /* ── uptime ── */
    printf("  %-42s %lds\n",   "simulator_uptime_seconds", uptime);

    if (snap.last_event_time > 0) {
        long idle = (long)(now - snap.last_event_time);
        printf("  %-42s %lds ago\n", "last_event_age_seconds", idle);
    }

    printf("\n");
}

/* ── Prometheus exposition format (/metrics endpoint) ────────────────── */

void metrics_print_prometheus(void) {
    Metrics snap;
    time_t  now;
    long    uptime;
    unsigned long avg_ms, p99_ms;

    pthread_mutex_lock(&g_lock);
    snap = g_metrics;
    pthread_mutex_unlock(&g_lock);

    now    = time(NULL);
    uptime = (long)(now - snap.uptime_start);

#define PCTR(name, help, val) \
    printf("# HELP " name " " help "\n" \
           "# TYPE " name " counter\n" \
           name " %lu\n\n", (unsigned long)(val))

#define PGAUGE(name, help, val) \
    printf("# HELP " name " " help "\n" \
           "# TYPE " name " gauge\n" \
           name " %lu\n\n", (unsigned long)(val))

    PCTR("containers_started_total",
         "Total number of containers started.",            snap.containers_started);
    PCTR("containers_stopped_total",
         "Total number of containers stopped.",            snap.containers_stopped);
    PCTR("containers_deleted_total",
         "Total number of containers deleted.",            snap.containers_deleted);
    PCTR("containers_paused_total",
         "Total number of pause operations.",              snap.containers_paused);
    PCTR("containers_unpaused_total",
         "Total number of unpause operations.",            snap.containers_unpaused);
    PCTR("exec_launches_total",
         "Total number of exec launches.",                 snap.exec_launches);
    PCTR("oom_kills_total",
         "Total number of OOM kill events.",               snap.oom_kills);
    PCTR("images_built_total",
         "Total number of images registered.",             snap.images_built);
    PCTR("images_removed_total",
         "Total number of images removed.",                snap.images_removed);
    PCTR("scheduler_toggles_total",
         "Total number of scheduler enable/disable ops.",  snap.sched_toggles);
    PCTR("events_total",
         "Total number of events emitted on the bus.",     snap.events_total);

    PGAUGE("memory_highwater_megabytes",
           "Peak RSS memory across all containers (MB).",  snap.mem_highwater_mb);
    PGAUGE("simulator_uptime_seconds",
           "Seconds since simulator started.",             (unsigned long)uptime);

    if (snap.startup_count > 0) {
        avg_ms = snap.startup_total_ms / snap.startup_count;
        p99_ms = compute_p99(&snap);

        printf("# HELP startup_latency_milliseconds"
               " Container startup latency (ms) summary.\n");
        printf("# TYPE startup_latency_milliseconds summary\n");
        printf("startup_latency_milliseconds{quantile=\"0.99\"} %lu\n", p99_ms);
        printf("startup_latency_milliseconds_sum %lu\n",   snap.startup_total_ms);
        printf("startup_latency_milliseconds_count %lu\n\n", snap.startup_count);

        PGAUGE("startup_latency_min_milliseconds",
               "Minimum container startup latency (ms).", snap.startup_min_ms);
        PGAUGE("startup_latency_avg_milliseconds",
               "Average container startup latency (ms).", avg_ms);
        PGAUGE("startup_latency_max_milliseconds",
               "Maximum container startup latency (ms).", snap.startup_max_ms);
    }

#undef PCTR
#undef PGAUGE
}
