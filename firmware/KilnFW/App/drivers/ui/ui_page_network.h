// The "network" page -- TODO.md 10.9's LCD-side Wi-Fi settings page, linked
// from ui_page_config.c's Configuration nav hub. New this pass; mirrors what
// wifi_provision_page.html already does on the web (mode toggle, scan,
// connect, saved-network list with forget, AP identity display) plus a QR
// code for phone-based join, per 10.9's own reasoning that the LCD is
// reachable with zero network connectivity at all -- exactly the
// bootstrapping problem a QR code solves.
//
// Per TODO.md 10.1a's shared-backend rule, every control on this page calls
// the exact same wifi_prov.h getters/setters wifi_provision_http.c's
// handlers already call -- see ui_page_network.c's header comment for the
// full list. Deliberately its own page, not folded into ui_page_config.c's
// hub itself -- same "one page one file" reasoning as ui_page_board_health.h,
// and this page is real scope (scan/connect/forget/QR), not a one-line nav
// stub.
#ifndef UI_PAGE_NETWORK_H
#define UI_PAGE_NETWORK_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first kiln_ui_show("network"). */
lv_obj_t *ui_page_network_build(void);

/* Closes the AP edit modal and clears typed SSID/password; called on the LCD relock edge. */
void ui_page_network_relock_close(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_NETWORK_H
