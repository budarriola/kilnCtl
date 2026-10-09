// profiles_export_http -- single-profile export/import (UI_PLAN.md "Open:
// settings import/export (partial) and profile import/export", item 2).
//
// Separate from backup_http.c/backup_export.c/backup_import.c on purpose --
// that pair round-trips the WHOLE board (zones, PID tuning, every profile
// slot) as one file; this module exports/imports exactly ONE profile, so a
// user can share a single fire schedule (or move it between two kilnCtl
// boards) without also carrying the other board's zone/PID configuration.
// Kept as its own module rather than folded into profiles_http.c/
// profiles_catalog_http.c/profiles_edit_http.c so it can be added, reviewed,
// and (if ever needed) removed without touching that split's existing three
// files -- it only calls their public API (profiles_http_get()/
// profiles_http_save(), profiles_http.h) plus the JSON reader already
// shared with backup_import.c (persist/backup_json.h), never their
// internal-linkage state.
#ifndef PROFILES_EXPORT_HTTP_H
#define PROFILES_EXPORT_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers GET /api/profile/export?id=N and POST /api/profile/import on
// the server profiles_http_start() already registered on. Must be called
// AFTER profiles_http_start() (same ordering rule every *_http_start() in
// main_network_http.c documents) since both handlers call profiles_http_get()/
// profiles_http_save(), which read/write profiles_http.c's own NVS-backed
// slot state -- calling before that state is loaded would export/import
// against not-yet-initialized slots.
esp_err_t profiles_export_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // PROFILES_EXPORT_HTTP_H
