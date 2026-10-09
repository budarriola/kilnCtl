// backup_http -- Settings & profile backup/restore (TODO.md 0.5's settings
// import/export item and UI_PLAN.md's separately-named import/export item,
// unified into ONE feature per this pass's brief: they were the same
// request wearing two names, not two features).
//
// Serves GET /settings/backup (a dedicated page, not a section bolted onto
// settings_page.html -- both source docs call out import/export as its own
// destination), GET /api/backup/export (a streamed JSON file download, same
// Content-Disposition convention as dashboard_http.c's /api/history.csv) and
// POST /api/backup/import (validate-everything-before-writing-anything JSON
// upload).
//
// SCOPE, and why it is narrower than "every setting on the board": this pass
// owns settings_http.c/.h, profiles_http.c/.h, and their two *_page.html
// files -- NOT rules_http.c/.h or wifi_prov.c/.h, which other work this same
// session owns. zones_http.c/.h IS this file's to widen, though (2026-08-21,
// backup format version 2): a backup can only round-trip a value through a
// PUBLIC getter *and* a public SETTER, and zones_http.h originally exposed
// get+set pairs for only four things (per-channel PID gains, the FOPDT plant
// model, per-channel thermocouple type, and the RP2040 safety processor's own
// thermocouple type) -- everything else a zone stores (name, relay/
// thermocouple wiring, cal_offset_c, max ramp ceiling, sanity-rate threshold,
// control mode, temperature limits, heater timing, the 8 guard-threshold
// overrides, and cross_zone_max_delta_c) had a getter with no setter at all,
// so it read back on export but could never actually be restored. This pass
// added the missing setters to zones_http.h/.c and widened the export/import
// in backup_http.c to match, closing that gap: a field that reads back but
// can't be written is worse than one that was never offered, and now every
// zone field is one or the other -- exported AND restorable.
//
// Fire profiles (profiles_http.h: profiles_http_get()/_save()/_delete(), a
// complete CRUD API already) round-trip in full.
//
// Wi-Fi credentials are DELIBERATELY NOT included, on either side. A backup
// restored onto a board on a different network with the original board's
// Wi-Fi credentials baked in would try to join a network that may not exist
// where the replacement board now lives, or -- worse -- silently join
// whatever network the ORIGINAL board's credentials happened to name if that
// SSID exists nearby (an operator who thinks they are restoring kiln tuning
// would not expect their kiln to also change what network it's on). Wi-Fi
// stays a per-board, page-local decision made on /wifi, same as it always
// has been; see backup_page.html's own on-page text, which says this
// explicitly rather than leaving it as a silent omission a reader would have
// to infer.
#ifndef BACKUP_HTTP_H
#define BACKUP_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Registers /settings/backup, GET /api/backup/export and
 * POST /api/backup/import on the server wifi_provision_http.c already
 * started. No hardware pointers needed -- this module owns no NVS storage of
 * its own; every value it reads or writes belongs to profiles_http.c or
 * zones_http.c, which it calls through their existing public APIs only. Same
 * "logs and returns esp_err_t, a failed registration just 404s these routes
 * this boot" convention as every other *_http_start() in this directory. */
esp_err_t backup_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // BACKUP_HTTP_H
