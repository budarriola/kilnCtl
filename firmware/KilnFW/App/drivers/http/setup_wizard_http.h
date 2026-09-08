// setup_wizard_http -- serves GET /setup, the whole-kiln setup wizard's page
// shell (docs/SETUP_WIZARD_PLAN.md section 8, implementation step 3).
//
// This module owns NOTHING but the static page. It is deliberately as thin
// as readiness_http.c's own page_get_handler(): no config store, no NVS, no
// progress blob. The wizard's actual step content and its NVS-backed
// progress store are docs/SETUP_WIZARD_PLAN.md's steps 1/2/4-11, owned
// elsewhere/later -- this file only wires the URL the plan's step 3 asks for
// so the shell has somewhere to be served from. The page's own JS is the
// thing that calls GET /api/readiness (already live, readiness_http.c) and
// GET /api/setup/progress (not live yet as of this file's authorship -- the
// page's fetch for it is written defensively against a 404/network error,
// per the plan's step 2 documented shape: {"version":1,"steps":{"<n>":
// {"state":"pending"|"done"|"skipped","ts":<unix>,"note":<string>}}}).
#ifndef SETUP_WIZARD_HTTP_H
#define SETUP_WIZARD_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers GET /setup on the server wifi_provision_http.c already started.
 * No hardware pointers needed -- the page is static markup/JS; every fact it
 * renders comes from client-side fetches against endpoints other modules
 * already expose. Call any time after wifi_provision_http_start(), same
 * ordering rule readiness_http_start() documents (this file has no ordering
 * dependency on readiness_http_start() itself -- either order is fine, both
 * just register URIs on the shared server). */
esp_err_t setup_wizard_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // SETUP_WIZARD_HTTP_H
