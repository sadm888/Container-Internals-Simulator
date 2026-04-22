#ifndef WEBSERVER_H
#define WEBSERVER_H

/*
 * Minimal embedded HTTP/1.0 server exposing a JSON REST API and a
 * single-page web dashboard.  Runs on a dedicated pthread; thread-safe
 * reads of container state and events are performed from handler threads.
 *
 * Endpoints
 *   GET  /                            → web/index.html
 *   GET  /api/containers              → JSON array of all containers
 *   GET  /api/events[?n=N]            → JSON array of last N events (default 50)
 *   GET  /api/metrics                 → JSON object of aggregated metrics
 *   POST /api/containers/:id/stop     → send SIGTERM to container (202)
 */

int  webserver_start(int port);  /* returns 0 on success, -1 on error */
void webserver_stop(void);

#endif
