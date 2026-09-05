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
// wifi_provision_http_start()/wifi_provision_http_get_server() moved to
// wifi_provision_state.h (docs/HW_ABSTRACTION_PLAN.md "drivers/ layering"
// item 9) -- factory_reset.c and wifi_prov.c only ever needed one accessor
// apiece, not the whole httpd-handler header. Included below so existing
// callers of this header are unaffected; the implementation stays in
// wifi_provision_http.c.
#ifndef WIFI_PROVISION_HTTP_H
#define WIFI_PROVISION_HTTP_H

#include "esp_err.h"
#include "esp_http_server.h"

#include "wifi_provision_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif // WIFI_PROVISION_HTTP_H
