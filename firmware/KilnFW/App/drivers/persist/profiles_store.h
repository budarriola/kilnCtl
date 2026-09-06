// profiles_store -- catalog storage accessors for fire profiles, split out
// of profiles_http.h (see drivers/ layering item 12, docs/HW_ABSTRACTION.md).
//
// These three functions are pure NVS-backed storage/validation calls with no
// httpd dependency; they are implemented in profiles_http.c (which still
// owns the /profiles httpd registration and CRUD handlers) and declared here
// so a control/persist/safety-tier caller does not have to pull in the
// http-named header just to read or write a profile slot.
//
// profiles_http.h includes this header back so its own callers (http/ui/
// bridge tier) are unaffected.
#ifndef PROFILES_STORE_H
#define PROFILES_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profiles_types.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 * committed slot to *out_id and the number of warnings (segments within 20% of
 * a zone's ramp-rate ceiling, PROFILE_RAMP_WARN_FRACTION, PLUS one more if
 * any ZONE_RAMP segment's target_c exceeds a participating zone's CURRENT
 * max_temp_c -- see profile_exceeds_zone_ceiling()'s own comment: that
 * condition is advisory here, never refused, since profiles are portable
 * between kilns and only STARTING one is where the ceiling is enforced, at
 * profile_executor_run.c's run-start re-check) to *out_warning_count (may
 * be NULL). On failure, leaves stored state untouched and writes a
 * human-readable reason into err_msg. */
bool profiles_http_save(uint8_t requested_id, const profile_t *candidate, uint8_t *out_id,
                        uint8_t *out_warning_count, char *err_msg, size_t err_cap);

/* Same erase-and-clear profile_delete_post_handler() runs. Returns false
 * (no-op) for an out-of-range or already-unused id. Pure storage, but kept
 * here alongside get/save (rather than only in profiles_http.h) because a
 * non-http-tier caller could reasonably need it too -- currently only
 * bridge-tier uart_bridge_ext_control.c calls it, which already includes
 * this header transitively via profiles_http.h; declared here regardless so
 * a future control/persist caller does not have to reach for the http
 * header for it. */
bool profiles_http_delete(uint8_t id);

#ifdef __cplusplus
}
#endif

#endif // PROFILES_STORE_H
