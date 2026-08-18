#include "wifi_status_ui.h"

#include <stdio.h>

#include "mdns.h"

#include "wifi_prov.h"

// See wifi_status_ui.h's header comment: this is ui_page_home.c's original
// wifi_status_text() (added the same pass as the status-bar readout it
// serves), relocated here verbatim (TODO.md 10.9's explicit ask) so
// ui_page_network.c can call the exact same formatter instead of a second
// copy of this switch statement. Behavior is unchanged from the original.

void wifi_status_ui_get_text(char *out, size_t out_cap)
{
    if (wifi_prov_get_mode() == WIFI_PROV_MODE_AP) {
        snprintf(out, out_cap, "WiFi: AP mode");
        return;
    }

    switch (wifi_prov_get_state()) {
    case WIFI_PROV_STATE_CONNECTED: {
        char ip[16];
        if (wifi_prov_get_sta_ip(ip, sizeof(ip)) != ESP_OK) {
            ip[0] = '\0';
        }
        char mdns_host[MDNS_NAME_BUF_LEN];
        if (mdns_hostname_get(mdns_host) == ESP_OK) {
            snprintf(out, out_cap, "WiFi: %s (%s.local)", ip, mdns_host);
        } else {
            snprintf(out, out_cap, "WiFi: %s", ip);
        }
        break;
    }
    case WIFI_PROV_STATE_CONNECTING:
        snprintf(out, out_cap, "WiFi: connecting...");
        break;
    case WIFI_PROV_STATE_RECONNECTING:
        snprintf(out, out_cap, "WiFi: reconnecting...");
        break;
    case WIFI_PROV_STATE_UNPROVISIONED:
        snprintf(out, out_cap, "WiFi: not set up (AP fallback)");
        break;
    case WIFI_PROV_STATE_AP_MODE:
        snprintf(out, out_cap, "WiFi: AP mode");
        break;
    default:
        snprintf(out, out_cap, "WiFi: unknown");
        break;
    }
}
