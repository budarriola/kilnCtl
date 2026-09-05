// wifi_provision_state.h -- the Wi-Fi-provisioning accessors non-http-tier
// callers are allowed to depend on, split out of wifi_provision_http.h (an
// http-layer header) per docs/HW_ABSTRACTION_PLAN.md's "drivers/ layering"
// item 9 -- factory_reset.c [persist] and wifi_prov.c [net] only ever
// needed one accessor apiece, not each other's half of the httpd-handler
// surface. wifi_provision_http.h includes this header so existing callers
// are unaffected; the implementation stays in wifi_provision_http.c, which
// already owns the httpd-server-instance state these read/start.
#ifndef WIFI_PROVISION_STATE_H
#define WIFI_PROVISION_STATE_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the HTTP server and registers the provisioning routes. Safe to call
 * regardless of current Wi-Fi mode -- esp_http_server listens on every
 * netif, so this works whether the board is currently reachable over the AP,
 * the station link, or both. wifi_prov.c is the non-httpd-tier caller. */
esp_err_t wifi_provision_http_start(void);

/* The server started above, for other modules (dashboard_http.c,
 * factory_reset.c, sim_backend.c) to register their own routes on -- one
 * httpd instance for the whole board, not one per feature. NULL if
 * wifi_provision_http_start() hasn't succeeded yet. */
httpd_handle_t wifi_provision_http_get_server(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISION_STATE_H
