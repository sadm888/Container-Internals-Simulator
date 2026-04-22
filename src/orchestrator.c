#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "container.h"
#include "eventbus.h"
#include "image.h"
#include "logger.h"
#include "orchestrator.h"

/* ── global active spec ─────────────────────────────────────────────────── */

static OrchestratorSpec  g_spec;
static int               g_running = 0;
static pthread_t         g_mon_tid = 0;
static pthread_mutex_t   g_lock    = PTHREAD_MUTEX_INITIALIZER;

/* ═══════════════════════════════════════════════════════════════════════════
 * Minimal JSON parser
 *
 * Recursive-descent parser covering the subset used by spec files:
 * objects, arrays, strings, numbers, booleans, null.
 * All allocation comes from a fixed 128 KB arena — no heap, no free().
 * ═══════════════════════════════════════════════════════════════════════════ */

#define JP_ARENA    (128 * 1024)
#define JP_MAX_OBJ  64
#define JP_MAX_ARR  64

typedef enum { JN_NULL, JN_BOOL, JN_NUM, JN_STR, JN_ARR, JN_OBJ } JNType;
typedef struct JNode JNode;
struct JNode {
    JNType type;
    union {
        int    b;
        double n;
        char  *s;
        struct { JNode **v; int len; } arr;
        struct { char **k; JNode **v; int len; } obj;
    } u;
};

typedef struct {
    const char *src;
    int         pos;
    int         len;
    char        arena[JP_ARENA];
    int         apos;
    char        err[128];
} JP;

static void *jp_alloc(JP *p, int sz) {
    int aligned = (sz + 7) & ~7;
    if (p->apos + aligned > JP_ARENA) {
        snprintf(p->err, sizeof(p->err), "arena exhausted");
        return NULL;
    }
    void *ptr = p->arena + p->apos;
    p->apos += aligned;
    memset(ptr, 0, aligned);
    return ptr;
}

static void jp_skip_ws(JP *p) {
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { p->pos++; continue; }
        /* C++-style line comments as a convenience extension */
        if (c == '/' && p->pos + 1 < p->len && p->src[p->pos + 1] == '/') {
            while (p->pos < p->len && p->src[p->pos] != '\n') p->pos++;
            continue;
        }
        break;
    }
}

static char jp_peek(JP *p)    { jp_skip_ws(p); return (p->pos < p->len) ? p->src[p->pos]     : '\0'; }
static char jp_consume(JP *p) { jp_skip_ws(p); return (p->pos < p->len) ? p->src[p->pos++]   : '\0'; }

static JNode *jp_node(JP *p, JNType t) {
    JNode *n = jp_alloc(p, sizeof(JNode));
    if (n) n->type = t;
    return n;
}

static JNode *jp_parse_value(JP *p);  /* forward declaration */

static char *jp_raw_string(JP *p) {
    if (jp_consume(p) != '"') {
        snprintf(p->err, sizeof(p->err), "expected '\"' at pos %d", p->pos);
        return NULL;
    }
    char tmp[512]; int ti = 0;
    while (p->pos < p->len && p->src[p->pos] != '"' && ti < 510) {
        if (p->src[p->pos] == '\\') {
            p->pos++;
            if (p->pos >= p->len) break;
            switch (p->src[p->pos++]) {
                case 'n': tmp[ti++] = '\n'; break;
                case 't': tmp[ti++] = '\t'; break;
                case 'r': tmp[ti++] = '\r'; break;
                default:  tmp[ti++] = p->src[p->pos - 1]; break;
            }
        } else { tmp[ti++] = p->src[p->pos++]; }
    }
    tmp[ti] = '\0';
    if (p->pos < p->len && p->src[p->pos] == '"') p->pos++;
    char *out = jp_alloc(p, ti + 1);
    if (out) memcpy(out, tmp, ti + 1);
    return out;
}

static JNode *jp_parse_string(JP *p) {
    JNode *n = jp_node(p, JN_STR);
    if (!n) return NULL;
    n->u.s = jp_raw_string(p);
    return n->u.s ? n : NULL;
}

