// setup_progress_http -- GET/POST /api/setup/progress: the HTTP face of
// drivers/persist/setup_wizard_progress.h.
//
// docs/SETUP_WIZARD.md implementation step 2. This is the whole-kiln
// setup wizard's progress store made reachable over HTTP -- nothing more.
// It does NOT compute or embed a live readiness snapshot in its response
// (that merge is the setup-page shell's job, implementation step 3, per the
// plan's own step table: "status merged from /api/setup/progress +
// /api/readiness" happens in the page's JS). Keeping this endpoint a plain
// read/write of the store's own state is what lets it stay a single
// source of no-config-values truth: it can never disagree with
// /api/readiness about a config value, because it holds none.
#ifndef SETUP_PROGRESS_HTTP_H
#define SETUP_PROGRESS_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts setup_wizard_progress_start() (loads/migrates the persisted
 * record) and registers GET+POST /api/setup/progress on the httpd instance
 * wifi_provision_http_start() already brought up. Non-fatal to app_main on
 * registration failure, same convention as every other *_http_start() in
 * this directory. */
esp_err_t setup_progress_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // SETUP_PROGRESS_HTTP_H
