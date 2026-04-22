#ifndef ORCHESTRATOR_H
#define ORCHESTRATOR_H

#include <time.h>
#include <sys/types.h>

/*
 * Module 12 — Orchestrator
 *
 * Manages multi-container applications described by a JSON spec file.
 * Features: dependency-ordered startup (topological sort), exec-based
 * health checks, restart policies with exponential backoff, graceful
 * ordered shutdown, and a background monitor thread.
 *
 * Spec format (JSON):
 *   {
 *     "name": "myapp",
 *     "services": {
 *       "db":  { "image": "base", "command": "...", "restart": "always",
 *                "health_check": { "exec": "/bin/true",
 *                                  "interval_ms": 5000, "retries": 3 } },
 *       "web": { "image": "base", "command": "...", "restart": "on-failure",
 *                "depends_on": ["db"] }
 *     }
 *   }
 */

#define ORCH_MAX_SERVICES   32
#define ORCH_MAX_DEPS        8
#define ORCH_SVC_NAME_LEN   64
#define ORCH_IMG_LEN        64
#define ORCH_CMD_LEN       256

typedef enum {
    RESTART_NEVER = 0,
    RESTART_ON_FAILURE,
    RESTART_ALWAYS,
} RestartPolicy;

typedef struct {
    char exec[ORCH_CMD_LEN];
    int  interval_ms;     /* how often to run the check  (default 5000) */
    int  timeout_ms;      /* max wait per check          (default 1000) */
    int  retries;         /* consecutive fails → unhealthy (default 3)  */
    int  start_period_ms; /* grace window before checks begin (def 1000)*/
    int  enabled;
} HealthCheck;

typedef enum {
    SVC_PENDING = 0,
    SVC_STARTING,
    SVC_RUNNING,
    SVC_HEALTHY,
    SVC_UNHEALTHY,
    SVC_STOPPED,
    SVC_FAILED,
    SVC_RESTARTING,
} ServiceState;

typedef struct {
    char          name[ORCH_SVC_NAME_LEN];
    char          image[ORCH_IMG_LEN];
    char          command[ORCH_CMD_LEN];
    RestartPolicy restart;
    int           max_restarts;     /* 0 = unlimited */
    HealthCheck   health;
    char          deps[ORCH_MAX_DEPS][ORCH_SVC_NAME_LEN];
    int           dep_count;

    /* runtime — managed by orchestrator, not persisted */
    char          container_id[64];
    ServiceState  state;
    int           restart_count;
    int           health_fail_count;
    time_t        started_at;
    time_t        last_health_check;
} Service;

typedef struct {
    char    name[ORCH_SVC_NAME_LEN];
    char    spec_path[256];
    Service services[ORCH_MAX_SERVICES];
    int     count;
} OrchestratorSpec;

/* Parse a JSON spec file. Returns 0 on success. */
int  orch_parse_spec(const char *path, OrchestratorSpec *spec);

/* Validate: resolve dep names, detect cycles. Returns 0 if valid. */
int  orch_validate(OrchestratorSpec *spec);

/* Start all services in dependency order. Launches monitor thread. */
int  orch_run(OrchestratorSpec *spec);

/* Stop all services in reverse order and join monitor thread. */
void orch_down(void);

/* Restart a single named service in the active spec. */
int  orch_restart_service(const char *name);

/* Print a live service-health table. */
void orch_status(void);

/* Print the dependency graph and computed startup order. */
void orch_print_graph(OrchestratorSpec *spec);

/* Top-level CLI dispatcher for the "orch" command. */
int  cmd_orch(int argc, char **argv);

#endif