static JNode *jp_parse_number(JP *p) {
    jp_skip_ws(p);
    char buf[64]; int bi = 0;
    while (p->pos < p->len && bi < 63) {
        char c = p->src[p->pos];
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
            buf[bi++] = c, p->pos++;
        else break;
    }
    buf[bi] = '\0';
    JNode *n = jp_node(p, JN_NUM);
    if (n) n->u.n = atof(buf);
    return n;
}

static JNode *jp_parse_object(JP *p) {
    if (jp_consume(p) != '{') { snprintf(p->err, sizeof(p->err), "expected '{'"); return NULL; }
    JNode *n = jp_node(p, JN_OBJ);
    if (!n) return NULL;
    char  **keys = jp_alloc(p, JP_MAX_OBJ * (int)sizeof(char *));
    JNode **vals = jp_alloc(p, JP_MAX_OBJ * (int)sizeof(JNode *));
    if (!keys || !vals) return NULL;
    n->u.obj.k = keys;
    n->u.obj.v = vals;
    int cnt = 0;
    while (jp_peek(p) != '}' && jp_peek(p) != '\0' && cnt < JP_MAX_OBJ) {
        char *key = jp_raw_string(p);
        if (!key || jp_consume(p) != ':') return NULL;
        JNode *val = jp_parse_value(p);
        if (!val) return NULL;
        keys[cnt] = key; vals[cnt] = val; cnt++;
        if (jp_peek(p) == ',') jp_consume(p);
    }
    if (jp_peek(p) == '}') jp_consume(p);
    n->u.obj.len = cnt;
    return n;
}

static JNode *jp_parse_array(JP *p) {
    if (jp_consume(p) != '[') { snprintf(p->err, sizeof(p->err), "expected '['"); return NULL; }
    JNode *n = jp_node(p, JN_ARR);
    if (!n) return NULL;
    JNode **items = jp_alloc(p, JP_MAX_ARR * (int)sizeof(JNode *));
    if (!items) return NULL;
    n->u.arr.v = items;
    int cnt = 0;
    while (jp_peek(p) != ']' && jp_peek(p) != '\0' && cnt < JP_MAX_ARR) {
        JNode *val = jp_parse_value(p);
        if (!val) return NULL;
        items[cnt++] = val;
        if (jp_peek(p) == ',') jp_consume(p);
    }
    if (jp_peek(p) == ']') jp_consume(p);
    n->u.arr.len = cnt;
    return n;
}

