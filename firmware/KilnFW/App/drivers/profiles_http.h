// profiles_http -- Fire profile creation page (TODO.md section 5), serving
// /profiles and its JSON CRUD API.
//
// Scope: this module owns profile STORAGE and validation. TODO.md section 6
// (the profile-execution engine, App/drivers/profile_executor.c) is the
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

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILES_MAX_COUNT 8
#define PROFILE_NAME_MAX_LEN 15
#define PROFILE_MAX_SEGMENTS 12

typedef struct {
    float target_c;
    float ramp_c_per_hr; /* 0 = no ramp-rate constraint on this segment (TODO.md section 5) */
    uint32_t dwell_min;
} profile_segment_t;

typedef struct {
    char name[PROFILE_NAME_MAX_LEN + 1];
    /* TODO.md 6A.5: a profile can now drive more than one zone at once,
     * sharing a single ramp/dwell schedule via ramp-lock (the shared
     * setpoint only advances once every participating zone is within
     * PROFILE_EXECUTOR_RAMP_LOCK_BAND_C of it). Bit i = zone i participates;
     * replaces the single zone_index this field used to be -- a profile
     * targeting exactly one zone is just a mask with one bit set, so this
     * is a superset, not a behavior change for the single-zone case. */
    uint8_t zone_mask;
    uint8_t segment_count;
    profile_segment_t segments[PROFILE_MAX_SEGMENTS];
} profile_t;

/* Loads all used profile slots from NVS (namespace "kiln_cfg", keys
 * "prof0".."prof7" plus a "prof_used" bitmap byte) and registers
 * /profiles + the profile CRUD API on the server wifi_provision_http.c
 * already started. No hardware pointers needed. */
esp_err_t profiles_http_start(void);

/* Read-only accessor for profile_executor.c: copies slot `id`'s stored
 * profile into *out. Returns false (and leaves *out untouched) for an
 * out-of-range or unused slot -- the caller must treat that as "cannot
 * run," not as an empty/zeroed profile. */
bool profiles_http_get(uint8_t id, profile_t *out);

/* Validate-and-commit entry point shared with the UART CONTROL bridge
 * (uart_bridge_ext.c) -- the same range/feasibility validation and NVS
 * commit profile_post_handler() runs, minus the x-www-form-urlencoded
 * parsing step (the caller already has a decoded profile_t). requested_id
 * >= PROFILES_MAX_COUNT means "first free slot", same convention as the
 * HTTP handler's "id missing/-1/out of range" case. On success, writes the
 * committed slot to *out_id and the number of ramp-rate warnings (segments
 * within 20% of a zone's ceiling; see PROFILE_RAMP_WARN_FRACTION) to
 * *out_warning_count (may be NULL). On failure, leaves stored state
 * untouched and writes a human-readable reason into err_msg. */
bool profiles_http_save(uint8_t requested_id, const profile_t *candidate, uint8_t *out_id,
                        uint8_t *out_warning_count, char *err_msg, size_t err_cap);

/* Same erase-and-clear profile_delete_post_handler() runs. Returns false
 * (no-op) for an out-of-range or already-unused id. */
bool profiles_http_delete(uint8_t id);

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
