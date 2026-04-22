#ifndef ALERT_H
#define ALERT_H

#include <time.h>

/*
 * Threshold alert system — watches CPU% and RSS(MB) per container.
 * Alerts fire when a metric crosses its threshold and resolve when it drops back.
 * State changes are emitted to the event bus and printed to the console.
 *
 * Usage:
 *   alert set <id> cpu <pct>   — fire when CPU% >= pct
 *   alert set <id> mem <MB>    — fire when RSS >= MB
 *   alert ls                   — list all configured alerts
 *   alert rm <id> [cpu|mem]    — remove alert(s) for container
 */

#define ALERT_MAX  128

typedef enum {
    ALERT_CPU = 0,   /* threshold is a CPU percentage (0–100+) */
    ALERT_MEM = 1    /* threshold is RSS in MB                  */
} AlertMetric;

typedef struct {
    char        container_id[64];
    AlertMetric metric;
    double      threshold;
    int         active;          /* 1 = slot in use               */
    int         firing;          /* 1 = currently over threshold  */
    time_t      fired_at;        /* time alert last fired         */
} Alert;

/* CRUD */
int  alert_set(const char *id, AlertMetric metric, double threshold);
int  alert_clear(const char *id, AlertMetric metric);  /* -1 if not found */
void alert_clear_all(void);

/* Query */
void alert_list(void);
int  alert_json(char *buf, int buflen);

/*
 * Called by the stats collection path with live values.
 * Fires or resolves alerts and emits events.
 * Returns number of state transitions (fires + resolves).
 */
int  alert_check(const char *id, double cpu_pct, double rss_mb);

#endif
