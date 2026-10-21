// diag_bundle_http.h -- GET /api/diag/bundle (FD-01). See diag_bundle_http.c.
#ifndef DIAG_BUNDLE_HTTP_H
#define DIAG_BUNDLE_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the route on the shared httpd (server_handle is an httpd_handle_t).
 * Called from diagnostics_http_start(). */
esp_err_t diag_bundle_http_register(void *server_handle);

#ifdef __cplusplus
}
#endif

#endif
