#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "eventbus.h"
#include "logger.h"
#include "metrics.h"

/* ── ring buffer state ───────────────────────────────────────────────── */

static Event          g_ring[EVENTBUS_RING_SIZE];
static unsigned       g_total = 0;          /* total events ever emitted   */
static pthread_mutex_t g_lock  = PTHREAD_MUTEX_INITIALIZER;

/* ── type → name table ───────────────────────────────────────────────── */

static const char *g_type_names[EVENT_TYPE_COUNT] = {
    [EVENT_CONTAINER_CREATED]         = "CONTAINER_CREATED",
    [EVENT_CONTAINER_STARTED]         = "CONTAINER_STARTED",
    [EVENT_CONTAINER_STOPPED]         = "CONTAINER_STOPPED",
    [EVENT_CONTAINER_DELETED]         = "CONTAINER_DELETED",
    [EVENT_CONTAINER_PAUSED]          = "CONTAINER_PAUSED",
    [EVENT_CONTAINER_UNPAUSED]        = "CONTAINER_UNPAUSED",
    [EVENT_EXEC_LAUNCHED]             = "EXEC_LAUNCHED",
    [EVENT_OOM_KILL]                  = "OOM_KILL",
    [EVENT_IMAGE_BUILT]               = "IMAGE_BUILT",
    [EVENT_IMAGE_REMOVED]             = "IMAGE_REMOVED",
    [EVENT_IMAGE_TAGGED]              = "IMAGE_TAGGED",
    [EVENT_NET_INIT]                  = "NET_INIT",
    [EVENT_NET_TEARDOWN]              = "NET_TEARDOWN",
    [EVENT_SECURITY_PROFILE_APPLIED]  = "SECURITY_PROFILE_APPLIED",
    [EVENT_SCHED_ENABLED]             = "SCHED_ENABLED",
    [EVENT_SCHED_DISABLED]            = "SCHED_DISABLED",
};

/* ── public API ──────────────────────────────────────────────────────── */

void eventbus_init(void) {
    pthread_mutex_lock(&g_lock);
    memset(g_ring, 0, sizeof(g_ring));
    g_total = 0;
    pthread_mutex_unlock(&g_lock);
}

const char *eventbus_type_name(EventType type) {
    if (type < 0 || type >= EVENT_TYPE_COUNT) return "UNKNOWN";
    return g_type_names[type] ? g_type_names[type] : "UNKNOWN";
}

unsigned eventbus_total(void) {
    unsigned t;
    pthread_mutex_lock(&g_lock);
    t = g_total;
    pthread_mutex_unlock(&g_lock);
    return t;
}

void eventbus_emit(EventType   type,
                   const char *container_id,
                   const char *detail,
                   long        value) {
    Event e;
    unsigned slot;

    e.type      = type;
    e.timestamp = time(NULL);
    e.value     = value;

    if (container_id && container_id[0])
        snprintf(e.container_id, sizeof(e.container_id), "%s", container_id);
    else
        e.container_id[0] = '\0';

    if (detail && detail[0])
        snprintf(e.detail, sizeof(e.detail), "%s", detail);
    else
        e.detail[0] = '\0';

    pthread_mutex_lock(&g_lock);
    e.seq = g_total;
    slot  = g_total & EVENTBUS_RING_MASK;
    g_ring[slot] = e;
    g_total++;
    pthread_mutex_unlock(&g_lock);

    /* Route to logger (file-based) — outside the lock to avoid contention. */
    if (e.container_id[0] && e.detail[0])
        log_event("[event] %s  id=%-18s  %s  val=%ld",
                  eventbus_type_name(type), e.container_id, e.detail, value);
    else if (e.container_id[0])
        log_event("[event] %s  id=%-18s  val=%ld",
                  eventbus_type_name(type), e.container_id, value);
    else
        log_event("[event] %s  %s", eventbus_type_name(type), e.detail);

    /* Route to metrics aggregator. */
    metrics_on_event(&e);
}

/* ── display helpers ─────────────────────────────────────────────────── */

static void print_event(const Event *e) {
    char tbuf[20];
    struct tm *tm_info;

    tm_info = localtime(&e->timestamp);
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tm_info);

    /* Format: [HH:MM:SS] #SEQ  TYPE                 id=XXX  detail  (val=N) */
    printf("  [%s] #%-4u  %-30s", tbuf, e->seq, eventbus_type_name(e->type));

    if (e->container_id[0])
        printf("  id=%-18s", e->container_id);
    else
        printf("  %-22s", "");

    if (e->detail[0])
        printf("  %s", e->detail);

    if (e->value != 0)
        printf("  (val=%ld)", e->value);

    printf("\n");
}

void eventbus_print_recent(int n) {
    eventbus_print_filtered(n, EVENT_TYPE_COUNT);
}

void eventbus_print_filtered(int n, EventType type_filter) {
    unsigned total, start, i;
    int printed = 0;

    pthread_mutex_lock(&g_lock);
    total = g_total;
    pthread_mutex_unlock(&g_lock);

    if (total == 0) {
        printf("  (no events yet)\n\n");
        return;
    }

    {
        unsigned available = (total < EVENTBUS_RING_SIZE) ? total : EVENTBUS_RING_SIZE;
        unsigned want      = (n <= 0 || (unsigned)n > available) ? available : (unsigned)n;
        start = total - want;
    }

    printf("\n");
    if (type_filter < EVENT_TYPE_COUNT)
        printf("  Filter : %s\n", eventbus_type_name(type_filter));
    printf("  %-8s %-4s  %-30s  %-20s  %s\n",
           "TIME", "SEQ", "EVENT", "CONTAINER", "DETAIL");
    printf("  %s\n", "─────────────────────────────────────────────────────────────────────");

    pthread_mutex_lock(&g_lock);
    for (i = start; i < total; i++) {
        const Event *e = &g_ring[i & EVENTBUS_RING_MASK];
        if (type_filter == EVENT_TYPE_COUNT || e->type == type_filter) {
            print_event(e);
            printed++;
        }
    }
    pthread_mutex_unlock(&g_lock);

    if (printed == 0)
        printf("  (no matching events)\n");
    printf("\n");
}

unsigned eventbus_drain_from(unsigned from_seq) {
    unsigned total, i;

    pthread_mutex_lock(&g_lock);
    total = g_total;
    pthread_mutex_unlock(&g_lock);

    if (from_seq >= total) return total;

    /* Clamp: can only go back RING_SIZE events */
    if (total - from_seq > EVENTBUS_RING_SIZE)
        from_seq = total - EVENTBUS_RING_SIZE;

    pthread_mutex_lock(&g_lock);
    for (i = from_seq; i < total; i++) {
        print_event(&g_ring[i & EVENTBUS_RING_MASK]);
    }
    pthread_mutex_unlock(&g_lock);

    return total;
}