static JNode *jp_parse_value(JP *p) {
    char c = jp_peek(p);
    if (c == '{') return jp_parse_object(p);
    if (c == '[') return jp_parse_array(p);
    if (c == '"') return jp_parse_string(p);
    if (c == 't') { p->pos += 4; JNode *n = jp_node(p, JN_BOOL); if (n) n->u.b = 1; return n; }
    if (c == 'f') { p->pos += 5; JNode *n = jp_node(p, JN_BOOL); if (n) n->u.b = 0; return n; }
    if (c == 'n') { p->pos += 4; return jp_node(p, JN_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) return jp_parse_number(p);
    snprintf(p->err, sizeof(p->err), "unexpected char '%c' at pos %d", c, p->pos);
    return NULL;
}

/* Lookup a key in a JSON object node. */
static JNode *jp_get(JNode *obj, const char *key) {
    if (!obj || obj->type != JN_OBJ) return NULL;
    for (int i = 0; i < obj->u.obj.len; i++)
        if (obj->u.obj.k[i] && strcmp(obj->u.obj.k[i], key) == 0)
            return obj->u.obj.v[i];
    return NULL;
}

static const char *jp_str(JNode *n) { return (n && n->type == JN_STR) ? n->u.s : NULL; }
static int         jp_int(JNode *n, int def) { return (n && n->type == JN_NUM) ? (int)n->u.n : def; }

/* ═══════════════════════════════════════════════════════════════════════════
 * Spec parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

static int parse_service(JNode *sobj, const char *name, Service *svc) {
    memset(svc, 0, sizeof(*svc));
    snprintf(svc->name,    sizeof(svc->name),    "%s", name);
    snprintf(svc->image,   sizeof(svc->image),   "%s", jp_str(jp_get(sobj, "image"))   ?: "");
    snprintf(svc->command, sizeof(svc->command), "%s", jp_str(jp_get(sobj, "command")) ?: "");

    const char *rp = jp_str(jp_get(sobj, "restart"));
    if      (rp && strcmp(rp, "always")     == 0) svc->restart = RESTART_ALWAYS;
    else if (rp && strcmp(rp, "on-failure") == 0) svc->restart = RESTART_ON_FAILURE;
    else                                          svc->restart = RESTART_NEVER;

    svc->max_restarts = jp_int(jp_get(sobj, "max_restarts"), 0);

    JNode *deps = jp_get(sobj, "depends_on");
    if (deps && deps->type == JN_ARR) {
        for (int i = 0; i < deps->u.arr.len && svc->dep_count < ORCH_MAX_DEPS; i++) {
            const char *d = jp_str(deps->u.arr.v[i]);
            if (d) snprintf(svc->deps[svc->dep_count++], ORCH_SVC_NAME_LEN, "%s", d);
        }
    }

    JNode *hc = jp_get(sobj, "health_check");
    if (hc && hc->type == JN_OBJ) {
        const char *hcmd = jp_str(jp_get(hc, "exec"));
        if (hcmd && hcmd[0]) {
            snprintf(svc->health.exec, sizeof(svc->health.exec), "%s", hcmd);
            svc->health.enabled         = 1;
            svc->health.interval_ms     = jp_int(jp_get(hc, "interval_ms"),     5000);
            svc->health.timeout_ms      = jp_int(jp_get(hc, "timeout_ms"),      1000);
            svc->health.retries         = jp_int(jp_get(hc, "retries"),            3);
            svc->health.start_period_ms = jp_int(jp_get(hc, "start_period_ms"), 1000);
        }
    }
    return 0;
}

int orch_parse_spec(const char *path, OrchestratorSpec *spec) {
    FILE *f = fopen(path, "r");
    if (!f) { printf("[orch] cannot open spec '%s': %s\n\n", path, strerror(errno)); return -1; }

    static char fbuf[64 * 1024];
    int flen = (int)fread(fbuf, 1, sizeof(fbuf) - 1, f);
    fclose(f);
    fbuf[flen] = '\0';

    static JP jp;
    memset(&jp, 0, sizeof(jp));
    jp.src = fbuf; jp.len = flen;

    JNode *root = jp_parse_value(&jp);
    if (!root || root->type != JN_OBJ) {
        printf("[orch] JSON parse error: %s\n\n", jp.err[0] ? jp.err : "root must be an object");
        return -1;
    }

    memset(spec, 0, sizeof(*spec));
    snprintf(spec->spec_path, sizeof(spec->spec_path), "%s", path);
    const char *sname = jp_str(jp_get(root, "name"));
    snprintf(spec->name, sizeof(spec->name), "%s", sname ?: "unnamed");

    JNode *svcs = jp_get(root, "services");
    if (!svcs || svcs->type != JN_OBJ) {
        printf("[orch] spec missing 'services' object\n\n");
        return -1;
    }
    for (int i = 0; i < svcs->u.obj.len && spec->count < ORCH_MAX_SERVICES; i++)
        if (parse_service(svcs->u.obj.v[i], svcs->u.obj.k[i],
                          &spec->services[spec->count]) == 0)
            spec->count++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Topological sort (Kahn's algorithm)
 * ═══════════════════════════════════════════════════════════════════════════ */

static int find_svc(OrchestratorSpec *spec, const char *name) {
    for (int i = 0; i < spec->count; i++)
        if (strcmp(spec->services[i].name, name) == 0) return i;
    return -1;
}

/* Fills order[] with service indices in startup order.
 * Returns count on success, -1 if a dependency cycle exists. */
static int topo_sort(OrchestratorSpec *spec, int *order) {
    int indegree[ORCH_MAX_SERVICES] = {0};
    int queue[ORCH_MAX_SERVICES], qh = 0, qt = 0, cnt = 0;

    for (int i = 0; i < spec->count; i++)
        for (int d = 0; d < spec->services[i].dep_count; d++) {
            int dep = find_svc(spec, spec->services[i].deps[d]);
            if (dep >= 0) indegree[i]++;
        }

    for (int i = 0; i < spec->count; i++)
        if (indegree[i] == 0) queue[qt++] = i;

    while (qh < qt) {
        int cur = queue[qh++];
        order[cnt++] = cur;
        for (int i = 0; i < spec->count; i++)
            for (int d = 0; d < spec->services[i].dep_count; d++)
                if (find_svc(spec, spec->services[i].deps[d]) == cur)
                    if (--indegree[i] == 0) queue[qt++] = i;
    }
    return (cnt == spec->count) ? cnt : -1;
}

int orch_validate(OrchestratorSpec *spec) {
    int ok = 1;
    for (int i = 0; i < spec->count; i++) {
        Service *s = &spec->services[i];
        if (s->image[0] == '\0') {
            printf("[orch] service '%s': missing 'image' field\n", s->name);
            ok = 0;
        }
        if (s->command[0] == '\0') {
            printf("[orch] service '%s': missing 'command' field\n", s->name);
            ok = 0;
        }
        for (int d = 0; d < s->dep_count; d++) {
            if (find_svc(spec, s->deps[d]) < 0) {
                printf("[orch] service '%s': unknown dependency '%s'\n",
                       s->name, s->deps[d]);
                ok = 0;
            }
        }
    }
    int dummy[ORCH_MAX_SERVICES];
    if (ok && topo_sort(spec, dummy) < 0) {
        printf("[orch] dependency cycle detected\n");
        ok = 0;
    }
    return ok ? 0 : -1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Monitor thread
 *
 * Wakes every MON_TICK_MS.  Collects work (health checks to run, containers
 * to restart) while holding g_lock, then drops the lock and does the
 * blocking work, then re-acquires to update state.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MON_TICK_MS 2000

typedef struct {
    int  idx;
    int  type;           /* 0 = health check, 1 = restart */
    char cid[64];
    char hcmd[ORCH_CMD_LEN];
    char image[ORCH_IMG_LEN];
    char command[ORCH_CMD_LEN];
    char name[ORCH_SVC_NAME_LEN];
    int  backoff_s;
    int  exit_code;
} WorkItem;

static void *monitor_thread(void *arg) {
    (void)arg;
    WorkItem work[ORCH_MAX_SERVICES];

    while (1) {
        struct timespec ts = { MON_TICK_MS / 1000,
                               (long)(MON_TICK_MS % 1000) * 1000000L };
        nanosleep(&ts, NULL);

        pthread_mutex_lock(&g_lock);
        if (!g_running) { pthread_mutex_unlock(&g_lock); break; }

        int wn = 0;
        time_t now = time(NULL);

        for (int i = 0; i < g_spec.count && wn < ORCH_MAX_SERVICES; i++) {
            Service *svc = &g_spec.services[i];
            if (svc->container_id[0] == '\0') continue;
            if (svc->state == SVC_PENDING || svc->state == SVC_STARTING ||
                svc->state == SVC_RESTARTING) continue;

            ContainerState cst;
            int ec = 0;
            pid_t pid = 0;
            if (container_get_info(svc->container_id, &cst, &ec, &pid) != 0) continue;

            /* — container exited — */
            if (cst == STATE_STOPPED &&
                svc->state != SVC_STOPPED && svc->state != SVC_FAILED) {
                int do_restart = 0;
                if (svc->restart == RESTART_ALWAYS) do_restart = 1;
                else if (svc->restart == RESTART_ON_FAILURE && ec != 0) do_restart = 1;

                if (do_restart &&
                    (svc->max_restarts == 0 || svc->restart_count < svc->max_restarts)) {
                    int backoff = 1 << svc->restart_count;
                    if (backoff > 30) backoff = 30;
                    svc->state = SVC_RESTARTING;
                    WorkItem *w = &work[wn++];
                    w->type     = 1;
                    w->idx      = i;
                    w->backoff_s = backoff;
                    w->exit_code = ec;
                    snprintf(w->cid,     sizeof(w->cid),     "%s", svc->container_id);
                    snprintf(w->image,   sizeof(w->image),   "%s", svc->image);
                    snprintf(w->command, sizeof(w->command), "%s", svc->command);
                    snprintf(w->name,    sizeof(w->name),    "%s", svc->name);
                } else {
                    svc->state = (ec == 0) ? SVC_STOPPED : SVC_FAILED;
                }
                continue;
            }

            /* — health check — */
            if (cst == STATE_RUNNING && svc->health.enabled &&
                (svc->state == SVC_RUNNING || svc->state == SVC_HEALTHY ||
                 svc->state == SVC_UNHEALTHY)) {
                if ((now - svc->started_at) * 1000 < svc->health.start_period_ms) continue;
                if ((now - svc->last_health_check) * 1000 < svc->health.interval_ms) continue;
                svc->last_health_check = now;
                WorkItem *w = &work[wn++];
                w->type = 0;
                w->idx  = i;
                snprintf(w->cid,  sizeof(w->cid),  "%s", svc->container_id);
                snprintf(w->hcmd, sizeof(w->hcmd), "%s", svc->health.exec);
                snprintf(w->name, sizeof(w->name), "%s", svc->name);
            } else if (cst == STATE_RUNNING && !svc->health.enabled &&
                       svc->state == SVC_RUNNING) {
                /* No health check configured — promote to healthy immediately. */
                svc->state = SVC_HEALTHY;
                eventbus_emit(EVENT_ORCH_SVC_HEALTHY, svc->name, svc->container_id, 0);
            }
        }

        pthread_mutex_unlock(&g_lock);

        /* — execute work items without holding g_lock — */
        for (int w = 0; w < wn; w++) {
            WorkItem *wi = &work[w];

            if (wi->type == 0) {
                /* health check */
                int hc = container_exec_quiet(wi->cid, wi->hcmd);
                pthread_mutex_lock(&g_lock);
                Service *svc = &g_spec.services[wi->idx];
                if (strcmp(svc->container_id, wi->cid) != 0) {
                    pthread_mutex_unlock(&g_lock);
                    continue;
                }
                if (hc == 0) {
                    svc->health_fail_count = 0;
                    if (svc->state != SVC_HEALTHY) {
                        svc->state = SVC_HEALTHY;
                        eventbus_emit(EVENT_ORCH_SVC_HEALTHY, svc->name, svc->container_id, 0);
                    }
                } else {
                    svc->health_fail_count++;
                    if (svc->health_fail_count >= svc->health.retries) {
                        svc->state = SVC_UNHEALTHY;
                        eventbus_emit(EVENT_ORCH_SVC_UNHEALTHY, svc->name, svc->container_id,
                                      (long)svc->health_fail_count);
                    }
                }
                pthread_mutex_unlock(&g_lock);

            } else {
                /* restart with exponential backoff */
                sleep((unsigned)wi->backoff_s);

                container_delete(wi->cid);

                char resolved[512];
                const char *rootfs;
                if (image_resolve(wi->image, resolved, sizeof(resolved)) == 0)
                    rootfs = resolved;
                else
                    rootfs = wi->image;

                ContainerSpec cs;
                memset(&cs, 0, sizeof(cs));
                cs.name         = wi->name;
                cs.hostname     = wi->name;
                cs.rootfs       = rootfs;
                cs.command_line = wi->command;

                char new_id[64] = {0};
                int started = (container_run_background(&cs, new_id, sizeof(new_id)) == 0);

                pthread_mutex_lock(&g_lock);
                Service *svc = &g_spec.services[wi->idx];
                if (started) {
                    snprintf(svc->container_id, sizeof(svc->container_id), "%s", new_id);
                    svc->state             = SVC_RUNNING;
                    svc->started_at        = time(NULL);
                    svc->health_fail_count = 0;
                    svc->restart_count++;
                    eventbus_emit(EVENT_ORCH_SVC_RESTARTED, svc->name, new_id,
                                  (long)svc->restart_count);
                    log_event("[orch] %s restarted (attempt %d, backoff %ds)",
                              svc->name, svc->restart_count, wi->backoff_s);
                } else {
                    svc->state = SVC_FAILED;
                }
                pthread_mutex_unlock(&g_lock);
            }
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * orch_run / orch_down / orch_restart_service
 * ═══════════════════════════════════════════════════════════════════════════ */

int orch_run(OrchestratorSpec *spec) {
    pthread_mutex_lock(&g_lock);
    if (g_running) {
        printf("[orch] a spec is already running — run 'orch down' first\n\n");
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    pthread_mutex_unlock(&g_lock);

    if (orch_validate(spec) != 0) return -1;

    int order[ORCH_MAX_SERVICES];
    int n = topo_sort(spec, order);
    if (n < 0) { printf("[orch] internal: topo_sort failed\n\n"); return -1; }

    pthread_mutex_lock(&g_lock);
    g_spec = *spec;
    pthread_mutex_unlock(&g_lock);

    printf("[orch] starting spec '%s' (%d service%s)\n",
           spec->name, spec->count, spec->count == 1 ? "" : "s");

    for (int oi = 0; oi < n; oi++) {
        int si = order[oi];

        pthread_mutex_lock(&g_lock);
        Service *svc = &g_spec.services[si];
        svc->state = SVC_STARTING;
        char img[ORCH_IMG_LEN], cmd[ORCH_CMD_LEN], sname[ORCH_SVC_NAME_LEN];
        snprintf(img,   sizeof(img),   "%s", svc->image);
        snprintf(cmd,   sizeof(cmd),   "%s", svc->command);
        snprintf(sname, sizeof(sname), "%s", svc->name);
        pthread_mutex_unlock(&g_lock);

        char resolved[512];
        const char *rootfs;
        if (image_resolve(img, resolved, sizeof(resolved)) == 0)
            rootfs = resolved;
        else
            rootfs = img;

        ContainerSpec cs;
        memset(&cs, 0, sizeof(cs));
        cs.name         = sname;
        cs.hostname     = sname;
        cs.rootfs       = rootfs;
        cs.command_line = cmd;

        char cid[64] = {0};
        printf("  [%-18s] starting…", sname);
        fflush(stdout);

        if (container_run_background(&cs, cid, sizeof(cid)) != 0) {
            printf(" FAILED\n");
            pthread_mutex_lock(&g_lock);
            g_spec.services[si].state = SVC_FAILED;
            pthread_mutex_unlock(&g_lock);
            continue;
        }

        printf(" up  id=%s\n", cid);

        pthread_mutex_lock(&g_lock);
        Service *sv2 = &g_spec.services[si];
        snprintf(sv2->container_id, sizeof(sv2->container_id), "%s", cid);
        sv2->state      = SVC_RUNNING;
        sv2->started_at = time(NULL);
        pthread_mutex_unlock(&g_lock);

        eventbus_emit(EVENT_ORCH_SVC_STARTED, sname, cid, 0);
        usleep(150000);  /* 150 ms: gives the container time to reach RUNNING state */
    }

    pthread_mutex_lock(&g_lock);
    g_running = 1;
    pthread_mutex_unlock(&g_lock);

    if (pthread_create(&g_mon_tid, NULL, monitor_thread, NULL) != 0) {
        printf("[orch] warning: monitor thread failed to start\n");
    }

    eventbus_emit(EVENT_ORCH_SPEC_UP, spec->name, spec->spec_path, (long)spec->count);
    printf("\n[orch] '%s' is up.  Use 'orch status' to check health.\n\n", spec->name);
    return 0;
}

void orch_down(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        printf("[orch] no spec is currently running\n\n");
        pthread_mutex_unlock(&g_lock);
        return;
    }
    g_running = 0;
    char spec_name[ORCH_SVC_NAME_LEN];
    snprintf(spec_name, sizeof(spec_name), "%s", g_spec.name);
    pthread_mutex_unlock(&g_lock);

    if (g_mon_tid) { pthread_join(g_mon_tid, NULL); g_mon_tid = 0; }

    int order[ORCH_MAX_SERVICES];
    int n = topo_sort(&g_spec, order);
    if (n < 0) n = g_spec.count;

    printf("[orch] stopping spec '%s'…\n", spec_name);
    for (int oi = n - 1; oi >= 0; oi--) {
        Service *svc = &g_spec.services[order[oi]];
        if (svc->container_id[0]) {
            printf("  [%-18s] stopping…\n", svc->name);
            container_stop(svc->container_id, 5);
            container_delete(svc->container_id);
            svc->state = SVC_STOPPED;
        }
    }

    eventbus_emit(EVENT_ORCH_SPEC_DOWN, spec_name, "", 0);
    printf("[orch] spec '%s' is down.\n\n", spec_name);
}

int orch_restart_service(const char *name) {
    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        printf("[orch] no spec running\n\n");
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    int idx = -1;
    for (int i = 0; i < g_spec.count; i++)
        if (strcmp(g_spec.services[i].name, name) == 0) { idx = i; break; }
    if (idx < 0) {
        printf("[orch] service '%s' not found in active spec\n\n", name);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    Service *svc = &g_spec.services[idx];
    char old_id[64], img[ORCH_IMG_LEN], cmd[ORCH_CMD_LEN], sname[ORCH_SVC_NAME_LEN];
    snprintf(old_id, sizeof(old_id), "%s", svc->container_id);
    snprintf(img,    sizeof(img),    "%s", svc->image);
    snprintf(cmd,    sizeof(cmd),    "%s", svc->command);
    snprintf(sname,  sizeof(sname),  "%s", svc->name);
    svc->state = SVC_RESTARTING;
    pthread_mutex_unlock(&g_lock);

    container_stop(old_id, 5);
    container_delete(old_id);

    char resolved[512];
    const char *rootfs;
    if (image_resolve(img, resolved, sizeof(resolved)) == 0)
        rootfs = resolved;
    else
        rootfs = img;

    ContainerSpec cs;
    memset(&cs, 0, sizeof(cs));
    cs.name = cs.hostname = sname;
    cs.rootfs       = rootfs;
    cs.command_line = cmd;

    char new_id[64] = {0};
    if (container_run_background(&cs, new_id, sizeof(new_id)) != 0) {
        printf("[orch] failed to restart service '%s'\n\n", name);
        pthread_mutex_lock(&g_lock);
        g_spec.services[idx].state = SVC_FAILED;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    pthread_mutex_lock(&g_lock);
    svc = &g_spec.services[idx];
    snprintf(svc->container_id, sizeof(svc->container_id), "%s", new_id);
    svc->state             = SVC_RUNNING;
    svc->started_at        = time(NULL);
    svc->health_fail_count = 0;
    pthread_mutex_unlock(&g_lock);

    eventbus_emit(EVENT_ORCH_SVC_RESTARTED, name, new_id, 0);
    printf("[orch] service '%s' restarted  id=%s\n\n", name, new_id);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Status display
 * ═══════════════════════════════════════════════════════════════════════════ */

static const char *svc_state_str(ServiceState s) {
    switch (s) {
        case SVC_PENDING:    return "pending";
        case SVC_STARTING:   return "starting";
        case SVC_RUNNING:    return "running";
        case SVC_HEALTHY:    return "healthy";
        case SVC_UNHEALTHY:  return "unhealthy";
        case SVC_STOPPED:    return "stopped";
        case SVC_FAILED:     return "failed";
        case SVC_RESTARTING: return "restarting";
        default:             return "unknown";
    }
}

static const char *health_icon(ServiceState s) {
    switch (s) {
        case SVC_HEALTHY:    return "✓  healthy";
        case SVC_UNHEALTHY:  return "✗  unhealthy";
        case SVC_RUNNING:    return "?  checking";
        case SVC_FAILED:     return "✗  failed";
        case SVC_RESTARTING: return "↺  restarting";
        case SVC_STOPPED:    return "-  stopped";
        default:             return "-  -";
    }
}

void orch_status(void) {
    pthread_mutex_lock(&g_lock);
    if (!g_running) {
        printf("  (no spec running — use 'orch run <spec.json>')\n\n");
        pthread_mutex_unlock(&g_lock);
        return;
    }

    printf("\n  Spec : %s\n", g_spec.name);
    printf("  File : %s\n\n", g_spec.spec_path);
    printf("  %-20s  %-12s  %-14s  %5s  %s\n",
           "SERVICE", "STATUS", "HEALTH", "RST", "CONTAINER-ID");
    printf("  %s\n",
           "────────────────────────────────────────────────────────────────────────");
    for (int i = 0; i < g_spec.count; i++) {
        Service *s = &g_spec.services[i];
        printf("  %-20s  %-12s  %-14s  %5d  %s\n",
               s->name,
               svc_state_str(s->state),
               health_icon(s->state),
               s->restart_count,
               s->container_id[0] ? s->container_id : "-");
    }
    printf("\n");
    pthread_mutex_unlock(&g_lock);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Dependency graph printer
 * ═══════════════════════════════════════════════════════════════════════════ */

void orch_print_graph(OrchestratorSpec *spec) {
    printf("\n  Dependency graph for '%s':\n\n", spec->name);
    for (int i = 0; i < spec->count; i++) {
        Service *s = &spec->services[i];
        if (s->dep_count == 0) {
            printf("    [%s]  (root — no dependencies)\n", s->name);
        } else {
            printf("    [%s]  depends on: ", s->name);
            for (int d = 0; d < s->dep_count; d++)
                printf("[%s]%s", s->deps[d], d < s->dep_count - 1 ? ", " : "");
            printf("\n");
        }
    }

    int order[ORCH_MAX_SERVICES];
    int n = topo_sort(spec, order);
    if (n < 0) { printf("\n  *** dependency cycle detected ***\n\n"); return; }

    printf("\n  Startup order: ");
    for (int i = 0; i < n; i++)
        printf("%s%s", spec->services[order[i]].name, i < n - 1 ? " → " : "");
    printf("\n\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Command dispatcher
 * ═══════════════════════════════════════════════════════════════════════════ */

int cmd_orch(int argc, char **argv) {
    if (argc < 2) {
        printf("  Usage: orch <subcommand> [args]\n\n");
        printf("  Subcommands:\n");
        printf("    run <spec.json>          parse spec and start all services\n");
        printf("    down                     graceful stop in reverse dep-order\n");
        printf("    status | ps              live service health table\n");
        printf("    restart <service>        restart a single named service\n");
        printf("    validate <spec.json>     validate spec without starting\n");
        printf("    graph <spec.json>        print dependency graph + startup order\n\n");
        return 0;
    }

    const char *sub = argv[1];

    if (strcmp(sub, "run") == 0) {
        if (argc < 3) { printf("[orch] usage: orch run <spec.json>\n\n"); return -1; }
        OrchestratorSpec spec;
        if (orch_parse_spec(argv[2], &spec) != 0) return -1;
        return orch_run(&spec);
    }

    if (strcmp(sub, "down") == 0) { orch_down(); return 0; }

    if (strcmp(sub, "status") == 0 || strcmp(sub, "ps") == 0) { orch_status(); return 0; }

    if (strcmp(sub, "restart") == 0) {
        if (argc < 3) { printf("[orch] usage: orch restart <service-name>\n\n"); return -1; }
        return orch_restart_service(argv[2]);
    }

    if (strcmp(sub, "validate") == 0) {
        if (argc < 3) { printf("[orch] usage: orch validate <spec.json>\n\n"); return -1; }
        OrchestratorSpec spec;
        if (orch_parse_spec(argv[2], &spec) != 0) return -1;
        if (orch_validate(&spec) != 0) return -1;
        int order[ORCH_MAX_SERVICES];
        int n = topo_sort(&spec, order);
        printf("[orch] '%s' is valid (%d service%s).  Startup order: ",
               spec.name, spec.count, spec.count == 1 ? "" : "s");
        for (int i = 0; i < n; i++)
            printf("%s%s", spec.services[order[i]].name, i < n - 1 ? " → " : "");
        printf("\n\n");
        return 0;
    }

    if (strcmp(sub, "graph") == 0) {
        if (argc < 3) { printf("[orch] usage: orch graph <spec.json>\n\n"); return -1; }
        OrchestratorSpec spec;
        if (orch_parse_spec(argv[2], &spec) != 0) return -1;
        orch_print_graph(&spec);
        return 0;
    }

    printf("[orch] unknown subcommand '%s' — try 'orch' for help\n\n", sub);
    return -1;
}
