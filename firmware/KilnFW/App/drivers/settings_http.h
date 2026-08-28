// settings_http -- the settings hub, the two
// routes UI_PLAN.md's "web page structure" section names as the part that
// actually splits main_page.html apart: that file still carried its own
// inline Settings block, Danger zone, and per-relay manual toggles even
// after the earlier 2026-08-19/20/21 pass shipped shared nav.js/app.js and
// the standalone diagnostics/safety/thermo-fault pages. This module owns no
// live hardware state of its own and reads none directly -- exactly like
// diagnostics_http.c, both pages are static HTML+JS that poll the existing
// GET /api/status, GET /api/zones and GET /api/readiness endpoints
// client-side (settings_page.html's Danger zone additionally POSTs to the
// pre-existing /api/factory_reset). No new JSON endpoint is added here.
//
// GET /settings/manual (the manual per-relay toggles moved off
// main_page.html per UI_PLAN.md's "a phone in a pocket can brush a screen
// in a way a panel mounted on a kiln cannot" reasoning) was removed
// 2026-08-27: owner's call once the kiln's heating elements were actually
// wired to this board -- "the danger zone in the diagnostics page covers it
// fine." diagnostics_http.c's Danger Zone (POST /api/diagnostics/danger/
// relay) is the one sanctioned place left to move a relay by hand; unlike
// the removed page it carries its own explicit accept-the-risk gate and
// auto-exit timer. dashboard_http.c's POST /api/relay (manual_page.html's
// only caller) was removed with it -- see dashboard_http.c's removal
// comment there.
//
//   GET /settings          -- ui_page_config.c's web twin: nav hub linking
//                              every settings-ish destination, plus the
//                              Danger zone block moved off main_page.html.
//   GET /settings/display  -- theme + °C/°F preferences, split out of
//                              /settings 2026-08-22 (owner report: "both
//                              display theme and reset menu items take me to
//                              the same page" -- the two nav.js menu entries
//                              used to be /settings#display and
//                              /settings#danger, two anchors on this one
//                              page, so either menu choice landed on the same
//                              document). See settings_display_page.html.
#ifndef SETTINGS_HTTP_H
#define SETTINGS_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the two page routes above on the httpd instance
 * wifi_provision_http.c already started. Takes no hardware pointers -- same
 * shape as diagnostics_http_start()/readiness_http_start(). Non-fatal to
 * app_main on failure, same convention as every other *_http_start() in
 * this directory: logs and returns the esp_err_t, and a failed registration
 * just means those routes 404 this boot rather than app_main refusing to
 * come up. */
esp_err_t settings_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // SETTINGS_HTTP_H
