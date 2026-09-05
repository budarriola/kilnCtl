// profiles_types -- pure profile types/constants shared by lower-tier headers
// (profile_executor.h, profile_executor_state.h, profile_feasibility.h,
// run_state.h, profiles_builtin.h) that only need profile_t's shape, not the
// httpd handler API declared in profiles_http.h. Split out per
// tools/drivers_reorg/DRYRUN.md section 5 "Fix 1" (same shape as the plan's
// item 4 profile_executor_state.h split, ad46061/5f2812a) so those headers
// stop pulling in an http-tier header. No httpd or http-tier includes here.
#ifndef PROFILES_TYPES_H
#define PROFILES_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

#ifdef __cplusplus
}
#endif

#endif // PROFILES_TYPES_H
