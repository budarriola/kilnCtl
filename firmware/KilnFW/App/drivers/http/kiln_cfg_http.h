// kiln_cfg_http -- the HTTP surface over kiln_cfg_store.c's saved-kiln-config
// slots (owner-report, 2026-08-21 follow-up: "save kiln profiles with
// different relay, thermocouple, and PID configs ... survive a programming
// cycle ... allow creating a config from an existing one"). Called a KILN
// CONFIG throughout, never a "profile" -- see kiln_cfg_store.h's header
// comment for why that word is already taken in this codebase.
//
// Routes (form-urlencoded POSTs via http_form.h, hand-built JSON GET via
// APPEND(snprintf) -- same conventions as every other *_http.c in this
// component, no cJSON anywhere in this codebase):
//
//   GET  /api/kiln_configs
//        {"active_id":<int|null>,"configs":[{"id":N,"name":"...",
//         "is_active":bool},...],"max_count":N}
//   POST /api/kiln_configs/save     name=<str>  [id=<int> to overwrite]
//   POST /api/kiln_configs/clone    id=<int>, name=<str>
//   POST /api/kiln_configs/apply    id=<int>
//   POST /api/kiln_configs/delete   id=<int>
//   POST /api/kiln_configs/rename   id=<int>, name=<str>
//
// SAFETY: only .../apply can rewrite a running kiln's relay/thermocouple/PID/
// guard configuration underneath it -- this handler, and only this handler,
// calls ota_http_check_interlocks() (ota_http.h, the SAME predicate
// POST /api/ota/esp and /api/ota/pico already gate on -- "is a profile
// running / are the heaters on", NOT heat_interlock.c's opposite question of
// "may heat run during an update") and refuses with 409 Conflict on anything
// but OTA_INTERLOCK_OK, BEFORE ever calling kiln_cfg_store_apply(). save/
// clone/delete/rename never touch the live zones config, so they carry no
// such gate.
#ifndef KILN_CFG_HTTP_H
#define KILN_CFG_HTTP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the six routes above on the server wifi_provision_http.c already
// started. Call AFTER kiln_cfg_store_init() (this module reads/writes that
// store) and AFTER ota_http_start() (the apply handler calls
// ota_http_check_interlocks(), which is only meaningful once ota_http.c has
// been handed the io/thermo_bus/safety pointers it needs) -- see
// App/main.c's call-order comment at the kiln_cfg_http_start() call site.
esp_err_t kiln_cfg_http_start(void);

#ifdef __cplusplus
}
#endif

#endif // KILN_CFG_HTTP_H
