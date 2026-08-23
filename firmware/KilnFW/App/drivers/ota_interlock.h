// ota_interlock.h -- the pure, host-testable half of TODO.md section 9.4's
// "Neither processor may be updated while the kiln can heat" rule
// (firmware/CommonFW/docs/UPDATE_PROTOCOL.md section 1).
//
// Same pure/impure split ota_auth.h documents in its own header comment,
// adapted: the actual data this check needs (profile-executor state,
// autotune-active, safety-link-up, per-zone temperatures, the update mutex)
// lives behind ESP-IDF/FreeRTOS-coupled modules (profile_executor.h,
// autotune_engine.h, safety_link.h) that cannot be host-built the way this
// file can. So this file takes a plain-old-data SNAPSHOT of that state --
// caller-filled, no pointers back into any of those modules -- and answers
// one pure question: does this snapshot permit starting an update, and if
// not, which specific precondition is unmet. The snapshot-taking itself (the
// ESP-IDF glue that calls profile_executor_get_status() etc. and populates
// an ota_interlock_snapshot_t) lives in ota_http.c, mirroring exactly how
// ota_http.c's hmac_sha256()/ota_challenge_get_handler() are the ESP-IDF
// glue around ota_auth.c's pure nonce/lockout state machine.
//
// UPDATE_PROTOCOL.md section 1's precondition table, and where each one is
// checked in this function:
//   | No profile or autotune running       | profile_state, autotune_active |
//   | No heater commanded on               | zones[i].heater_commanded      |
//   | run_state shows the kiln idle        | run_state_idle                 |
//   | Every zone below a configured ceiling | zones[i].actual_c vs ceiling  |
//   | Link healthy                         | safety_link_up                 |
// The Pico's own three preconditions (safety relay open, no trip pending,
// temperature ceiling) are enforced independently, on the Pico, per
// UPDATE_PROTOCOL.md's "It does not take the ESP's word for it" -- nothing
// here can or should stand in for that; this function is the ESP's own gate.
//
// "Both update paths refused unless..." (TODO.md 9.4) means this same check
// runs before EITHER a POST /api/ota/esp or a POST /api/ota/pico is allowed
// to proceed -- there is no per-path variant of the precondition list.
//
// Ordering versus authentication (ota_http_verify_request()): this file's
// header comment for ota_http_verify_request() and UPDATE_PROTOCOL.md
// section 2 do not specify whether interlocks or auth run first. This module
// is written so a future POST handler can call it either way, but the
// intended order, once that handler exists, is AUTH FIRST, THEN INTERLOCKS:
// revealing "zone 2 is at 340 C" to an unauthenticated caller leaks live
// kiln telemetry (temperatures, run state) to anyone on the LAN who can
// reach the endpoint, whereas the auth challenge/response reveals nothing
// about kiln state on failure. Checking interlocks first would make this
// endpoint an unauthenticated temperature-and-status oracle, which is a
// worse leak than the "wrong password" case UPDATE_PROTOCOL.md section 2
// already accepts as this design's known limit.
#ifndef KILNCTL_OTA_INTERLOCK_H
#define KILNCTL_OTA_INTERLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// No existing per-installation config store item covers this yet on the ESP
// side (zones_http.c has no "OTA temperature ceiling" field, and none was
// asked for) -- same honest situation as SaftyFW's own
// UPDATE_TASK_TEMP_CEILING_C (firmware/SaftyFW/src/tasks/update_task.c),
// which is also a hardcoded 100 degC default per SaftyFW/TODO.md item 10.9.
// A real config item is future work, not invented here to look more finished
// than it is.
#define OTA_INTERLOCK_TEMP_CEILING_C 100.0f

// Mirrors profile_exec_state_t (profile_executor.h) without depending on
// that header, which pulls in MAX31856.h/kiln_io.h/safety_link.h and is not
// host-buildable. The caller (ota_http.c) maps profile_executor_get_status()
// -> out.state onto this enum one-for-one.
typedef enum {
    OTA_INTERLOCK_PROFILE_IDLE = 0,
    OTA_INTERLOCK_PROFILE_RUNNING,
    OTA_INTERLOCK_PROFILE_PAUSED,
    OTA_INTERLOCK_PROFILE_DONE,
    OTA_INTERLOCK_PROFILE_FAULTED,
} ota_interlock_profile_state_t;

