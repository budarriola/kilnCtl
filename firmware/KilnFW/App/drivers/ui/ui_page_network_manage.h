// "network_manage" -- Scan/Saved network list sub-page, split out of
// ui_page_network.c 2026-08-24 (TODO.md's "ui_page_network.c's worst-case
// fit is ~268px against a ~264px budget -- already over" item).
//
// ui_page_network.c's own arithmetic had the Scan/Saved list block (toggle
// row + Scan button/status + one 70px list) landing right at the edge of
// this codebase's no-scroll content budget even before accounting for the
// "Change network"/"Show QR" button being visible at the SAME time as that
// list block once connected (a real, reachable state the earlier arithmetic
// never summed in) -- see ui_theme.h's UI_THEME_PAGE_CONTENT_BUDGET_PX and
// this file's own _Static_assert in ui_page_network_manage.c for the real
// numbers. TODO.md's own status note named the fix: split Scan and Saved
// into their own sub-page via kiln_ui_show(), the same pattern
// ui_page_safety.c/ui_page_history.c (now removed, but the pattern lives on
// in ui_page_board_health.c and friends) already use for "this doesn't fit
// on the page that links to it" content -- rather than shave padding again,
// which every prior pass on this file already tried and which just drifts
// back the next time a row is added.
//
// Everything that used to live in ui_page_network.c's "Home-mode section"
// list block moved here verbatim: the Scan/Saved toggle, the Scan button
// and its async worker (wifi_prov_scan() off lvgl_port_task, same freeze fix
// as before), the saved-network list with per-row Forget (+ confirm
// msgbox), and the Connect modal (SSID tap -> password entry ->
// wifi_prov_add_network(), also async). ui_page_network.c itself now only
// ever shows a single "Manage networks" button that navigates here via
// kiln_ui_show("network_manage") -- see that file's header comment for what
// stayed behind (status/mode/QR/AP-identity).
#ifndef UI_PAGE_NETWORK_MANAGE_H
#define UI_PAGE_NETWORK_MANAGE_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* kiln_ui_page_build_fn (kiln_ui.h) -- builds and returns this page's root
 * screen object. Called once by kiln_ui, on first
 * kiln_ui_show("network_manage"). */
lv_obj_t *ui_page_network_manage_build(void);

/* Closes the Wi-Fi connect modal and clears the typed password; called on the LCD relock edge. */
void ui_page_network_manage_relock_close(void);

#ifdef __cplusplus
}
#endif

#endif // UI_PAGE_NETWORK_MANAGE_H
