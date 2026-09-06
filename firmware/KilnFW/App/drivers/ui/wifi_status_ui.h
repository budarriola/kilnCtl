// wifi_status_ui -- the one "human-readable Wi-Fi state" text formatter,
// shared between ui_page_home.c's status-bar readout and ui_page_network.c
// (TODO.md 10.9's explicit ask: factor this out of ui_page_home.c rather
// than keep a second copy of the same switch statement). Every value it
// formats comes from the SAME plain-C getters wifi_provision_http.c's
// status_get_handler() already calls (wifi_prov_get_mode()/_get_state()/
// _get_sta_ip(), wifi_prov.h) plus the espressif mdns component's own
// mdns_hostname_get() -- TODO.md 10.1a's shared-backend rule, no parallel
// read of wifi_prov.c's internals.
//
// A small standalone module rather than folded into ui_theme.c/.h: this is
// Wi-Fi-specific formatting logic (it includes wifi_prov.h and mdns.h),
// while ui_theme.c/.h is deliberately generic (palette/spacing constants,
// the touch hit-area helper) and knows nothing about any particular
// subsystem -- keeping this here matches the codebase's existing
// one-concern-per-module convention (e.g. board_temps.c's getter living
// next to the sensor code it reads, not folded into a generic "helpers"
// file).
#ifndef WIFI_STATUS_UI_H
#define WIFI_STATUS_UI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Formats a one-line human-readable Wi-Fi status string into out (NUL
 * terminated, truncated to out_cap if needed) -- mode==AP mirrors
 * wifi_provision_http.c's mode_name()/state_name() mapping (AP-fallback is
 * reported as its own case, not folded into "disconnected") but in short
 * human text instead of the JSON tokens the HTTP status endpoint sends.
 * Safe to call every UI refresh tick -- every value it reads is a cheap
 * plain-C getter, not a device transaction. */
void wifi_status_ui_get_text(char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif

#endif // WIFI_STATUS_UI_H
