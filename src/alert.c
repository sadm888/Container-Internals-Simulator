#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "alert.h"
#include "eventbus.h"

static Alert           g_alerts[ALERT_MAX];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *metric_name(AlertMetric m) {
    return (m == ALERT_CPU) ? "cpu" : "mem";
}

/* ── CRUD ────────────────────────────────────────────────────────────── */

int alert_set(const char *id, AlertMetric metric, double threshold) {
    int i, free_slot = -1;

    if (id == NULL || id[0] == '\0' || threshold < 0) {
        errno = EINVAL;
        return -1;
    }

    pthread_mutex_lock(&g_lock);

    /* Update existing alert for same id+metric, or find a free slot. */
    for (i = 0; i < ALERT_MAX; i++) {
        if (!g_alerts[i].active) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (strcmp(g_alerts[i].container_id, id) == 0 &&
            g_alerts[i].metric == metric) {
            g_alerts[i].threshold = threshold;
            g_alerts[i].firing    = 0;   /* re-arm on update */
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }

    if (free_slot < 0) {
        pthread_mutex_unlock(&g_lock);
        errno = ENOMEM;
        return -1;
    }

    g_alerts[free_slot].active    = 1;
    g_alerts[free_slot].firing    = 0;
    g_alerts[free_slot].metric    = metric;
    g_alerts[free_slot].threshold = threshold;
    g_alerts[free_slot].fired_at  = 0;
    snprintf(g_alerts[free_slot].container_id,
             sizeof(g_alerts[free_slot].container_id), "%s", id);

    pthread_mutex_unlock(&g_lock);
    return 0;
}

int alert_clear(const char *id, AlertMetric metric) {
    int i, found = 0;

    if (id == NULL) { errno = EINVAL; return -1; }

    pthread_mutex_lock(&g_lock);
    for (i = 0; i < ALERT_MAX; i++) {
        if (!g_alerts[i].active) continue;
        if (strcmp(g_alerts[i].container_id, id) == 0 &&
            g_alerts[i].metric == metric) {
            g_alerts[i].active = 0;
            found = 1;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return found ? 0 : -1;
}

void alert_clear_all(void) {
    pthread_mutex_lock(&g_lock);
    memset(g_alerts, 0, sizeof(g_alerts));
    pthread_mutex_unlock(&g_lock);
}

/* ── display ─────────────────────────────────────────────────────────── */

void alert_list(void) {
    int i, any = 0;

    pthread_mutex_lock(&g_lock);
    printf("\n  %-18s %-6s %-12s %-10s %s\n",
           "CONTAINER", "METRIC", "THRESHOLD", "FIRING", "FIRED AT");
    printf("  ───────────────────────────────────────────────────────────\n");

    for (i = 0; i < ALERT_MAX; i++) {
        if (!g_alerts[i].active) continue;
        char thresh_buf[24];
        char fired_buf[20] = "-";

        if (g_alerts[i].metric == ALERT_CPU)
            snprintf(thresh_buf, sizeof(thresh_buf), "%.1f%%", g_alerts[i].threshold);
        else
            snprintf(thresh_buf, sizeof(thresh_buf), "%.0f MB", g_alerts[i].threshold);

        if (g_alerts[i].fired_at > 0) {
            struct tm *t = localtime(&g_alerts[i].fired_at);
            strftime(fired_buf, sizeof(fired_buf), "%H:%M:%S", t);
        }

        printf("  %-18s %-6s %-12s %-10s %s\n",
               g_alerts[i].container_id,
               metric_name(g_alerts[i].metric),
               thresh_buf,
               g_alerts[i].firing ? "\033[31mFIRING\033[0m" : "ok",
               fired_buf);
        any = 1;
    }

    if (!any) printf("  (no alerts configured)\n");
    printf("\n");
    pthread_mutex_unlock(&g_lock);
}

/* ── JSON serialiser (for /api/alerts REST endpoint) ─────────────────── */

int alert_json(char *buf, int buflen) {
    int i, written = 0, first = 1;

    if (buf == NULL || buflen <= 2) return -1;

    pthread_mutex_lock(&g_lock);
    written += snprintf(buf + written, (size_t)(buflen - written), "[");

    for (i = 0; i < ALERT_MAX && written < buflen - 256; i++) {
        if (!g_alerts[i].active) continue;
        char thresh_str[24];
        char fired_buf[22] = "";

        if (g_alerts[i].metric == ALERT_CPU)
            snprintf(thresh_str, sizeof(thresh_str), "%.1f%%", g_alerts[i].threshold);
        else
            snprintf(thresh_str, sizeof(thresh_str), "%.0fMB", g_alerts[i].threshold);

        if (g_alerts[i].fired_at > 0) {
            struct tm *t = localtime(&g_alerts[i].fired_at);
            strftime(fired_buf, sizeof(fired_buf), "%Y-%m-%dT%H:%M:%S", t);
        }

        written += snprintf(buf + written, (size_t)(buflen - written),
            "%s{"
            "\"container_id\":\"%s\","
            "\"metric\":\"%s\","
            "\"threshold\":\"%s\","
            "\"firing\":%s,"
            "\"fired_at\":\"%s\""
            "}",
            first ? "" : ",",
            g_alerts[i].container_id,
            metric_name(g_alerts[i].metric),
            thresh_str,
            g_alerts[i].firing ? "true" : "false",
            fired_buf);
        first = 0;
    }

    pthread_mutex_unlock(&g_lock);

    if (written < buflen - 1) {
        buf[written++] = ']';
        buf[written]   = '\0';
    }
    return written;
}

/* ── threshold checking ──────────────────────────────────────────────── */

int alert_check(const char *id, double cpu_pct, double rss_mb) {
    int i, transitions = 0;

    if (id == NULL) return 0;

    pthread_mutex_lock(&g_lock);

    for (i = 0; i < ALERT_MAX; i++) {
        if (!g_alerts[i].active) continue;
        if (strcmp(g_alerts[i].container_id, id) != 0) continue;

        double value = (g_alerts[i].metric == ALERT_CPU) ? cpu_pct : rss_mb;
        int over     = (value >= g_alerts[i].threshold);
        char detail[64];

        if (over && !g_alerts[i].firing) {
            /* transition: ok → FIRING */
            g_alerts[i].firing   = 1;
            g_alerts[i].fired_at = time(NULL);
            transitions++;

            if (g_alerts[i].metric == ALERT_CPU)
                snprintf(detail, sizeof(detail), "cpu=%.1f%% >= %.1f%%",
                         cpu_pct, g_alerts[i].threshold);
            else
                snprintf(detail, sizeof(detail), "rss=%.1fMB >= %.0fMB",
                         rss_mb, g_alerts[i].threshold);

            /* unlock before I/O and event emission */
            pthread_mutex_unlock(&g_lock);
            printf("\n\033[31m[ALERT]\033[0m %s  %s  %s\n\n",
                   id, metric_name(g_alerts[i].metric), detail);
            eventbus_emit(EVENT_ALERT_FIRED, id, detail, (long)value);
            pthread_mutex_lock(&g_lock);

        } else if (!over && g_alerts[i].firing) {
            /* transition: FIRING → ok (resolved) */
            g_alerts[i].firing = 0;
            transitions++;

            if (g_alerts[i].metric == ALERT_CPU)
                snprintf(detail, sizeof(detail), "cpu=%.1f%% < %.1f%%",
                         cpu_pct, g_alerts[i].threshold);
            else
                snprintf(detail, sizeof(detail), "rss=%.1fMB < %.0fMB",
                         rss_mb, g_alerts[i].threshold);

            pthread_mutex_unlock(&g_lock);
            printf("\n\033[32m[RESOLVED]\033[0m %s  %s  %s\n\n",
                   id, metric_name(g_alerts[i].metric), detail);
            eventbus_emit(EVENT_ALERT_RESOLVED, id, detail, (long)value);
            pthread_mutex_lock(&g_lock);
        }
    }

    pthread_mutex_unlock(&g_lock);
    return transitions;
}
