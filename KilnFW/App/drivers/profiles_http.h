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

#ifdef __cplusplus
}
#endif

#endif // PROFILES_HTTP_H