// One zone's worth of the state this check needs. `heater_commanded` is
// deliberately its own field rather than derived from profile_state here:
// TODO.md 9.4 lists "no heater commanded" as a *separate* precondition from
// "no profile running" (a manual relay-on via POST /api/relay, outside any
// profile run, must also block an update), even though the only source this
// pass has for it is profile_executor_get_status()'s per-zone
// relay_commanded_on -- which is only ever non-zero for zones an active run
// targets. A manual/rule-driven relay held on with NO profile running at all
// is a real gap this snapshot cannot see yet (relay_authority.c has no
// "what's commanded right now, globally" query independent of a caller);
// flagged here rather than silently pretended-covered. See TODO.md 9.4's
// checklist note for the same caveat spelled out for a human reader.
typedef struct {
    bool  active;         // this zone participates in the current run/config -- an inactive
                           // zone is skipped by every check below, matching
                           // profile_exec_zone_status_t's own "only zones[i] with .active ==
                           // true participated" convention.
    bool  actual_valid;   // false = no trustworthy reading (thermocouple fault, stale, absent)
    float actual_c;       // calibration-corrected; meaningless if !actual_valid
    bool  heater_commanded; // this zone's relay(s) are currently commanded on
} ota_interlock_zone_snapshot_t;

// Everything ota_interlock_check() needs, gathered by the caller from
// profile_executor_get_status(), autotune_engine_is_active(),
// safety_link_get_status(), run_state (see run_state.h's own header comment
// on why its breadcrumb, not profile_executor's live state, is the wrong
// source for "is a profile running" -- but IS the right source for this
// specific "did a firing survive a reboot without a clean ending" check),
// and the update-in-progress mutex (ota_http_update_in_progress(), same
// file). No pointers into any live module -- a snapshot, taken once, so the
// answer this function gives cannot change out from under the caller
// mid-check.
typedef struct {
    ota_interlock_profile_state_t profile_state;
    bool  autotune_active;
    bool  safety_link_up;
    // True if run_state's boot-record breadcrumb (run_state.h) shows a
    // firing that was never cleanly ended -- RUN_STATE_PHASE_RUNNING or
    // RUN_STATE_PHASE_PAUSED, per run_state_boot_record_interrupted(). This
    // is deliberately independent of profile_state above: profile_state is
    // "is the executor doing something right now, this boot", while this
    // catches "a firing was in flight when the board last stopped, and
    // nobody has acknowledged it" -- exactly the case UPDATE_PROTOCOL.md's
    // precondition table calls out as "catches a firing that survived a
    // reboot." A board that rebooted cleanly (or has never fired) reports
    // false here regardless of profile_state.
    bool  run_state_interrupted;
    // True if some update (either processor, either direction) is already
    // under way -- the single cross-processor mutex TODO.md 9.4/9.5 both
    // reference ("this item overlaps with 9.5's 'single update mutex'
    // bullet"). Named after TODO.md's own phrasing ("refuse to start either
    // update while the OTHER is in progress"), but in this single-mutex
    // design it also covers a second attempt at the SAME processor's update
    // racing the first -- there is only ever one update in flight, not one
    // per processor, so "other" and "any" are the same question here.
    bool  other_update_in_progress;
    // UPDATE_PROTOCOL.md section 1: "Measured temperature below a
    // configured ceiling ... Default 100 C." Callers should pass
    // OTA_INTERLOCK_TEMP_CEILING_C unless/until a real config item exists.
    float temp_ceiling_c;
    // The operator has been shown the "no safety processor is watching this
    // kiln -- heaters may come on, or stay stuck on, with nothing
    // independent to cut them" warning for THIS request and chose to
    // continue. Set from an explicit per-request token (a form field or an
    // HTTP header), never from a stored preference: the acknowledgement is
    // of one action taken now, not a mode the board is left in. Ignored
    // entirely when safety_link_up is true.
    bool  operator_ack_no_safety_processor;
} ota_interlock_snapshot_t;

// The warning an operator must be shown before their acknowledgement of
// OTA_INTERLOCK_REFUSED_NEEDS_ACK means anything. Defined once, here, so the
// web dialogs (app.js) and the LCD dialog (ui_page_kiln_cfg_setup.c) cannot
// drift into describing the same risk two different ways -- the LCD includes
// this header directly; the web copy (app.js's NO_SAFETY_WARNING) is
// compared against this macro by App/test/lint_pages.js, so an edit to one
// without the other fails that check rather than shipping two different
// descriptions of the same risk.
//
// States the consequence, not the mechanism: an operator deciding whether to
// restore a config needs to know that nothing will cut the heaters if they
// stick on, not which UART is silent.
#define OTA_INTERLOCK_NO_SAFETY_WARNING                                                          \
    "The safety processor is not answering. Nothing independent is watching this kiln: the "     \
    "heaters may come on, or stay stuck on, with no second processor able to cut them. "         \
    "Continue anyway?"

