// ui_edit_firing_apply -- the LVGL-free half of ui_page_edit_firing.c.
//
// Everything that decides WHAT the LCD's "Edit firing" page may change and
// WHETHER an Apply goes through lives here, so a host test can call the real
// code (test_ui_edit_firing_apply.c) instead of re-running a copy of the
// sequence. ui_page_edit_firing.c owns only widgets, paging and text.
//
// Apply mirrors api_profile_live_post_handler() (profiles_live_http.c) --
// the same parse-level ranges, profiles_validate_candidate(HARD),
// live_edit_check_window() against the same origin reference, then
// live_profile_save_working() -- plus the fork the web does as a separate
// POST /api/profile/live/fork, done only AFTER every check has passed so a
// refused edit never leaves an unchanged pending working copy behind (which
// would trigger the web's end-of-run save/discard prompt for nothing).
//
// Three guards the HTTP route does not need, because its page reloads what
// it posts from the server each time while this page holds a copy for as
// long as it is open:
//   - the firing must still be the one the copy was loaded from
//     (profile_id), else a stale copy of profile A would be saved as the
//     working copy of a newly started profile B;
//   - live_profile_generation() must not have moved since the copy was
//     loaded, else a web edit made while the page was open would be silently
//     overwritten by the LCD's older copy.
//   - relay/IO segments are read-only (the web's live page shows them
//     read-only too).
#ifndef UI_EDIT_FIRING_APPLY_H
#define UI_EDIT_FIRING_APPLY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profiles_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EDIT_FIRING_TARGET_STEP_C 5.0f
#define EDIT_FIRING_RAMP_STEP_C_HR 5.0f
#define EDIT_FIRING_DWELL_STEP_MIN 5u

typedef enum {
    EDIT_FIRING_SEG_FINISHED = 0,
    EDIT_FIRING_SEG_RUNNING,
    EDIT_FIRING_SEG_UPCOMING,
} edit_firing_seg_phase_t;

typedef enum {
    EDIT_FIRING_FIELD_TARGET = 0,
    EDIT_FIRING_FIELD_RAMP,
    EDIT_FIRING_FIELD_DWELL,
} edit_firing_field_t;

// What a loaded copy was loaded against.
typedef struct {
    uint8_t origin_id;         // profile id the running firing was started from
    uint8_t running_seg;       // executor segment_index at load / last poll
    uint32_t generation;       // live_profile_generation() read BEFORE the load
} edit_firing_ctx_t;

edit_firing_seg_phase_t edit_firing_seg_phase(uint8_t seg, uint8_t running_seg);

// True iff `seg` of `p` may be changed from the LCD: a ZONE_RAMP segment that
// is running or upcoming. Local mirror only; Apply re-checks authoritatively.
bool edit_firing_seg_editable(const profile_t *p, uint8_t seg, uint8_t running_seg);

// One +/- step on one field, clamped to the same PROFILE_* bounds the HTTP
// parser enforces. dir > 0 steps up, otherwise down. Returns true iff the
// value changed (false when not editable or already at the bound).
bool edit_firing_step(profile_t *p, uint8_t seg, uint8_t running_seg, edit_firing_field_t field, int dir);

// profiles_parse_profile_fields()'s per-segment range rules applied to an
// in-memory profile (the LCD never goes through the form parser). ZONE_RAMP:
// target 0..2015, ramp 0..1000, dwell 0..1440; RELAY_IO: dwell 0..1440.
bool edit_firing_fields_in_range(const profile_t *p, char *err, size_t err_cap);

// Loads the copy to edit for the running firing: the pending working copy
// for its origin if there is one, else the origin profile. False (ctx and
// *out unspecified) when no firing is active or nothing could be read.
bool edit_firing_load(profile_t *out, edit_firing_ctx_t *ctx);

// Apply. On success *ctx->generation is advanced to the post-save generation
// (so the next Apply from the same page is not refused as "edited
// elsewhere") and ctx->running_seg is refreshed. On refusal `err` holds the
// reason and nothing was written.
bool edit_firing_apply(const profile_t *candidate, edit_firing_ctx_t *ctx, char *err, size_t err_cap);

typedef enum {
    EDIT_FIRING_POLL_OK = 0,        // same firing, nobody else saved
    EDIT_FIRING_POLL_ENDED,         // firing no longer RUNNING/PAUSED/FAULTED
    EDIT_FIRING_POLL_OTHER_FIRING,  // a different profile is running now
    EDIT_FIRING_POLL_EDITED_ELSEWHERE,
} edit_firing_poll_state_t;

typedef struct {
    edit_firing_poll_state_t state;
    uint8_t running_seg;
    bool refused;             // executor refused the generation this page applied
    char refusal_msg[128];
} edit_firing_poll_t;

// Periodic check for the open page. `applied_generation` is the generation
// this page's last successful Apply produced (0 = none); a pickup refusal the
// executor recorded for exactly that generation is reported in `refused`.
void edit_firing_poll(const edit_firing_ctx_t *ctx, uint32_t applied_generation, edit_firing_poll_t *out);

#ifdef __cplusplus
}
#endif

#endif // UI_EDIT_FIRING_APPLY_H
