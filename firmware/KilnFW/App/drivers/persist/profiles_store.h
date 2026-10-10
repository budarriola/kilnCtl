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

/* Run-start re-check for profile_executor_run() (HTTP input parsing audit L23).
 * Called with s_exec.lock held, so it is lock-free on purpose: an atomic load
 * and a RAM bitmap read, never the profiles save lock (save lock ->
 * s_exec.lock is the established order, cfg_save_lock.h) and never the flash
 * worker. False when a delete of user slot `id` is in flight, or the slot was
 * deleted after the caller copied it with profiles_http_get(). Builtin ids are
 * always runnable (they cannot be deleted). */
bool profiles_http_slot_runnable(uint8_t id);

/* ---- Zone -> aux rule retarget (docs/SPARE_RELAY_ONOFF_PLAN.md section 10) ----
 *
 * Rewrites every stored profile's on/off rules with zone_index == `zone` to
 * the aux target for `relay` (1..4). The rewrite is all-or-nothing: the commit
 * validates each rewritten profile, persists it, reads every persisted blob
 * back, and on ANY failure reverses the slots it already changed (the
 * transform is an exact, invertible byte swap of zone_index, so the revert
 * needs no stored copy of the old profiles). Never called while a profile
 * runs; the caller owns that gate.
 *
 * Plan is read-only. It refuses (returns false, reason in err) when:
 *   - a running/paused profile has a rule for `zone`;
 *   - a rule for `zone` cannot be represented on an aux output (temp_source
 *     above 1, or a temperature compare while `zone_has_tc` is false);
 *   - any stored rule already targets the destination aux relay.
 * Commit requires the aux entry for `relay` to be ENABLED already (it runs the
 * normal rule validator on every rewritten profile). */
typedef struct {
    uint8_t profiles_scanned;   /* used slots examined */
    uint8_t profiles_affected;  /* slots holding at least one rule for the zone */
    uint16_t rules_retargeted;  /* rules rewritten (planned, or committed) */
} profiles_retarget_counts_t;

bool profiles_retarget_zone_to_aux_plan(uint8_t zone, uint8_t relay, bool zone_has_tc,
                                        profiles_retarget_counts_t *counts, char *err, size_t err_cap);

/* Returns true only if every affected slot was rewritten, persisted and read
 * back. On false, stored state is back to what it was (err says whether the
 * revert itself was clean). *counts is filled on both outcomes. */
bool profiles_retarget_zone_to_aux_commit(uint8_t zone, uint8_t relay, bool zone_has_tc,
                                          profiles_retarget_counts_t *counts, char *err, size_t err_cap);

/* Resume variant of commit for an interrupted conversion: slots an earlier run already moved
 * (rules at the aux target) are accepted and left alone; the rest are rewritten the same way.
 * Counts report this call's own rewrites. */
bool profiles_retarget_zone_to_aux_resume(uint8_t zone, uint8_t relay, bool zone_has_tc,
                                          profiles_retarget_counts_t *counts, char *err, size_t err_cap);

/* Swaps every rule aimed at aux `relay` back to `zone` in every stored profile and re-persists
 * each changed slot (read-back verified). For the caller's own rollback after a commit that
 * succeeded but a later step failed. Safe because the plan refuses any profile that already had
 * a rule at the aux target. True = every changed slot persisted and verified. */
bool profiles_retarget_zone_to_aux_revert(uint8_t zone, uint8_t relay);

#ifdef __cplusplus
}
#endif

#endif // PROFILES_STORE_H
