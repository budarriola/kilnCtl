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
