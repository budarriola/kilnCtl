// wifi_provision_http -- the HTTP side of Wi-Fi provisioning: a static page
// served from the fallback AP (and from the station link too, once joined --
// nothing here cares which interface a client arrived on) plus two small
// endpoints wifi_prov.c's state feeds.
//
// Per TODO.md section 1's hard requirement, every handler here treats the
// client as actively hostile: bounded reads, a body-size cap enforced from
// Content-Length before a single byte is read, no allocation sized from
// anything the client sent.  A malformed request gets a 4xx, not a crash.
//
// wifi_provision_http_start() moved to wifi_provision_state.h
// (docs/HW_ABSTRACTION.md "drivers/ layering" item 10) -- wifi_prov.c
// only ever needed that one accessor, not the whole httpd-handler header.
// Included below so existing callers of this header are unaffected; the
// implementation stays in wifi_provision_http.c.
//
// wifi_provision_http_get_server() stays HERE, not in wifi_provision_state.h:
// it returns an httpd_handle_t, so every caller already needs
// esp_http_server.h regardless -- unlike wifi_provision_http_start(), moving
// it would not have removed anyone's httpd dependency.
//
// Item 13: the underlying state (the httpd instance pointer) is now
// exposed untyped via wifi_provision_state.h's
// wifi_provision_get_httpd_handle() -- see that header's comment -- for
// callers that don't otherwise need esp_http_server.h. This declaration
// stays a plain extern (not `static inline`): several host tests provide
// their own definition of this exact symbol in place of linking
// wifi_provision_http.c, so a body here would collide with theirs. The .c
// file's definition is now a one-line cast over
// wifi_provision_get_httpd_handle().
#ifndef WIFI_PROVISION_HTTP_H
#define WIFI_PROVISION_HTTP_H

#include "esp_err.h"
#include "esp_http_server.h"

#include "wifi_provision_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The server wifi_provision_http_start() (wifi_provision_state.h) brings up,
 * for other modules (dashboard_http.c, factory_reset.c, sim_backend.c) to
 * register their own routes on -- one httpd instance for the whole board,
 * not one per feature. NULL if wifi_provision_http_start() hasn't succeeded
 * yet. */
httpd_handle_t wifi_provision_http_get_server(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISION_HTTP_H
