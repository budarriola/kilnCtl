// wifi_prov_unprovisioned_stub.c -- host-test-only stand-in for
// wifi_prov_is_unprovisioned() (drivers/net/wifi_prov.c), for every host
// executable that links http_auth_policy_iface.c (which now calls this
// function, owner decision 2026-09-28: ROUTE_TIER_WIFI_SETUP) but has no
// other reason to build the real wifi_prov.c -- that file is a full ESP-IDF
// Wi-Fi driver with no host-portable path, same class of gap
// http_auth_link_stub.c already covers for two other ESP-only symbols.
//
// Only test_wifi_prov.c's own executable links the REAL wifi_prov.c (via its
// own #include, same convention as test_ota_http.c's #include of
// ota_http.c) and so already defines the real symbol; this stub must never
// be linked alongside that file, or the two definitions collide.
//
// Returns false (i.e. "provisioned") unconditionally: every executable that
// links this stub has no Wi-Fi state of its own to report, and false is the
// fail-closed answer for http_auth_check()'s ROUTE_TIER_WIFI_SETUP case --
// it makes /wifi, /networks and /scan gate like ROUTE_TIER_ADMIN rather than
// staying open, which is the safer default absent a real provisioning state.
#include <stdbool.h>

bool wifi_prov_is_unprovisioned(void) {
    return false;
}

// 2026-09-29: same reasoning as above, for wifi_prov_request_arrived_on_ap()
// (drivers/net/wifi_prov_link.c) -- http_auth_http.c now calls it to tag
// each session touch with whether the request arrived over the SoftAP
// interface (see http_session_iface.h's http_auth_session_touch() doc
// comment). Every executable that links this stub has no real socket to
// inspect, so it reports false ("not AP", i.e. treat as an ordinary LAN/STA
// request) -- the fail-closed direction for THIS call site, since the
// caller only uses the result to tag a session as AP-origin, and understating
// that (never over-tagging a LAN request as AP) is the safer default absent
// a real socket to check.
bool wifi_prov_request_arrived_on_ap(int sockfd) {
    (void)sockfd;
    return false;
}

// F4 (WEB_UI_XSS_AUDIT_2026-10-09): http_auth_http.c reads the mDNS hostname for its
// Host allow-list. Every executable that links this stub reports the boot name.
#include <string.h>
#include "mdns.h"
esp_err_t mdns_hostname_get(char *hostname) {
    strcpy(hostname, "kilnctl");
    return ESP_OK;
}
