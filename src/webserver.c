#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "alert.h"
#include "container.h"
#include "eventbus.h"
#include "metrics.h"
#include "webserver.h"

#define MAX_REQUEST   4096
#define BUF_EVENTS   32768   /* 32 KB — enough for 100 events */
#define BUF_METRICS   4096
#define BUF_CONTAINERS (256 * 1024)  /* 256 KB — up to ~700 containers */

static int           g_server_fd = -1;
static pthread_t     g_server_thread;
static volatile int  g_running   = 0;

/* ── HTTP response helpers ───────────────────────────────────────────── */

static void send_response(int fd, int code, const char *reason,
                          const char *ctype, const char *body, int bodylen) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n\r\n",
        code, reason, ctype, bodylen);
    (void)write(fd, hdr, n);
    if (body && bodylen > 0) (void)write(fd, body, bodylen);
}

static void resp_json(int fd, int code, const char *reason,
                      const char *body, int bodylen) {
    send_response(fd, code, reason, "application/json", body, bodylen);
}

static void resp_ok_json(int fd, const char *body, int bodylen) {
    resp_json(fd, 200, "OK", body, bodylen);
}

static void resp_not_found(int fd) {
    const char *b = "{\"error\":\"not found\"}";
    resp_json(fd, 404, "Not Found", b, (int)strlen(b));
}

static void resp_accepted(int fd) {
    const char *b = "{\"status\":\"accepted\"}";
    resp_json(fd, 202, "Accepted", b, (int)strlen(b));
}

/* ── serve static file ───────────────────────────────────────────────── */

static void serve_file(int fd, const char *path, const char *ctype) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) { resp_not_found(fd); return; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)sz + 1);
    if (buf == NULL) { fclose(f); resp_not_found(fd); return; }

    (void)fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[sz] = '\0';

    send_response(fd, 200, "OK", ctype, buf, (int)sz);
    free(buf);
}

/* ── metrics → JSON ──────────────────────────────────────────────────── */

static int metrics_to_json(char *buf, int buflen) {
    const Metrics *m  = metrics_snapshot();
    time_t         now = time(NULL);
    long uptime = (m->uptime_start > 0) ? (long)(now - m->uptime_start) : 0;
    unsigned long avg_ms = (m->startup_count > 0)
                           ? m->startup_total_ms / m->startup_count : 0;

    return snprintf(buf, (size_t)buflen,
        "{"
        "\"containers_started\":%lu,"
        "\"containers_stopped\":%lu,"
        "\"containers_deleted\":%lu,"
        "\"containers_paused\":%lu,"
        "\"exec_launches\":%lu,"
        "\"oom_kills\":%lu,"
        "\"images_built\":%lu,"
        "\"images_removed\":%lu,"
        "\"sched_toggles\":%lu,"
        "\"events_total\":%lu,"
        "\"uptime_seconds\":%ld,"
        "\"startup_avg_ms\":%lu,"
        "\"startup_max_ms\":%lu,"
        "\"mem_highwater_mb\":%lu"
        "}",
        m->containers_started,
        m->containers_stopped,
        m->containers_deleted,
        m->containers_paused,
        m->exec_launches,
        m->oom_kills,
        m->images_built,
        m->images_removed,
        m->sched_toggles,
        m->events_total,
        uptime,
        avg_ms,
        m->startup_max_ms,
        m->mem_highwater_mb);
}

/* ── query string parser ─────────────────────────────────────────────── */

static int qs_int(const char *qs, const char *key, int def) {
    if (qs == NULL) return def;
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *p = strstr(qs, search);
    if (p == NULL) return def;
    return (int)strtol(p + strlen(search), NULL, 10);
}

/* ── connection handler ──────────────────────────────────────────────── */

