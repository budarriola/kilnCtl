// profiles_http -- Fire profile creation page (TODO.md section 5), serving
// /profiles and its JSON CRUD API.
//
// Scope: this module owns profile STORAGE and validation. TODO.md section 6
// (the profile-execution engine, App/drivers/control/profile_executor.c) is the
// consumer -- it reads a profile through profiles_http_get() below and
// drives relays from it, but does not itself touch NVS or this module's
// storage, same one-owner discipline zones_http.c already established for
// zone config.
//
// Feasibility checking (TODO.md section 5) reads zones_http.c's per-zone
// max-ramp ceiling through zones_config_get_max_ramp() -- a read-only
// getter, not NVS access of its own -- so zone config has exactly one
// owner.
#ifndef PROFILES_HTTP_H
#define PROFILES_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "profiles_types.h"
#include "profiles_store.h" /* profiles_http_get()/_save()/_delete() -- pure
                              * storage accessors, split out for
                              * control/persist/safety callers; included back
                              * here so existing callers of this header keep
                              * seeing them. */

#ifdef __cplusplus
extern "C" {
#endif

/* Loads all used profile slots from NVS (namespace "kiln_cfg", keys
 * "prof0".."prof7" plus a "prof_used" bitmap byte) and registers
 * /profiles + the profile CRUD API on the server wifi_provision_http.c
 * already started. No hardware pointers needed. */
esp_err_t profiles_http_start(void);

/* Read-only accessor for the validation bounds profiles_http.c enforces at
 * save time (PROFILE_TARGET_C_MIN/MAX, PROFILE_RAMP_C_PER_HR_MIN/MAX,
 * PROFILE_DWELL_MIN_MAX -- see profiles_http.c's #defines, which stay the
 * single source of truth; this just hands them out so a caller like the LCD
 * profile builder (ui_page_profile_builder_segment.c) can feed them to its
 * numeric keypad as min/max and make an out-of-range value impossible to
 * enter, instead of duplicating the numbers and hoping they stay in sync.
 * Any output pointer may be NULL if that bound isn't needed. */
void profiles_http_get_bounds(float *out_target_c_min, float *out_target_c_max,
                              float *out_ramp_c_per_hr_min, float *out_ramp_c_per_hr_max,
                              uint32_t *out_dwell_min_max);

#ifdef __cplusplus
}
#endif

#endif // PROFILES_HTTP_H
