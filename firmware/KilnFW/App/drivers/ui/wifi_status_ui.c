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
        /* Non-blocking cached read (2026-09-25 LCD freeze follow-up) --
         * this formatter is called every tick from ui_home_refresh_cb's
         * never-deleted LVGL timer (ui_page_home.c) and from
         * ui_page_network.c, both on lvgl_port_task. The direct
         * wifi_prov_get_sta_ip() round-trips through the owner-task queue
         * and can block the caller up to WIFI_OWNER_WAIT_MS (12s) behind a
         * scan/connect already in flight -- exactly the freeze class fixed
         * for the saved-networks list. wifi_prov_get_cached_sta_ip_netmask()
         * never blocks and never touches the owner queue. */
        char ip[16];
        char netmask[16];
        if (wifi_prov_get_cached_sta_ip_netmask(ip, sizeof(ip), netmask, sizeof(netmask)) != ESP_OK) {
            ip[0] = '\0';
        }
        char mdns_host[MDNS_NAME_BUF_LEN];
        /* 2026-09-28 owner request follow-up: while the fallback AP is being
         * kept up deliberately (a logged-in user, or an AP client with auth
         * off), say so -- otherwise the LCD reads plain "connected" while a
         * second radio interface a bench tech might not expect is still
         * live. Cheap direct-read bool, checked last so the common case
         * (no pending teardown) costs nothing extra. */
        const char *ap_note = wifi_prov_get_ap_pending_teardown() ? " [AP kept up]" : "";
        if (mdns_hostname_get(mdns_host) == ESP_OK) {
            snprintf(out, out_cap, "WiFi: %s (%s.local)%s", ip, mdns_host, ap_note);
        } else {
            snprintf(out, out_cap, "WiFi: %s%s", ip, ap_note);
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
