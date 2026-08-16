// wifi_provision_http -- the HTTP side of Wi-Fi provisioning: a static page
// served from the fallback AP (and from the station link too, once joined --
// nothing here cares which interface a client arrived on) plus two small
// endpoints wifi_prov.c's state feeds.
//
// Per TODO.md section 1's hard requirement, every handler here treats the
// client as actively hostile: bounded reads, a body-size cap enforced from
// Content-Length before a single byte is read, no allocation sized from
// anything the client sent. A malformed request gets a 4xx, not a crash.
#ifndef WIFI_PROVISION_HTTP_H
#define WIFI_PROVISION_HTTP_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the HTTP server and registers the provisioning routes. Safe to call
 * regardless of current Wi-Fi mode -- esp_http_server listens on every
 * netif, so this works whether the board is currently reachable over the AP,
 * the station link, or both. */
esp_err_t wifi_provision_http_start(void);

/* The server started above, for other modules (dashboard_http.c) to
 * register their own routes on -- one httpd instance for the whole board,
 * not one per feature. NULL if wifi_provision_http_start() hasn't
 * succeeded yet. */
httpd_handle_t wifi_provision_http_get_server(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISION_HTTP_H
