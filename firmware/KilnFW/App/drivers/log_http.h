// log_http -- read-back HTTP endpoint for the on-flash firing/autotune log
// store (log_store.c/log_store_mount.c). New file, not dashboard_http.c: at
// the time this was added, dashboard_http.c/app.js/main_page.html were owned
// by parallel work, and this endpoint has no dependency on any of those
// files -- same "own file, register on the shared httpd instance" shape as
// readiness_http.c/diagnostics_http.c already use.
#ifndef LOG_HTTP_H
#define LOG_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers GET /api/logs/firing and GET /api/logs/autotune on the shared
 * httpd instance (wifi_provision_http_get_server() -- must already be
 * running, same precondition as readiness_http_start()/diagnostics_http_
 * start()). Each streams every retained line for that kind, oldest first,
 * one per line, as text/plain -- the exact lines log_store_reader_next()
 * returns, which are themselves the exact lines telemetry_format_firing()/
 * telemetry_format_autotune() produced (same format used over the debug
 * UART, not a second format). */
esp_err_t log_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // LOG_HTTP_H
