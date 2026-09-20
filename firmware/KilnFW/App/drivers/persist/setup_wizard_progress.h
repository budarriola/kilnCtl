// setup_wizard_progress -- thin, versioned NVS record of which steps of the
// whole-kiln setup wizard (docs/SETUP_WIZARD.md) have been visited.
//
// This module owns ONLY step-visited state: {state, timestamp, note} per
// step, nothing else. It never stores a copy of any config value (zone
// settings, PID gains, safety-processor params, ...) -- every such value's
// source of truth stays its existing store (zones_cfg via
// zones_config_json.h, the Pico's config_store via safety_cfg_store.h,
// etc). See docs/SETUP_WIZARD.md section 5 point 2: "the wizard can
// never disagree with the board about a value; at worst it disagrees about
// whether a step was visited" -- and section 9: "do not add a wizard-owned
// copy of any config value" (this repo's "reset one side of a pair" bug
// class, four confirmed instances -- see MEMORY.md
// project_reset_one_side_bug_class).
//
// PERSISTENCE: lives in NVS ONLY, deliberately NOT on the `cfg` LittleFS
// partition and NOT dual-written there the way display_power_cfg.c/
// unit_pref.c are -- docs/SETUP_WIZARD.md section 5 point 1: user
// config is mid-migration to `cfg` with NVS dual-write
// (docs/CONFIG_FILESYSTEM.md), and this record is exactly what must stay
// intact while diagnosing a filesystem problem, so it deliberately does not
// ride on that same migration.
//
// AUTHORITY: `/api/readiness` is authoritative over this store. A step
// recorded here as done, whose corresponding readiness item reports
// not_done, must render as REGRESSED, never as done -- the precedence rule
// is setup_wizard_progress_effective_state() below, a pure function so it
// can be exercised by a host test with no HTTP/readiness wiring at all.
// Wiring it to a live readiness snapshot is the setup-page shell's job
// (docs/SETUP_WIZARD.md implementation step 3), not this module's.
//
// SCHEMA HISTORY / MIGRATION: version 1 stored {state, ts} per step only.
// Version 2 appended `note[SETUP_WIZARD_NOTE_MAX]` to the TAIL of each
// per-step record -- same "append only at the true tail" discipline
// zones_config_migrate.c documents for its own per-zone structs. A v1 blob
// loads with every note defaulted to empty; nothing else changes. Version 3
// grew the step count 13->14 (added step 13, "Authentication (optional)").
// Version 4 DROPPED what was step 11 ("Coupling matrix (optional)", removed
// from the wizard 2026-09-19) and shifted every step after it down by one
// slot, landing SETUP_WIZARD_STEP_COUNT at 13. Version 5 (current) removes a
// second, separate step: the old standalone step 8 ("Current sensing:
// install & calibrate") was folded into step 7 (safety processor
// commissioning), which now also carries the CT channel-mapping and gain
// calibration fields, and every step after old 8 shifts down by one more
// slot -- SETUP_WIZARD_STEP_COUNT is now 12. An old blob's real per-step
// state survives both shifts (see setup_wizard_progress.c's
// remap_drop_index() and the version-specific migrate functions built on
// it); only whatever was recorded for a removed step itself is discarded,
// never anything else.
#ifndef KILNCTL_SETUP_WIZARD_PROGRESS_H
#define KILNCTL_SETUP_WIZARD_PROGRESS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// One row per docs/SETUP_WIZARD.md section 3's table (steps 0..12 as of
// 2026-09-19's removal of the former step 11, "Coupling matrix (optional)").
// Kept in lockstep with setup_wizard_page.html's WIZARD_STEPS array -- the
// two used to drift silently (WIZARD_STEPS grew a 14th entry, step 13
// "Authentication (optional)", for the web-auth work while this constant
// stayed at 13, so GET /api/setup/progress never surfaced step 13 and POST
// rejected it with "step out of range"; found 2026-09-18 against the live
// bench board). setup_wizard_step_count_mirror_drift_check.py pins these two
// counts against each other so this cannot happen again silently.
#define SETUP_WIZARD_STEP_COUNT 12

typedef enum {
    SETUP_WIZ_STEP_PENDING = 0,
    SETUP_WIZ_STEP_DONE = 1,
    SETUP_WIZ_STEP_SKIPPED = 2,
} setup_wizard_step_state_t;

static inline bool setup_wizard_step_state_is_valid(setup_wizard_step_state_t s)
{
    return s == SETUP_WIZ_STEP_PENDING || s == SETUP_WIZ_STEP_DONE || s == SETUP_WIZ_STEP_SKIPPED;
}

// Wire-format name <-> enum, shared by setup_progress_http.c's GET/POST
// handlers and this header's own host tests -- kept here (rather than
// file-static in the .c, like readiness_http.c's status_name()) so the
// endpoint's JSON shape can be exercised without dragging in
// esp_http_server, same "pure predicate split out for host testing" reason
// readiness_http.h gives for readiness_commissioning_status().
static inline const char *setup_wizard_step_state_name(setup_wizard_step_state_t s)
{
    switch (s) {
    case SETUP_WIZ_STEP_DONE: return "done";
    case SETUP_WIZ_STEP_SKIPPED: return "skipped";
    case SETUP_WIZ_STEP_PENDING:
    default: return "pending";
    }
}

