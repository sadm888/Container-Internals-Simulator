#ifndef EVENTBUS_H
#define EVENTBUS_H

#include <time.h>

/*
 * Typed event bus — decouples all runtime modules from the logger and
 * metrics collector.  Any module emits structured events; the bus routes
 * them to the ring buffer (for the CLI `events` command) and to the
 * metrics aggregator simultaneously.
 *
 * Design: single global ring buffer (lock-free write via atomic sequence,
 * mutex-protected full publish path). No heap allocation per event.
 */

#define EVENTBUS_RING_SIZE  256   /* power-of-2 so seq % SIZE == seq & MASK */
#define EVENTBUS_RING_MASK  (EVENTBUS_RING_SIZE - 1)
#define EVENTBUS_DETAIL_LEN 128

typedef enum {
    /* Container lifecycle */
    EVENT_CONTAINER_CREATED = 0,
    EVENT_CONTAINER_STARTED,      /* value = startup_latency_ms */
    EVENT_CONTAINER_STOPPED,      /* value = exit_code          */
    EVENT_CONTAINER_DELETED,
    EVENT_CONTAINER_PAUSED,
    EVENT_CONTAINER_UNPAUSED,

    /* Execution */
    EVENT_EXEC_LAUNCHED,          /* value = exec exit_code     */
    EVENT_OOM_KILL,               /* value = rss_mb at kill     */

    /* Image registry */
    EVENT_IMAGE_BUILT,
    EVENT_IMAGE_REMOVED,
    EVENT_IMAGE_TAGGED,

    /* Network */
    EVENT_NET_INIT,
    EVENT_NET_TEARDOWN,

    /* Security */
    EVENT_SECURITY_PROFILE_APPLIED, /* value = cap_drop count   */

    /* Scheduler */
    EVENT_SCHED_ENABLED,
    EVENT_SCHED_DISABLED,

    EVENT_TYPE_COUNT   /* sentinel — keep last */
} EventType;

typedef struct {
    EventType type;
    time_t    timestamp;
    char      container_id[64];
    char      detail[EVENTBUS_DETAIL_LEN];
    long      value;   /* type-specific numeric payload */
    unsigned  seq;     /* monotonic sequence number     */
} Event;

/* Initialise — call once at startup before any emit. */
void eventbus_init(void);

/*
 * Emit a structured event.  Thread-safe.
 * Internally: (1) writes to ring buffer, (2) calls log_event(),
 *             (3) updates metrics counters.
 * container_id and detail may be NULL (stored as "").
 */
void eventbus_emit(EventType    type,
                   const char  *container_id,
                   const char  *detail,
                   long         value);

/* Human-readable type name for display. */
const char *eventbus_type_name(EventType type);

/* Current write sequence number (total events emitted). */
unsigned eventbus_total(void);

/*
 * Print the most recent `n` events to stdout.
 * If n == 0, prints all events currently in the ring buffer.
 */
void eventbus_print_recent(int n);

/*
 * Stream events starting from sequence `from_seq`.
 * Returns the next sequence number to pass on subsequent calls
 * (i.e. eventbus_total() at the time of return).
 * Prints any new events since from_seq; returns immediately.
 */
unsigned eventbus_drain_from(unsigned from_seq);

/*
 * Like eventbus_print_recent but filters by event type.
 * Pass EVENT_TYPE_COUNT as type_filter to show all types (equivalent
 * to eventbus_print_recent).
 */
void eventbus_print_filtered(int n, EventType type_filter);

#endif