static void handle_connection(int fd) {
    char req[MAX_REQUEST];
    int  n = (int)read(fd, req, sizeof(req) - 1);
    if (n <= 0) return;
    req[n] = '\0';

    char method[8] = {0};
    char path[256]  = {0};
    if (sscanf(req, "%7s %255s", method, path) != 2) return;

    /* split path from query string */
    char *qs = strchr(path, '?');
    if (qs) { *qs = '\0'; qs++; }

    if (strcmp(method, "OPTIONS") == 0) {
        const char *preflight =
            "HTTP/1.0 200 OK\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)write(fd, preflight, strlen(preflight));
        return;
    }

    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
            serve_file(fd, "web/index.html", "text/html; charset=utf-8");
        } else if (strcmp(path, "/style.css") == 0) {
            serve_file(fd, "web/style.css", "text/css; charset=utf-8");
        } else if (strcmp(path, "/theme.css") == 0) {
            serve_file(fd, "web/theme.css", "text/css; charset=utf-8");
        } else if (strcmp(path, "/app.js") == 0) {
            serve_file(fd, "web/app.js", "application/javascript; charset=utf-8");

        } else if (strcmp(path, "/api/containers") == 0) {
            char *buf = malloc(BUF_CONTAINERS);
            if (!buf) { resp_not_found(fd); return; }
            int len = container_json_all(buf, BUF_CONTAINERS);
            if (len < 0) { snprintf(buf, BUF_CONTAINERS, "[]"); len = 2; }
            resp_ok_json(fd, buf, len);
            free(buf);

        } else if (strcmp(path, "/api/events") == 0) {
            int want = qs_int(qs, "n", 50);
            char *buf = malloc(BUF_EVENTS);
            if (!buf) { resp_not_found(fd); return; }
            int len = eventbus_json_recent(want, buf, BUF_EVENTS);
            if (len < 0) { snprintf(buf, BUF_EVENTS, "[]"); len = 2; }
            resp_ok_json(fd, buf, len);
            free(buf);

        } else if (strcmp(path, "/api/metrics") == 0) {
            char buf[BUF_METRICS];
            int len = metrics_to_json(buf, sizeof(buf));
            resp_ok_json(fd, buf, len);

        } else if (strcmp(path, "/api/alerts") == 0) {
            char buf[8192];
            int len = alert_json(buf, sizeof(buf));
            if (len < 0) { snprintf(buf, sizeof(buf), "[]"); len = 2; }
            resp_ok_json(fd, buf, len);

        } else if (strcmp(path, "/api/stats") == 0) {
            char *buf = malloc(BUF_CONTAINERS);
            if (!buf) { resp_not_found(fd); return; }
            int len = container_stats_json_all(buf, BUF_CONTAINERS);
            if (len < 0) { snprintf(buf, BUF_CONTAINERS, "{}"); len = 2; }
            resp_ok_json(fd, buf, len);
            free(buf);

        } else {
            resp_not_found(fd);
        }

    } else if (strcmp(method, "POST") == 0) {
        char cid[64] = {0};
        if (sscanf(path, "/api/containers/%63[^/]/stop", cid) == 1) {
            if (container_send_signal(cid, SIGTERM) == 0)
                resp_accepted(fd);
            else
                resp_not_found(fd);
        } else {
            resp_not_found(fd);
        }

    } else {
        resp_not_found(fd);
    }
}

/* ── per-connection thread ───────────────────────────────────────────── */

static void *conn_thread_fn(void *arg) {
    int fd = *(int *)arg;
    free(arg);
    handle_connection(fd);
    close(fd);
    return NULL;
}

/* ── accept loop thread ──────────────────────────────────────────────── */

static void *server_thread_fn(void *arg) {
    (void)arg;

    while (g_running) {
        int *cfd = malloc(sizeof(int));
        if (!cfd) continue;

        *cfd = accept(g_server_fd, NULL, NULL);
        if (*cfd < 0) {
            free(cfd);
            continue;
        }

        pthread_t t;
        if (pthread_create(&t, NULL, conn_thread_fn, cfd) != 0) {
            close(*cfd);
            free(cfd);
        } else {
            pthread_detach(t);
        }
    }
    return NULL;
}

/* ── public API ──────────────────────────────────────────────────────── */

int webserver_start(int port) {
    struct sockaddr_in addr;
    int opt = 1;

    if (g_running) return 0; /* already started */

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) return -1;

    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    if (listen(g_server_fd, 16) < 0) {
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    g_running = 1;
    if (pthread_create(&g_server_thread, NULL, server_thread_fn, NULL) != 0) {
        g_running   = 0;
        close(g_server_fd);
        g_server_fd = -1;
        return -1;
    }

    return 0;
}

void webserver_stop(void) {
    if (!g_running) return;
    g_running = 0;
    if (g_server_fd >= 0) {
        shutdown(g_server_fd, SHUT_RDWR);
        close(g_server_fd);
        g_server_fd = -1;
    }
    pthread_join(g_server_thread, NULL);
}
