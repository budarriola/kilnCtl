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

/* Owner's request, verbatim (2026-08-27): "insted of the relays and rules
 * section i want them to be part of the profile. unused relays or io may be
 * controled through the profile as a segment. the relay/io segments should
 * have the option to be blocking or nonblocking for the rest of the profile
 * (runs with the next segment or before)". This is the segment "kind"
 * discriminator that request adds: a segment is either the pre-existing
 * temperature ramp/dwell step, or a relay/IO command. The rules engine
 * (rules_http.c/rules_task.c) still exists and is NOT touched by this pass --
 * it is deleted in a later task, once this gives every "drive a non-zone
 * relay" use case a home that doesn't depend on it. Do not add a case here
 * for anything rules-specific. */
typedef enum {
    PROFILE_SEG_KIND_ZONE_RAMP = 0, /* target_c/ramp_c_per_hr/dwell_min drive the shared
                                      * zone setpoint, exactly the pre-existing behavior */
    PROFILE_SEG_KIND_RELAY_IO = 1,  /* io_target/io_state/io_blocking/io_leave_on_at_end below
                                      * drive one relay or general-purpose IO line instead */
} profile_seg_kind_t;

/* profile_segment_t::io_target encoding -- deliberately NOT the SX1509's raw
 * IO0..IO15 pin numbers, so an out-of-range or miscoded value cannot land on
 * a pin this feature must never reach. kiln_io.h's own pin map (see that
 * file's top comment, and its "IO8..IO10 ~DRDY... IO14 LCD_IORQ, IO15
 * LCD_Reset" lines) is why: a profile that could assert LCD_Reset would blank
 * the display mid-firing, and one that could toggle a thermocouple's ~DRDY
 * line would corrupt SPI framing on a live temperature reading -- neither a
 * profile segment has any legitimate reason to touch. Two disjoint ranges,
 * both validated against kiln_io.h's own KILN_IO_RELAY_COUNT/
 * KILN_IO_DIGITAL_COUNT (never a literal 4/7 duplicated here) by
 * profiles_http.c's validate_io_target() AND profile_executor.c's own
 * (independent, "two checks deliberately" per the owner's ask) re-check at
 * run start -- see that function for the second gate. */
#define PROFILE_IO_TARGET_NONE 0u
#define PROFILE_IO_TARGET_RELAY_BASE 1u  /* 1..KILN_IO_RELAY_COUNT (4) = Relay1..Relay4 */
#define PROFILE_IO_TARGET_IO_BASE 11u    /* 11..(10+KILN_IO_DIGITAL_COUNT) (17) = IO_1..IO_7.
                                           * The 10-wide gap between the two ranges (5..10) is
                                           * deliberately never assigned to anything -- it is not
                                           * "DRDY" or "LCD" by construction, just dead encoding
                                           * space, so a future relay/IO count change on the board
                                           * has room to grow either range without the two ever
                                           * touching. */

typedef struct {
    float target_c;
    float ramp_c_per_hr; /* 0 = no ramp-rate constraint on this segment (TODO.md section 5) */
    uint32_t dwell_min;  /* ZONE_RAMP: dwell minutes at target_c.
                           * RELAY_IO: how long this segment's hold/handoff lasts, in minutes --
                           * for a BLOCKING segment, how long the rest of the schedule waits;
                           * for a NON-BLOCKING one, how long its own on/off command holds before
                           * this segment finishes on its own (see profile_executor.c's
                           * io_segs_tick()), independent of whatever ramp/dwell segments run
                           * during that time. 0 is legal for either kind: a blocking segment with
                           * dwell_min 0 fires its relay/IO command and releases the schedule on
                           * the very next tick ("blocking... before" the next segment, per the
                           * owner's wording, with essentially no hold); a non-blocking one with
                           * dwell_min 0 fires and is immediately eligible to finish (see
                           * io_seg_finish()). */
    uint8_t seg_kind;     /* profile_seg_kind_t */
    uint8_t io_target;    /* PROFILE_IO_TARGET_* encoding; meaningful only when
                            * seg_kind == PROFILE_SEG_KIND_RELAY_IO */
    uint8_t io_state;     /* 0 = off, nonzero = on; meaningful only for RELAY_IO */
    uint8_t io_blocking;  /* nonzero = the rest of the profile's schedule waits for this segment's
                            * dwell_min before advancing (runs BEFORE the next segment); zero =
                            * the schedule advances to the next segment immediately and this
                            * segment's hold runs concurrently (runs WITH the next segment) --
                            * this is the owner's "blocking or nonblocking... runs with the next
                            * segment or before" option, verbatim. Meaningful only for RELAY_IO. */
    uint8_t io_leave_on_at_end; /* Owner's follow-up answer: "each segment chooses" whether a
                                  * still-active NON-BLOCKING segment is left in its last state if
                                  * the run ends before its own hold time elapses, or forced off
                                  * like everything else. DEFAULT MUST STAY 0/false -- a
                                  * zero-initialized profile_segment_t (a fresh save, a partially
                                  * filled form, a still-v2 profile carried across the migration
                                  * below) must force its relay off, never leave it energized with
                                  * nothing owning it. The elements on this board are live as of
                                  * 2026-08-26 -- see profile_executor.c's io_seg_finish() for the
                                  * one place this flag is actually read, and its doc comment for
                                  * exactly which run-ending paths honor it (only the clean DONE
                                  * path; FAULTED/HALT/PAUSE force off regardless, on purpose).
                                  * Meaningful only for a RELAY_IO segment with io_blocking == 0. */
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
