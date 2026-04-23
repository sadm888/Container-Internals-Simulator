#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "eventbus.h"
#include "logger.h"
#include "metrics.h"

#define EVENTS_LOG_PATH "events.log"

/* ── ring buffer state ───────────────────────────────────────────────── */

static Event           g_ring[EVENTBUS_RING_SIZE];
static unsigned        g_total = 0;
static pthread_mutex_t g_lock  = PTHREAD_MUTEX_INITIALIZER;
static FILE           *g_logfile = NULL;   /* append-only persistence */

static void json_escape_copy(char *dst, size_t dst_size, const char *src) {
    size_t out = 0;

    if (dst_size == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; src[i] != '\0' && out + 1 < dst_size; i++) {
        unsigned char ch = (unsigned char)src[i];
        if ((ch == '\\' || ch == '"') && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = (char)ch;
        } else if (ch == '\n' && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = 'n';
        } else if (ch == '\r' && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = 'r';
        } else if (ch == '\t' && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = 't';
        } else if (ch >= 0x20) {
            dst[out++] = (char)ch;
        }
    }

    dst[out] = '\0';
}

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
    [EVENT_ORCH_SPEC_UP]              = "ORCH_SPEC_UP",
    [EVENT_ORCH_SPEC_DOWN]            = "ORCH_SPEC_DOWN",
    [EVENT_ORCH_SVC_STARTED]          = "ORCH_SVC_STARTED",
    [EVENT_ORCH_SVC_HEALTHY]          = "ORCH_SVC_HEALTHY",
    [EVENT_ORCH_SVC_UNHEALTHY]        = "ORCH_SVC_UNHEALTHY",
    [EVENT_ORCH_SVC_RESTARTED]        = "ORCH_SVC_RESTARTED",
    [EVENT_ALERT_FIRED]               = "ALERT_FIRED",
    [EVENT_ALERT_RESOLVED]            = "ALERT_RESOLVED",
};

/* ── persistence helpers ─────────────────────────────────────────────── */

/* Parse a single tab-delimited events.log line back into an Event.
 * Format: seq\ttimestamp\ttype_name\tcontainer_id\tdetail\tvalue\n
 * Returns 1 on success, 0 on parse failure. */
static int parse_event_line(char *line, Event *e) {
    char *fields[6];
    int   n = 0;
    char *p = line;
    char *tab;

    while (n < 6) {
        tab = strchr(p, '\t');
        if (tab) { *tab = '\0'; fields[n++] = p; p = tab + 1; }
        else      { fields[n++] = p; break; }
    }
    if (n < 6) return 0;

    /* strip trailing newline from last field */
    char *nl = strchr(fields[5], '\n');
    if (nl) *nl = '\0';

    e->seq       = (unsigned)strtoul(fields[0], NULL, 10);
    e->timestamp = (time_t)strtol(fields[1], NULL, 10);
    e->value     = strtol(fields[5], NULL, 10);

    /* resolve type name → enum */
    e->type = EVENT_TYPE_COUNT;
    for (int i = 0; i < EVENT_TYPE_COUNT; i++) {
        if (g_type_names[i] && strcmp(g_type_names[i], fields[2]) == 0) {
            e->type = (EventType)i;
            break;
        }
    }
    if (e->type == EVENT_TYPE_COUNT) return 0;

    snprintf(e->container_id, sizeof(e->container_id), "%s",
             strcmp(fields[3], "-") == 0 ? "" : fields[3]);
    snprintf(e->detail, sizeof(e->detail), "%s",
             strcmp(fields[4], "-") == 0 ? "" : fields[4]);
    return 1;
}

/* Load the last RING_SIZE events from EVENTS_LOG_PATH into the ring buffer.
 * Uses a circular window so we only keep the tail. */
