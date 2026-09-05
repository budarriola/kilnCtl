// wifi_provision_state.h -- the one Wi-Fi-provisioning accessor non-http-tier
// callers are allowed to depend on, split out of wifi_provision_http.h (an
// http-layer header) per docs/HW_ABSTRACTION_PLAN.md's "drivers/ layering"
// item 10 -- wifi_prov.c [net] only ever needed this one accessor, not the
// rest of the httpd-handler surface, and unlike wifi_provision_http_get_
// server() (which stays in wifi_provision_http.h -- see that header's
// comment), this one returns no httpd type, so it is the one call that can
// actually drop the esp_http_server.h dependency for its caller.
// wifi_provision_http.h includes this header so existing callers are
// unaffected; the implementation stays in wifi_provision_http.c, which
// already owns the httpd-server-instance state this starts.
#ifndef WIFI_PROVISION_STATE_H
#define WIFI_PROVISION_STATE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the HTTP server and registers the provisioning routes. Safe to call
 * regardless of current Wi-Fi mode -- esp_http_server listens on every
 * netif, so this works whether the board is currently reachable over the AP,
 * the station link, or both. wifi_prov.c is the non-httpd-tier caller. */
esp_err_t wifi_provision_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISION_STATE_H
