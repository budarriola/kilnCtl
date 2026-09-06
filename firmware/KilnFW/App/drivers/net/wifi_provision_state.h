// wifi_provision_state.h -- the one Wi-Fi-provisioning accessor non-http-tier
// callers are allowed to depend on, split out of wifi_provision_http.h (an
// http-layer header) per docs/HW_ABSTRACTION.md's "drivers/ layering"
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

/* Item 13 (docs/HW_ABSTRACTION.md "drivers/ layering"): the mid-tier
 * accessor for the shared httpd instance wifi_provision_http_start() brings
 * up. Returns it as a plain `void *` -- IDF's esp_http_server.h defines
 * `typedef void *httpd_handle_t;`, and a second, compatible typedef of the
 * same name here would conflict wherever both headers end up included, so
 * this header stays free of esp_http_server.h entirely. Callers that already
 * have (or want) the httpd_handle_t spelling should use
 * wifi_provision_http_get_server() (wifi_provision_http.h) instead, which is
 * a thin alias over this one; callers that only need to pass the handle
 * through opaquely (sim_backend.c registering routes) can use this and skip
 * the esp_http_server.h dependency. NULL if wifi_provision_http_start()
 * hasn't succeeded yet. */
void *wifi_provision_get_httpd_handle(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISION_STATE_H