static void replay_events_file(void) {
    FILE *f = fopen(EVENTS_LOG_PATH, "r");
    if (!f) return;

    /* Allocate a rolling window of RING_SIZE events */
    Event *window = calloc(EVENTBUS_RING_SIZE, sizeof(Event));
    if (!window) { fclose(f); return; }

    unsigned count = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        Event e = {0};
        if (parse_event_line(line, &e)) {
            window[count & EVENTBUS_RING_MASK] = e;
            count++;
        }
    }
    fclose(f);

    if (count == 0) { free(window); return; }

    /* Replay into ring, preserving original sequence numbers */
    unsigned loaded = (count < EVENTBUS_RING_SIZE) ? count : EVENTBUS_RING_SIZE;
    unsigned start  = count - loaded;   /* first slot in window to use */

    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < loaded; i++) {
        const Event *e = &window[(start + i) & EVENTBUS_RING_MASK];
        unsigned slot  = e->seq & EVENTBUS_RING_MASK;
        g_ring[slot]   = *e;
    }
    /* Advance g_total to one past the highest replayed seq */
    const Event *last = &window[(start + loaded - 1) & EVENTBUS_RING_MASK];
    g_total = last->seq + 1;
    pthread_mutex_unlock(&g_lock);

    free(window);
}

/* ── public API ──────────────────────────────────────────────────────── */

void eventbus_init(void) {
    pthread_mutex_lock(&g_lock);
    memset(g_ring, 0, sizeof(g_ring));
    g_total = 0;
    pthread_mutex_unlock(&g_lock);

    replay_events_file();

    g_logfile = fopen(EVENTS_LOG_PATH, "a");
}

void eventbus_close(void) {
    if (g_logfile) {
        fflush(g_logfile);
        fclose(g_logfile);
        g_logfile = NULL;
    }
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

    /* Persist to events.log: seq\tts\ttype\tcid\tdetail\tvalue */
    if (g_logfile) {
        fprintf(g_logfile, "%u\t%ld\t%s\t%s\t%s\t%ld\n",
                e.seq,
                (long)e.timestamp,
                eventbus_type_name(e.type),
                e.container_id[0] ? e.container_id : "-",
                e.detail[0]       ? e.detail       : "-",
                e.value);
        fflush(g_logfile);
    }

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

int eventbus_json_recent(int n, char *buf, int buflen) {
    unsigned total, start, i;
    int written = 0;
    int first   = 1;

    if (buf == NULL || buflen <= 2) return -1;

    pthread_mutex_lock(&g_lock);
    total = g_total;
    {
        unsigned available = (total < EVENTBUS_RING_SIZE) ? total : EVENTBUS_RING_SIZE;
        unsigned want      = (n <= 0 || (unsigned)n > available) ? available : (unsigned)n;
        start = total - want;
    }

    written += snprintf(buf + written, (size_t)(buflen - written), "[");

    for (i = start; i < total && written < buflen - 256; i++) {
        const Event *e = &g_ring[i & EVENTBUS_RING_MASK];
        char tbuf[22] = "-";
        char type_json[40];
        char ts_json[32];
        char id_json[sizeof(e->container_id) * 2];
        char detail_json[sizeof(e->detail) * 2];
        struct tm *tm_info = localtime(&e->timestamp);
        if (tm_info) strftime(tbuf, sizeof(tbuf), "%Y-%m-%dT%H:%M:%S", tm_info);
        json_escape_copy(type_json, sizeof(type_json), eventbus_type_name(e->type));
        json_escape_copy(ts_json, sizeof(ts_json), tbuf);
        json_escape_copy(id_json, sizeof(id_json), e->container_id);
        json_escape_copy(detail_json, sizeof(detail_json), e->detail);

        written += snprintf(buf + written, (size_t)(buflen - written),
            "%s{"
            "\"seq\":%u,"
            "\"type\":\"%s\","
            "\"timestamp\":\"%s\","
            "\"container_id\":\"%s\","
            "\"detail\":\"%s\","
            "\"value\":%ld"
            "}",
            first ? "" : ",",
            e->seq,
            type_json,
            ts_json,
            id_json,
            detail_json,
            e->value);
        first = 0;
    }

    pthread_mutex_unlock(&g_lock);

    if (written < buflen - 1) {
        buf[written++] = ']';
        buf[written]   = '\0';
    }
    return written;
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
