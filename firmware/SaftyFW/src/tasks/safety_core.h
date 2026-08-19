// safety_core.h -- assembles snapshots, will call safety_guards_tick() (Phase
// 4's pure evaluator, already ported in src/safety_guards.c but NOT yet wired
// in here -- that integration is separate follow-on work, not this phase's),
// and commands relay_owner.
//
// THE ONE RULE THAT MATTERS (docs/ARCHITECTURE.md section 2): this file, and
// safety_core.c, must never #include a link or uart header. Checked by
// tools/check_isolation.ps1 as well as by inspection -- see that script.
#ifndef SAFTYFW_TASKS_SAFETY_CORE_H
#define SAFTYFW_TASKS_SAFETY_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "safety_guards.h" // safety_trip_t -- not link/uart-shaped, fine for check_isolation.ps1

#ifdef __cplusplus
extern "C" {
#endif

// Creates safety_core at SAFTYFW_PRIO_SAFETY_CORE, pinned to
// SAFTYFW_CORE_TRIP_PATH. Returns false if task creation failed.
bool safety_core_start(void);

// Telemetry pull for link_task's status frame (LINK_PROTOCOL.md's Frame A):
// "K4 energized" and "heating currently permitted". link_task.c is
// structurally forbidden from including relay_owner.h or naming the relay at
// all (tools/check_isolation.ps1), so this is the one legal channel -- a
// plain data pull through safety_core, which already legitimately depends on
// relay_owner (it commands it), not a new coupling. relay_energized comes
// from relay_owner_is_energized() (is GPIO6 actually high right now);
// heating_enabled from relay_owner_get_state() == ARMED (would a genuine
// energize command be honoured right now) -- matching safety_link.h's own
// documented bit semantics ("heating currently permitted", not "currently
// heating"). Safe to call from any task.
void safety_core_get_output_status(bool *out_relay_energized, bool *out_heating_enabled);

// Diagnostics pull for link_task's Frame B (SAFETY_CMD_DIAG,
// CommonFW/docs/LINK_PROTOCOL.md section 6, TODO.md Phase 8). Same channel
// pattern as safety_core_get_output_status() above -- link_task cannot see
// s_guard_state (safety_core.c's local static) or relay_owner's state
// directly without violating the isolation rule, so this is the legal path
// for both.
//
//   out_trip_reason: the latched guard's reason, SAFETY_TRIP_NONE if nothing
//     has tripped. Real, not degraded -- safety_guards_tick() only ever
//     tracks one reason for the whole module in this build (5 of 13 guards
//     implemented, see safety_guards.h), so there is exactly one to report.
//
//   out_warn_active: true if S5 or S12 is currently in its WARN state
//     (safety_guard_state_t.s5_warn / .s12_warn) without having tripped.
//     This is an OR of the only two guards in this build that have a WARN
//     concept at all -- it cannot say *which* guard is warning, because
//     safety_guards.c has no per-guard identity to report beyond that,
//     honesty preferred over inventing one (see link_frame.h's warn_mask
//     doc comment for how the caller turns this into the wire field).
//
//   out_diag_state: DIAG byte 24 (LINK_PROTOCOL.md: 0 init/1 grace/2 armed/
//     3 warn/4 tripped), computed here because only safety_core can see both
//     relay_owner_get_state() and the guard warn flags needed to tell
//     "ARMED" apart from "ARMED but a guard is warning" -- relay_owner_state_t
//     itself has no WARN state (INIT/GRACE/ARMED/TRIPPED only; see
//     relay_owner.h), so this function is what maps the two together rather
//     than link_task or relay_owner trying to.
//
// Any output pointer may be NULL if the caller does not need it. Safe to
// call from any task.
void safety_core_get_diag_status(safety_trip_t *out_trip_reason, bool *out_warn_active,
                                  uint8_t *out_diag_state);

// Explicit operator-acknowledged clear -- the only way out of a latched trip
// (SAFETY_MODEL.md section 2). Re-evaluates against a fresh input snapshot
// before honoring the clear (safety_guards_try_clear(), see its own doc
// comment in safety_guards.h for exactly what this does and does not catch
// -- in short, an unwindowed guard like S7/S6a/S6b's hard backstop is caught
// reliably if still active, a graduated guard like S1/S2/S3/S5/S9/S11/S12/S13
// is not guaranteed to retrip on this single retick even if its underlying
// condition persists). Returns true if the trip is now clear (or was already
// not tripped), false if the clear was refused because the guard retripped.
//
// NOT YET CALLED FROM ANYWHERE in this build -- same honesty as
// relay_owner_clear_trip()'s own header comment: the real trigger is Phase
// 7's link_task CLEAR_TRIP (0x0A) command from the ESP/GUI, which does not
// exist yet. This function is the policy/API half of that; wiring an actual
// caller is a separate, later pass.
bool safety_core_request_clear_trip(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_SAFETY_CORE_H
