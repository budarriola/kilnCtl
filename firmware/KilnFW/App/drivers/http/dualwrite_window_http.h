// dualwrite_window_http -- GET /api/dualwrite_window: the owner-approved
// dual-write closure criterion's live progress (see
// drivers/persist/dualwrite_window.h and docs/FILESYSTEM.md's
// "Dual-write window" section) -- "14 of 20 clean boots, firing not yet
// done, restore verified" as one small JSON body, so an operator does not
// have to read flash or logs to answer "can this window close yet".
//
// Deliberately its OWN endpoint, not a field bolted onto GET /api/cfgfs:
// cfg_fs_status.c is owned by another pass as of this writing (see that
// module's own header for its established, deliberately narrow scope --
// "calls ONLY cfg_fs.h's public API"), and this module's data does not come
// from cfg_fs at all (it comes from plain NVS, by design -- see
// dualwrite_window.h's top comment for why). Bolting an unrelated NVS-backed
// counter onto a module whose entire design point is "pure, reads only
// cfg_fs.h" would violate that module's own stated scope. A request for a
// `dual_write_window` field on /api/cfgfs instead of this file has been
// filed with that endpoint's owner (see this pass's report) -- if that
// lands, this file's GET handler becomes redundant and can be removed; until
// then this is the only place the progress is visible over HTTP.
//
// POST /api/dualwrite_window/restore_verified: lets PC-side backup/restore
// tooling (owned elsewhere as of this writing -- "backup-over-filesystem" is
// out of this pass's scope) attest that it has completed and verified one
// backup/restore round trip against the file path. This module does not
// perform or check that round trip itself -- see dualwrite_window.h's
// dualwrite_window_note_restore_verified() doc comment. No body is read;
// the POST itself is the attestation. Idempotent -- a second POST after the
// first is a no-op (the underlying flag is sticky).
#ifndef DUALWRITE_WINDOW_HTTP_H
#define DUALWRITE_WINDOW_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers both routes on the httpd instance wifi_provision_http_start()
 * already brought up, and runs dualwrite_window_boot_check() once (see that
 * function's own doc comment for why it is deliberately triggered from
 * here rather than from main_boot_early.c). Non-fatal to app_main on
 * registration failure, same convention as every other *_http_start() in
 * this directory. */
esp_err_t dualwrite_window_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // DUALWRITE_WINDOW_HTTP_H