static inline bool setup_wizard_step_state_from_name(const char *name, setup_wizard_step_state_t *out)
{
    if (strcmp(name, "pending") == 0) { *out = SETUP_WIZ_STEP_PENDING; return true; }
    if (strcmp(name, "done") == 0)    { *out = SETUP_WIZ_STEP_DONE; return true; }
    if (strcmp(name, "skipped") == 0) { *out = SETUP_WIZ_STEP_SKIPPED; return true; }
    return false;
}

// Max length of the free-text note (a skip reason, mainly -- section 5
// point 5: "[s]kipping is allowed and recorded with a reason"), NUL
// included.
#define SETUP_WIZARD_NOTE_MAX 32

// In-RAM view of one step's recorded state -- shape only, no config value.
typedef struct {
    setup_wizard_step_state_t state;
    uint32_t ts;                          // seconds since boot at last write (esp_timer, monotonic -- see .c)
    char note[SETUP_WIZARD_NOTE_MAX];     // "" unless state == SKIPPED (or a note was recorded some other way)
} setup_wizard_step_t;

// The effective state a caller (eventually the /setup page, via a live
// readiness snapshot) must render -- REGRESSED is not a stored value, only
// ever a computed one: see setup_wizard_progress_effective_state().
typedef enum {
    SETUP_WIZ_EFFECTIVE_PENDING = 0,
    SETUP_WIZ_EFFECTIVE_DONE = 1,
    SETUP_WIZ_EFFECTIVE_SKIPPED = 2,
    SETUP_WIZ_EFFECTIVE_REGRESSED = 3,
} setup_wizard_effective_state_t;

// THE precedence rule (docs/SETUP_WIZARD.md section 5 point 3):
// `/api/readiness` is authoritative over this store. A step stored as DONE
// whose readiness-backed item is applicable and reports not-ready must
// render as REGRESSED, never as DONE -- readiness wins.
//
// `readiness_applicable` is false for a step with no 1:1 readiness item
// (e.g. step 4, zone type) or when the caller has no live readiness
// snapshot yet -- in either case there is nothing to regress against, so
// the stored state is reported as-is. `readiness_ready` is only consulted
// when `readiness_applicable` is true.
//
// Pure function, no I/O -- host-testable with no NVS/HTTP/readiness stub at
// all, which is exactly what lets this precedence be negative-tested in
// isolation (see test_setup_wizard_progress.c).
static inline setup_wizard_effective_state_t setup_wizard_progress_effective_state(
    setup_wizard_step_state_t stored, bool readiness_applicable, bool readiness_ready)
{
    if (stored == SETUP_WIZ_STEP_DONE && readiness_applicable && !readiness_ready) {
        return SETUP_WIZ_EFFECTIVE_REGRESSED;
    }
    switch (stored) {
    case SETUP_WIZ_STEP_DONE:
        return SETUP_WIZ_EFFECTIVE_DONE;
    case SETUP_WIZ_STEP_SKIPPED:
        return SETUP_WIZ_EFFECTIVE_SKIPPED;
    case SETUP_WIZ_STEP_PENDING:
    default:
        return SETUP_WIZ_EFFECTIVE_PENDING;
    }
}

// Loads the persisted record from NVS (migrating an older-version blob if
// found). Never fails the boot: on any problem (partition absent, never
// written, corrupt/wrong-size/unrecognized-version blob), every step is
// left at {PENDING, 0, ""} -- the safe "nothing visited yet" state -- and
// ESP_OK is still returned, same convention as touch_cal_store_load()/
// display_power_cfg_start(). Idempotent; safe to call more than once.
esp_err_t setup_wizard_progress_start(void);

// Copies every step's current in-RAM state into out[SETUP_WIZARD_STEP_COUNT].
void setup_wizard_progress_get_all(setup_wizard_step_t out[SETUP_WIZARD_STEP_COUNT]);

// Single-step read. Returns ESP_ERR_INVALID_ARG (leaving *out untouched) for
// step_index >= SETUP_WIZARD_STEP_COUNT.
esp_err_t setup_wizard_progress_get_step(uint8_t step_index, setup_wizard_step_t *out);

// Records `state` (and `note`, which may be NULL/empty -- truncated to
// SETUP_WIZARD_NOTE_MAX-1 bytes if longer, never rejected outright, since a
// too-long note is an operator's free text, not a schema violation) for one
// step, stamps `ts` from the monotonic clock, and persists the whole record.
// Refuses (ESP_ERR_INVALID_ARG, in-RAM state left untouched) for
// step_index >= SETUP_WIZARD_STEP_COUNT or an out-of-range `state` value --
// this is how "unknown-step rejection" is enforced, since there is no other
// gate between an HTTP POST body and this call.
//
// In-RAM state updates first regardless of whether the NVS write below
// succeeds (same ordering as display_power_cfg_set()/unit_pref_set()); a
// persist failure is logged via esp_err_to_name() and returned, never
// silently discarded, but the live value already took effect for this boot.
esp_err_t setup_wizard_progress_set_step(uint8_t step_index, setup_wizard_step_state_t state, const char *note);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_SETUP_WIZARD_PROGRESS_H
