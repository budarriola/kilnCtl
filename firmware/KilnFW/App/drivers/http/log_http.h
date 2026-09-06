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
 * start()). Each streams every retained record for that kind, oldest
 * first, back-to-back with no delimiter, as application/octet-stream --
 * raw event_log.h binary records exactly as log_store_reader_next() returns
 * them. Decode with tools/PcTools/src/kilnctrl/event_log_decoder.py; there
 * is no text/plain form any more (event_log.h's file banner: flash holds
 * binary event records, not the old per-tick text lines). */
esp_err_t log_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // LOG_HTTP_H