#define OTA_INTERLOCK_REASON_MAX 96 // same precedent as profile_exec_status_t::fault_reason
                                     // (profile_executor.h) -- one more 96-char human-readable
                                     // "why" field in a codebase that already has several.

typedef enum {
    OTA_INTERLOCK_OK = 0,     // every precondition holds; caller may proceed to auth/transfer
    OTA_INTERLOCK_REFUSED,    // reason_out names the first unmet precondition found
    // The safety link is down and the operator has not acknowledged that.
    // Every OTHER precondition either passed or has not been reached yet.
    //
    // Split out from OTA_INTERLOCK_REFUSED (2026-08-22, owner request) so a
    // caller can tell "you cannot do this" from "you can do this if you
    // accept a specific risk, and here is the risk." Without the split, a
    // board whose safety processor is absent or dead could not be updated
    // or have its configuration restored AT ALL -- including restoring the
    // very configuration that might get the safety processor commissioned.
    // The refusal it replaced was inherited wholesale from
    // UPDATE_PROTOCOL.md's rule about firmware TRANSFERS over a marginal
    // link, which is not what a local config restore does.
    //
    // Only this precondition is overridable. The others describe a kiln
    // that is actually doing something (updating, autotuning, firing, hot)
    // where proceeding corrupts work in flight rather than merely removing
    // a supervisor; there is no acknowledgement that makes those safe, so
    // they keep returning OTA_INTERLOCK_REFUSED.
    //
    // Callers that only branch on `!= OTA_INTERLOCK_OK` keep refusing when
    // they see this, which is the safe default for any site that has not
    // been taught to offer the acknowledgement.
    OTA_INTERLOCK_REFUSED_NEEDS_ACK,
} ota_interlock_result_t;

// Checks `snap` plus `zones[0..zone_count)` against every UPDATE_PROTOCOL.md
// section 1 precondition this side (the ESP) is responsible for, and returns
// OTA_INTERLOCK_OK or OTA_INTERLOCK_REFUSED. On refusal, writes a
// human-readable, specific reason into reason_out (e.g. "zone 2 is at 340 C",
// never a generic "update failed" -- TODO.md 9.4's explicit requirement) --
// always null-terminated, truncated if reason_cap is too small.
// reason_out/reason_cap may be NULL/0 if the caller only cares about the
// pass/fail result.
//
// Checks run in this order, each one short-circuiting the rest:
//   1. other_update_in_progress -- cheapest, and orthogonal to kiln state:
//      true regardless of temperature/profile/link, so checking it first
//      never reports a stale-sounding kiln-state reason for what is really
//      "someone else is already updating".
//   2. safety_link_up -- UPDATE_PROTOCOL.md section 1: "Do not start a
//      transfer over a link that is already marginal." Checked early because
//      every zone temperature reading downstream is only as trustworthy as
//      the link that (eventually) carries the safety processor's own
//      corroborating view -- refusing here first avoids naming a specific
//      zone temperature the operator has less reason to trust anyway.
//      Returns OTA_INTERLOCK_REFUSED_NEEDS_ACK rather than
//      OTA_INTERLOCK_REFUSED, and is skipped entirely when the caller set
//      operator_ack_no_safety_processor -- in which case checks 3-7 below
//      still run in full. See that enum value's comment.
//   3. autotune_active -- an update mid-autotune abandons a test that is
//      deliberately driving a zone away from steady state.
//   4. profile_state RUNNING or PAUSED -- "an update mid-firing abandons a
//      hot kiln" (UPDATE_PROTOCOL.md section 1), and PAUSED is still a
//      firing the operator can resume, not an ended one.
//   5. run_state_interrupted -- catches the reboot case profile_state alone
//      cannot see (a fresh boot's executor is always IDLE, per
//      profile_executor.h's "the executor always comes up IDLE" guarantee,
//      even if run_state's breadcrumb says the LAST boot never cleanly
//      ended).
//   6. Per active zone, heater_commanded -- "no heater commanded on, all
//      SSRs off" independent of whether a profile is driving it.
//   7. Per active zone, temperature vs. temp_ceiling_c -- "a cooling kiln is
//      still a hot kiln." A zone with !actual_valid is treated as a
//      refusal too (not skipped): this codebase's standing rule is "no
//      valid data -> the safe default", and the safe default for "may I
//      start an update" is no, the same as it is for "may I command heat"
//      elsewhere in this firmware.
ota_interlock_result_t ota_interlock_check(const ota_interlock_snapshot_t *snap,
                                            const ota_interlock_zone_snapshot_t *zones,
                                            uint8_t zone_count, char *reason_out,
                                            size_t reason_cap);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_OTA_INTERLOCK_H
